#include "Kernel.h"

#include <Arduino.h>
#include <LittleFS.h>

#include "../apps/AppRegistry.h"
#include "../board_config.h"
#include "../feature_flags.h"
#include "../hardware/BatteryAdapter.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/InputAdapter.h"
#include "../hardware/RtcAdapter.h"
#include "../hardware/SdCardAdapter.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../serial/SerialCommandService.h"
#include "../services/AssistantService.h"
#include "../services/BookService.h"
#include "../services/CalendarService.h"
#include "../services/ContactsService.h"
#include "../services/MusicService.h"
#include "../services/NewsService.h"
#include "../services/NotesService.h"
#include "../services/PodcastService.h"
#include "../services/OpenMeteoWeatherService.h"
#include "../services/ProvisioningService.h"
#include "../services/RecorderService.h"
#include "../services/SettingsService.h"
#include "../services/TimeService.h"
#include "../services/WifiService.h"
#include "../storage/SdStorage.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"
#include "AppRouter.h"
#include "EventBus.h"
#include "Services.h"
#include "SystemState.h"

namespace {

DisplayAdapter displayAdapter;
InputAdapter inputAdapter;
SdCardAdapter sdCardAdapter;
RtcAdapter rtcAdapter;
BatteryAdapter batteryAdapter;
AudioAdapter audioAdapter;

SdStorage sdStorage;

SettingsService settingsService;
WifiService wifiService;
ProvisioningService provisioningService;
TimeService timeService;
OpenMeteoWeatherService weatherService;
NewsService newsService;
NotesService notesService;
CalendarService calendarService;
ContactsService contactsService;
RecorderService recorderService;
AssistantService assistantService;
BookService bookService;
MusicService musicService;
PodcastService podcastService;

SerialCommandService serialCommandService;

EventBus eventBus;
AppRouter appRouter;
SystemState systemState;
Services services;

AmoledProtection amoledProtection;

uint32_t lastLoopMs = 0;
uint32_t statusAccumMs = 0;

// Capture quieting. Root cause of the recording buzz, proven by A/B takes:
// every I2C transaction on the Wire bus couples into the ES8311 analog-mic
// path, and the historic per-loop FT3168 poll produced a 500 Hz/1 kHz comb
// 40+ dB above the floor while stretching the loop to 2 ms. Touch polling is
// now INT-gated inside InputAdapter (I2C-silent unless the glass is touched);
// while a take runs the remaining housekeeping I2C — AXP2101 battery reads,
// PCF85063 cache refresh, the unproven-INT fallback poll — pauses as well
// (`recordings quiet off` keeps it live for A/B diagnostics). The display
// stays ON during takes: panel-off captures buzzed identically, so QSPI is
// innocent, and the on-screen STOP needs the panel.
bool recordingCaptureActive = false;

// Loop-cadence evidence: sampled with micros() while a take runs, printed at
// stop. The buzz comb sat at exactly the loop rate, so this number is part of
// every capture diagnosis.
bool diagLoopArmed = false;
uint32_t diagLoopPrevUs = 0;
uint32_t diagLoopCount = 0;
uint64_t diagLoopSumUs = 0;
uint32_t diagLoopMinUs = 0;
uint32_t diagLoopMaxUs = 0;

void updateRecordingQuietCapture() {
  // Assistant question captures use the same mic path and deserve the same
  // silent bus as recorder takes.
  const bool recording = recorderService.recording() || assistantService.capturing();
  if (recording && !recordingCaptureActive) {
    recordingCaptureActive = true;
    diagLoopArmed = false;
    diagLoopCount = 0;
    diagLoopSumUs = 0;
    diagLoopMinUs = UINT32_MAX;
    diagLoopMaxUs = 0;
    inputAdapter.setIntFallbackSuppressed(recorderService.quietCapture());
    Serial.printf("[recorder] capture: display on, touch INT-gated, PMU/RTC I2C %s\n",
                  recorderService.quietCapture() ? "paused" : "LIVE (quiet off)");
  } else if (!recording && recordingCaptureActive) {
    recordingCaptureActive = false;
    inputAdapter.setIntFallbackSuppressed(false);
    systemState.version++;  // battery/clock may have moved while paused
    if (diagLoopCount > 0) {
      Serial.printf("[diag] capture loop period: n=%lu avg=%luus min=%luus max=%luus\n",
                    (unsigned long)diagLoopCount,
                    (unsigned long)(diagLoopSumUs / diagLoopCount),
                    (unsigned long)diagLoopMinUs, (unsigned long)diagLoopMaxUs);
    }
  }
}

// Refreshes the shared status snapshot about once a second; bumps
// SystemState::version only when something actually changed so UI redraw
// checks stay cheap.
void refreshSystemState() {
  SystemState& s = systemState;
  bool changed = false;

  char hhmm[6];
  timeService.formatHhMm(hhmm, sizeof(hhmm));
  if (strncmp(hhmm, s.clockHhMm, sizeof(s.clockHhMm)) != 0) {
    strncpy(s.clockHhMm, hhmm, sizeof(s.clockHhMm) - 1);
    s.clockHhMm[sizeof(s.clockHhMm) - 1] = '\0';
    changed = true;
  }
  if (timeService.valid() != s.timeValid) {
    s.timeValid = timeService.valid();
    changed = true;
  }

  if (wifiService.state() != s.wifi) {
    s.wifi = wifiService.state();
    changed = true;
  }
  if (sdCardAdapter.state() != s.sd) {
    s.sd = sdCardAdapter.state();
    changed = true;
  }
  if (recorderService.recording() != s.recording) {
    s.recording = recorderService.recording();
    changed = true;
  }
  if (audioAdapter.isPlaying() != s.playingAudio) {
    s.playingAudio = audioAdapter.isPlaying();
    changed = true;
  }

  const bool batteryPresent = batteryAdapter.batteryPresent();
  const int batteryPercent = batteryAdapter.percent();
  const bool charging = batteryAdapter.charging();
  if (batteryPresent != s.batteryPresent || batteryPercent != s.batteryPercent ||
      charging != s.charging) {
    s.batteryPresent = batteryPresent;
    s.batteryPercent = batteryPercent;
    s.charging = charging;
    changed = true;
  }

  if (changed) {
    s.version++;
  }
}

// Deferred filesystem work. EventBus::publish() is synchronous and is called
// from inside SdCardAdapter::update(), so a handler that touched the card
// would run re-entrantly and stall the frame. Handlers therefore only set
// bits here; runDeferredWork() drains at most one job per frame.
enum : uint32_t {
  kWorkEnsureTree = 1u << 0,
  kWorkRecoverNotes = 1u << 1,
  kWorkSeedWelcome = 1u << 2,
  kWorkReloadContacts = 1u << 3,
};
uint32_t pendingWork = 0;  // loop task only — see EventBus.h

void onSystemEvent(SystemEvent event, void* /*context*/) {
  switch (event) {
    case SystemEvent::SdMounted:
      // Fresh card (or remount): make sure the content tree exists before
      // any service touches it, then seed the first-boot welcome note.
      sdStorage.resetTreeCursor();
      pendingWork |= kWorkEnsureTree | kWorkRecoverNotes | kWorkSeedWelcome |
                     kWorkReloadContacts;
      break;
    case SystemEvent::SdRemoved:
      // The card is going away: drop queued work and tell the recorder to
      // let go of its file handle without touching the filesystem again.
      pendingWork = 0;
      audioAdapter.abandonRecording();
      // Both caches describe a card that is no longer there. Dropping them
      // here is pure RAM work — neither service touches the filesystem to
      // invalidate — so it is safe inside a synchronous event handler.
      calendarService.invalidate();
      contactsService.invalidate();
      break;
    case SystemEvent::WeatherUpdated:
    case SystemEvent::NewsUpdated:
      // Fresh data arrived on a background fetch. Bump the shared version so
      // the Weather/News app repaints on its next frame instead of only when
      // the user leaves and returns.
      systemState.version++;
      break;
    default:
      break;
  }
}

void runDeferredWork() {
  if (pendingWork & kWorkEnsureTree) {
    if (sdStorage.ensureTreeStep()) {
      pendingWork &= ~kWorkEnsureTree;
    }
    return;
  }
  if (pendingWork & kWorkRecoverNotes) {
    pendingWork &= ~kWorkRecoverNotes;
    notesService.recoverInterrupted();
    return;
  }
  if (pendingWork & kWorkSeedWelcome) {
    pendingWork &= ~kWorkSeedWelcome;
    notesService.seedWelcomeIfEmpty();
    return;
  }
  // Last, because reload() reads /littlecube/contacts/contacts.json and that
  // directory only exists once kWorkEnsureTree has finished walking the tree.
  if (pendingWork & kWorkReloadContacts) {
    pendingWork &= ~kWorkReloadContacts;
    contactsService.reload();
    // The calendar reads day files lazily, so it has nothing to load here —
    // it only has to forget whatever the previous card said.
    calendarService.invalidate();
  }
}

// Wired into SdCardAdapter so it can quiesce writers before unmounting,
// without hardware/ needing to know about services/.
void sdQuiesceRequest(void* /*context*/) {
  recorderService.stop();  // request; finalize lands in its update()
  audioAdapter.stopPlayback();
}

bool sdQuiesceCheck(void* /*context*/) {
  // recorderService.recording() stays true until the .partial has been
  // renamed, which is why it is checked separately from the audio tasks.
  return audioAdapter.recordIdle() && audioAdapter.playbackIdle() &&
         !recorderService.recording();
}

void wireServices() {
  services.display = &displayAdapter;
  services.input = &inputAdapter;
  services.sdCard = &sdCardAdapter;
  services.rtc = &rtcAdapter;
  services.battery = &batteryAdapter;
  services.audio = &audioAdapter;
  services.storage = &sdStorage;
  services.settings = &settingsService;
  services.wifi = &wifiService;
  services.provisioning = &provisioningService;
  services.time = &timeService;
  services.weather = &weatherService;
  services.news = &newsService;
  services.notes = &notesService;
  services.calendar = &calendarService;
  services.contacts = &contactsService;
  services.recorder = &recorderService;
  services.assistant = &assistantService;
  services.books = &bookService;
  services.music = &musicService;
  services.podcasts = &podcastService;
  services.serial = &serialCommandService;
  services.amoled = &amoledProtection;
  services.events = &eventBus;
  services.router = &appRouter;
  services.state = &systemState;
}

}  // namespace

