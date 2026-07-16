#include "Kernel.h"

#include <Arduino.h>

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
#include "../services/NotesService.h"
#include "../services/OpenMeteoWeatherService.h"
#include "../services/ProvisioningService.h"
#include "../services/RecorderService.h"
#include "../services/SettingsService.h"
#include "../services/TimeService.h"
#include "../services/WifiService.h"
#include "../storage/SdStorage.h"
#include "../ui/AmoledProtection.h"
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
NotesService notesService;
RecorderService recorderService;

SerialCommandService serialCommandService;

EventBus eventBus;
AppRouter appRouter;
SystemState systemState;
Services services;

AmoledProtection amoledProtection;

uint32_t lastLoopMs = 0;
uint32_t statusAccumMs = 0;

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

void onSystemEvent(SystemEvent event, void* /*context*/) {
  switch (event) {
    case SystemEvent::SdMounted:
      // Fresh card (or remount): make sure the content tree exists before
      // any service touches it.
      sdStorage.ensureTree();
      break;
    default:
      break;
  }
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
  services.notes = &notesService;
  services.recorder = &recorderService;
  services.serial = &serialCommandService;
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
  settingsService.begin();
  displayAdapter.begin();
  displayAdapter.setBrightness(settingsService.brightness());
  displayAdapter.splash(FIRMWARE_NAME, FIRMWARE_VERSION);
  inputAdapter.begin();

  // Storage wiring and the event subscription must precede the SD begin:
  // a successful boot mount publishes SdMounted synchronously.
  sdStorage.begin(&sdCardAdapter);
  eventBus.subscribe(onSystemEvent, nullptr);
  sdCardAdapter.begin(&eventBus);

  rtcAdapter.begin();
  batteryAdapter.begin();
  audioAdapter.begin();

  wifiService.begin(&eventBus, &systemState);
  provisioningService.begin(&wifiService, &settingsService);
  timeService.begin(&rtcAdapter, &settingsService, &eventBus, &systemState);
  weatherService.attach(&settingsService, &wifiService, &eventBus);
  weatherService.begin();
  notesService.begin(&sdStorage);
  recorderService.begin(&audioAdapter, &sdCardAdapter, &sdStorage, &eventBus, &systemState);

  serialCommandService.begin(&services);
  amoledProtection.begin(&displayAdapter, &settingsService);

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

  serialCommandService.update();

  InputEvent event;
  while (inputAdapter.poll(event)) {
    amoledProtection.onActivity();
    if (!amoledProtection.screenOff()) {
      appRouter.handleInput(event);
    }
  }

  statusAccumMs += deltaMs;
  if (statusAccumMs >= 1000) {
    statusAccumMs = 0;
    refreshSystemState();
  }

  wifiService.update(deltaMs);
  provisioningService.update(deltaMs);
  timeService.update(deltaMs);
  weatherService.update(deltaMs);
  sdCardAdapter.update(deltaMs);
  audioAdapter.update(deltaMs);
  recorderService.update(deltaMs);
  amoledProtection.update(deltaMs);

  appRouter.update(deltaMs);
  appRouter.render();
  displayAdapter.present();

  delay(1);
}
