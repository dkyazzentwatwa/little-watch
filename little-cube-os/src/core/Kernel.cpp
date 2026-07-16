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
  inputAdapter.begin();

  sdCardAdapter.begin(&eventBus);
  sdStorage.begin(&sdCardAdapter);

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