void kernelSetup() {
  Serial.begin(SERIAL_BAUD);
  delay(50);
  Serial.printf("\n%s %s\n", FIRMWARE_NAME, FIRMWARE_VERSION);

  wireServices();

  // Boot order: settings first (brightness etc.), then display so the user
  // sees life immediately, then input, then storage and the rest. Nothing
  // here may block on missing Wi-Fi or a missing SD card.
  // Internal flash FS first (settings flags, weather cache, recovery
  // metadata). Format-on-fail keeps a corrupted partition from bricking
  // boot.
  // The partition in partitions.csv is labelled "littlefs"; LittleFS.begin()
  // defaults to looking for one called "spiffs", so the label must be passed
  // explicitly or the mount can never succeed (formatOnFail cannot conjure a
  // partition that was never found).
  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    Serial.println("[fs] LittleFS unavailable");
  }

  settingsService.begin();
  theme::applyTheme(settingsService.themeIndex());  // palette before the first pixel
  displayAdapter.begin();
  displayAdapter.setBrightness(settingsService.brightness());
  displayAdapter.splash(FIRMWARE_NAME, FIRMWARE_VERSION);
  inputAdapter.begin();

  // Audio comes up before storage: begin() creates the task gates and drives
  // the amp pin low, and the boot mount consults the quiesce hooks.
  audioAdapter.begin();
  // Capture chain from NVS. Without this the mic gain, normalize and gate all
  // silently revert to their compiled defaults on every boot, so a value set
  // from the Sound screen or over serial would not survive a restart.
  audioAdapter.setMicGain(settingsService.micGain());
  audioAdapter.setRecordNormalize(settingsService.recordNormalize());
  audioAdapter.setRecordGate(settingsService.recordGate());

  // Storage wiring and the event subscription must precede the SD begin:
  // a successful boot mount publishes SdMounted synchronously.
  sdStorage.begin(&sdCardAdapter);
  eventBus.subscribe(onSystemEvent, nullptr);
  sdCardAdapter.setQuiesceHooks(sdQuiesceRequest, sdQuiesceCheck, nullptr);
  sdCardAdapter.begin(&eventBus);

  rtcAdapter.begin();
  batteryAdapter.begin();

  wifiService.begin(&eventBus, &systemState);
  provisioningService.begin(&wifiService, &settingsService);
  timeService.begin(&rtcAdapter, &settingsService, &eventBus, &systemState);
  weatherService.attach(&settingsService, &wifiService, &eventBus);
  weatherService.begin();
  newsService.attach(&wifiService, &eventBus);
  newsService.begin();
  notesService.begin(&sdStorage);
  // Both are pull-only: they take no update(deltaMs) slot in kernelLoop() and
  // read the card only when an app or a serial command asks them to.
  // CalendarService needs the clock for "today", so it comes after
  // timeService.begin().
  calendarService.begin(&sdStorage, &timeService);
  contactsService.begin(&sdStorage);
  bookService.begin(&sdStorage);   // e-reader index; pull-only, no update() slot
  musicService.begin(&sdStorage);  // music/podcast library scan; pull-only
  podcastService.begin(&sdStorage, &wifiService, &eventBus);  // RSS/download on a task
  recorderService.begin(&audioAdapter, &sdCardAdapter, &sdStorage, &eventBus, &systemState);
  assistantService.begin(&audioAdapter, &wifiService, &settingsService, &sdStorage,
                         &sdCardAdapter, &systemState);

  serialCommandService.begin(&services);
  // From here on AmoledProtection owns the panel brightness: it needs settings
  // for the baseline, input for the resting-finger case, and time for the
  // bedtime window. The setBrightness() above is the last direct write.
  amoledProtection.begin(&displayAdapter, &settingsService, &inputAdapter, &timeService);

  registerApps(appRouter, services);
  appRouter.begin(AppId::Home);

  lastLoopMs = millis();
  Serial.println("boot: ready");
}

