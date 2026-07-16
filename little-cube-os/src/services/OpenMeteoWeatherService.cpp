#include "OpenMeteoWeatherService.h"

// TODO(task-14): geocoding + forecast fetch on a job task, ArduinoJson
// filtered parse, LittleFS cache with timestamp, offline tolerance.

void OpenMeteoWeatherService::attach(SettingsService* settings, WifiService* wifi,
                                     EventBus* events) {
  settings_ = settings;
  wifi_ = wifi;
  events_ = events;
}

void OpenMeteoWeatherService::begin() {}

void OpenMeteoWeatherService::update(uint32_t deltaMs) {
  (void)deltaMs;
}

bool OpenMeteoWeatherService::refresh() {
  return false;
}
