#pragma once

#include <Arduino.h>

// Typed settings on NVS (Preferences, namespace "littlecube"). Secrets
// (Wi-Fi credentials) also live in NVS — never on the SD card — but are
// owned by WifiService, not exposed through the generic get/set surface.
class SettingsService {
 public:
  void begin();

  uint8_t brightness() const { return brightness_; }
  void setBrightness(uint8_t value);

  uint32_t screenTimeoutSec() const { return screenTimeoutSec_; }
  void setScreenTimeoutSec(uint32_t value);

  bool alwaysOn() const { return alwaysOn_; }
  void setAlwaysOn(bool value);

  String deviceName() const { return deviceName_; }
  void setDeviceName(const String& value);

  String timezone() const { return timezone_; }
  void setTimezone(const String& value);

  String weatherCity() const { return weatherCity_; }
  float weatherLat() const { return weatherLat_; }
  float weatherLon() const { return weatherLon_; }
  void setWeatherLocation(const String& city, float lat, float lon);

  uint8_t volumePercent() const { return volumePercent_; }
  void setVolumePercent(uint8_t value);

 private:
  void load();

  uint8_t brightness_ = 220;
  uint32_t screenTimeoutSec_ = 60;
  bool alwaysOn_ = false;
  String deviceName_ = "LittleCube";
  String timezone_ = "UTC0";
  String weatherCity_;
  float weatherLat_ = 0.0f;
  float weatherLon_ = 0.0f;
  uint8_t volumePercent_ = 70;
};