// setup()/loop() are defined here instead of the .ino: the sketch
// preprocessor mangles auto-generated prototypes for .ino-defined
// functions on some toolchains (see little-cube-os.ino). The Arduino core
// declares and calls them; defining them in any linked TU is equivalent.
void setup() {
  kernelSetup();
}

void loop() {
  kernelLoop();
}

void kernelLoop() {
  const uint32_t now = millis();
  const uint32_t deltaMs = now - lastLoopMs;
  lastLoopMs = now;

  // Mic-buzz evidence: the buzz comb sits at exactly the loop/tick rate, so
  // the measured loop cadence during a take is part of the proof.
  if (recordingCaptureActive) {
    const uint32_t nowUs = micros();
    if (diagLoopArmed) {
      const uint32_t d = nowUs - diagLoopPrevUs;
      diagLoopCount++;
      diagLoopSumUs += d;
      if (d < diagLoopMinUs) diagLoopMinUs = d;
      if (d > diagLoopMaxUs) diagLoopMaxUs = d;
    }
    diagLoopPrevUs = nowUs;
    diagLoopArmed = true;
  } else {
    diagLoopArmed = false;
  }

  serialCommandService.update();

  InputEvent event;
  while (inputAdapter.poll(event)) {
    // onActivity() returns the verdict instead of exposing the sleep state:
    // there is no way to ask whether the screen was dark without also clearing
    // the latch, so a wake gesture can never leak through into the app.
    if (amoledProtection.onActivity()) {
      appRouter.handleInput(event);
    }
  }

  // While a quiet capture runs the Wire bus must stay silent: battery status
  // is AXP2101 I2C and TimeService's 1 Hz cache refresh is PCF85063 I2C, both
  // on the bus shared with the ES8311. Pause them for the take; accumulators
  // resume where they left off and the restore path bumps SystemState.
  const bool quietCaptureActive =
      recordingCaptureActive && recorderService.quietCapture();

  statusAccumMs += deltaMs;
  if (statusAccumMs >= 1000) {
    statusAccumMs = 0;
    if (!quietCaptureActive) {
      refreshSystemState();
    }
  }

  wifiService.update(deltaMs);
  provisioningService.update(deltaMs);
  if (!quietCaptureActive) {
    timeService.update(deltaMs);
  }
  weatherService.update(deltaMs);
  newsService.update(deltaMs);
  podcastService.update(deltaMs);

  // Reapers before teardown, and this order is a safety rule, not style:
  // the audio adapter reaps finished tasks, the recorder then finalizes the
  // file, and only then may the card adapter decide it is safe to unmount.
  // Running the card adapter first would judge teardown against writer state
  // that is a frame stale.
  audioAdapter.update(deltaMs);
  recorderService.update(deltaMs);
  assistantService.update(deltaMs);
  updateRecordingQuietCapture();
  // Root probes and capacity walks are real SD transactions. On this board
  // they couple into the ES8311 analog-mic path, so defer them until capture
  // and the post-stop PSRAM spool write are both complete.
  if (!recorderService.recording() && !assistantService.capturing()) {
    sdCardAdapter.update(deltaMs);
  }
  runDeferredWork();

  amoledProtection.update(deltaMs);
  // Apps early-out of render() unless SystemState::version moved, so a pixel
  // shift is published as a version bump rather than as an invalidate() hook
  // on all 12 apps.
  if (amoledProtection.consumeShiftChanged()) {
    systemState.version++;
  }

  // update() keeps running while the screen is dark — recordings, Wi-Fi and
  // timers must not pause just because nobody is looking — but rendering into
  // a canvas nobody can see is pure waste. Apps never clear lastStateVersion_
  // while blanked, so the first frame after a wake is a full redraw.
  appRouter.update(deltaMs);
  // Flushing is capped at ~30 fps but this loop runs far faster, so anything
  // rendered while a flush is still outstanding is overwritten before it ever
  // reaches the panel — a full 322 KB canvas repaint thrown away each time.
  if (!amoledProtection.screenOff() && !displayAdapter.flushPending()) {
    appRouter.render();
  }
  // present() stays unconditional: it is a no-op unless the canvas is dirty,
  // and letting the last pre-blank frame drain here means a wake never starts
  // by flushing a stale one.
  displayAdapter.present();

  delay(1);
}
