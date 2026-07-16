#include "SettingsService.h"

// TODO(task-6): persist every setter through Preferences("littlecube") and
// load() from NVS with defaults.

void SettingsService::begin() {
  load();
}

void SettingsService::load() {}

void SettingsService::setBrightness(uint8_t value) {
  brightness_ = value;
}

void SettingsService::setScreenTimeoutSec(uint32_t value) {
  screenTimeoutSec_ = value;
}

void SettingsService::setAlwaysOn(bool value) {
  alwaysOn_ = value;
}

void SettingsService::setDeviceName(const String& value) {
  deviceName_ = value;
}

void SettingsService::setTimezone(const String& value) {
  timezone_ = value;
}

void SettingsService::setWeatherLocation(const String& city, float lat, float lon) {
  weatherCity_ = city;
  weatherLat_ = lat;
  weatherLon_ = lon;
}

void SettingsService::setVolumePercent(uint8_t value) {
  volumePercent_ = value > 100 ? 100 : value;
}
