#include "SettingsService.h"

#include <Preferences.h>

#include "../board_config.h"

// Typed settings persisted in NVS. Every setter writes through immediately —
// settings changes are rare and NVS wear is negligible at this rate; write
// through keeps unexpected power loss harmless (spec §43).

namespace {
Preferences prefs;

constexpr const char* kKeyBrightness = "bright";
constexpr const char* kKeyScreenTimeout = "scrTimeout";
constexpr const char* kKeyAlwaysOn = "alwaysOn";
constexpr const char* kKeyDeviceName = "devName";
constexpr const char* kKeyTimezone = "tz";
constexpr const char* kKeyWeatherCity = "wxCity";
constexpr const char* kKeyWeatherLat = "wxLat";
constexpr const char* kKeyWeatherLon = "wxLon";
constexpr const char* kKeyVolume = "volume";
}  // namespace

void SettingsService::begin() {
  prefs.begin(PREF_NAMESPACE, false);
  load();
}

void SettingsService::load() {
  brightness_ = prefs.getUChar(kKeyBrightness, DEFAULT_BRIGHTNESS);
  screenTimeoutSec_ = prefs.getUInt(kKeyScreenTimeout, 60);
  alwaysOn_ = prefs.getBool(kKeyAlwaysOn, false);
  deviceName_ = prefs.getString(kKeyDeviceName, "LittleCube");
  timezone_ = prefs.getString(kKeyTimezone, "UTC0");
  weatherCity_ = prefs.getString(kKeyWeatherCity, "");
  weatherLat_ = prefs.getFloat(kKeyWeatherLat, 0.0f);
  weatherLon_ = prefs.getFloat(kKeyWeatherLon, 0.0f);
  volumePercent_ = prefs.getUChar(kKeyVolume, 70);
}

void SettingsService::setBrightness(uint8_t value) {
  brightness_ = value;
  prefs.putUChar(kKeyBrightness, value);
}

void SettingsService::setScreenTimeoutSec(uint32_t value) {
  screenTimeoutSec_ = value;
  prefs.putUInt(kKeyScreenTimeout, value);
}

void SettingsService::setAlwaysOn(bool value) {
  alwaysOn_ = value;
  prefs.putBool(kKeyAlwaysOn, value);
}

void SettingsService::setDeviceName(const String& value) {
  deviceName_ = value;
  prefs.putString(kKeyDeviceName, value);
}

void SettingsService::setTimezone(const String& value) {
  timezone_ = value;
  prefs.putString(kKeyTimezone, value);
}

void SettingsService::setWeatherLocation(const String& city, float lat, float lon) {
  weatherCity_ = city;
  weatherLat_ = lat;
  weatherLon_ = lon;
  prefs.putString(kKeyWeatherCity, city);
  prefs.putFloat(kKeyWeatherLat, lat);
  prefs.putFloat(kKeyWeatherLon, lon);
}

void SettingsService::setVolumePercent(uint8_t value) {
  volumePercent_ = value > 100 ? 100 : value;
  prefs.putUChar(kKeyVolume, volumePercent_);
}
