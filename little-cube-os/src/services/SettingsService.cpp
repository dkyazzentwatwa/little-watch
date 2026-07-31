#include "SettingsService.h"

#include <Preferences.h>

#include "../board_config.h"
#include "../ui/ClockFaces.h"
#include "../ui/Theme.h"

// Typed settings persisted in NVS. Every setter writes through immediately —
// settings changes are rare and NVS wear is negligible at this rate; write
// through keeps unexpected power loss harmless (spec §43).
//
// Nothing read back from NVS is trusted: a record written by older firmware,
// or corrupted by a power cut mid-write, must not be able to brick the device
// (spec §43, "recover from invalid settings"). Every value is clamped on the
// way in and on the way out, so the in-memory copy is always usable.

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
constexpr const char* kKeyTheme = "theme";
constexpr const char* kKeyClockFace = "clockface";
constexpr const char* kKeyOpenaiKey = "aikey";
// NVS keys are limited to 15 characters.
constexpr const char* kKeyMicGain = "micgain";
constexpr const char* kKeyRecNormalize = "recnorm";
constexpr const char* kKeyRecGate = "recgate";
// NVS keys are limited to 15 characters.
constexpr const char* kKeyBedtimeOn = "bedOn";
constexpr const char* kKeyBedtimeStart = "bedStart";
constexpr const char* kKeyBedtimeEnd = "bedEnd";
constexpr const char* kKeyBedtimeBright = "bedBright";

constexpr uint32_t kDefaultScreenTimeoutSec = 60;
constexpr uint32_t kMinScreenTimeoutSec = 5;     // below this the screen is unusable
constexpr uint32_t kMaxScreenTimeoutSec = 3600;  // an hour of idle is already "never" in spirit
constexpr size_t kMaxDeviceNameLen = 32;
constexpr size_t kMaxTimezoneLen = 48;  // POSIX TZ strings with DST rules run long
constexpr size_t kMaxCityLen = 64;
constexpr uint16_t kMinutesPerDay = 24 * 60;

// Takes int, not uint8_t: comparing a uint8_t against MAX_BRIGHTNESS (255)
// is always false and trips -Wtype-limits.
uint8_t clampBrightness(int value) {
  if (value < MIN_BRIGHTNESS) {
    return MIN_BRIGHTNESS;
  }
  return value > MAX_BRIGHTNESS ? MAX_BRIGHTNESS : static_cast<uint8_t>(value);
}

uint32_t clampTimeout(uint32_t value) {
  if (value == 0) {
    return 0;  // 0 is the "never" step in the Settings UI, not a bad value
  }
  if (value < kMinScreenTimeoutSec) {
    return kMinScreenTimeoutSec;
  }
  return value > kMaxScreenTimeoutSec ? kMaxScreenTimeoutSec : value;
}

uint8_t clampVolume(int value) {
  if (value < 0) {
    return 0;
  }
  return value > 100 ? 100 : static_cast<uint8_t>(value);
}

// Empty or absurdly long means the record is junk; fall back rather than
// carry a string that will be truncated into every snprintf downstream.
String clampText(const String& value, size_t maxLen, const char* fallback) {
  if (value.length() == 0 || value.length() > maxLen) {
    return String(fallback);
  }
  return value;
}

uint16_t clampMinutes(uint16_t value, uint16_t fallback) {
  return value < kMinutesPerDay ? value : fallback;
}
}  // namespace

void SettingsService::begin() {
  prefs.begin(PREF_NAMESPACE, false);
  load();
}

void SettingsService::setMicGain(uint8_t value) {
  micGain_ = value > 7 ? 7 : value;
  prefs.putUChar(kKeyMicGain, micGain_);
}

void SettingsService::setRecordNormalize(bool value) {
  recordNormalize_ = value;
  prefs.putBool(kKeyRecNormalize, value);
}

void SettingsService::setRecordGate(bool value) {
  recordGate_ = value;
  prefs.putBool(kKeyRecGate, value);
}

void SettingsService::setOpenaiKey(const String& value) {
  openaiKey_ = value;
  prefs.putString(kKeyOpenaiKey, openaiKey_);
}

void SettingsService::load() {
  brightness_ = clampBrightness(prefs.getUChar(kKeyBrightness, DEFAULT_BRIGHTNESS));
  screenTimeoutSec_ = clampTimeout(prefs.getUInt(kKeyScreenTimeout, kDefaultScreenTimeoutSec));
  alwaysOn_ = prefs.getBool(kKeyAlwaysOn, false);
  deviceName_ = clampText(prefs.getString(kKeyDeviceName, "LittleCube"), kMaxDeviceNameLen,
                          "LittleCube");
  timezone_ = clampText(prefs.getString(kKeyTimezone, "UTC0"), kMaxTimezoneLen, "UTC0");
  // The city may legitimately be empty (no location chosen yet), so it is
  // length-capped rather than defaulted.
  weatherCity_ = prefs.getString(kKeyWeatherCity, "");
  if (weatherCity_.length() > kMaxCityLen) {
    weatherCity_ = "";
  }
  weatherLat_ = prefs.getFloat(kKeyWeatherLat, 0.0f);
  weatherLon_ = prefs.getFloat(kKeyWeatherLon, 0.0f);
  openaiKey_ = prefs.getString(kKeyOpenaiKey, "");
  micGain_ = prefs.getUChar(kKeyMicGain, 7);
  if (micGain_ > 7) {
    micGain_ = 7;
  }
  recordNormalize_ = prefs.getBool(kKeyRecNormalize, true);
  recordGate_ = prefs.getBool(kKeyRecGate, true);
  volumePercent_ = clampVolume(prefs.getUChar(kKeyVolume, 70));
  themeIndex_ = prefs.getUChar(kKeyTheme, 0);
  if (themeIndex_ >= theme::kThemeCount) {
    themeIndex_ = 0;
  }
  // Downgrade protection lives here, not in the setter: if NVS holds a face
  // index from a firmware with more faces than this one defines, clamp to
  // the default before any caller ever observes the stale value. clockFace()
  // only ever returns an already-clamped value. Untrusted NVS data, so this
  // stays silent (unlike the setter's out-of-range branch, which is a bug).
  clockFace_ = prefs.getUChar(kKeyClockFace, 0);
  if (clockFace_ >= clockfaces::kFaceCount) {
    clockFace_ = 0;
  }
  bedtimeEnabled_ = prefs.getBool(kKeyBedtimeOn, false);
  bedtimeStartMin_ = clampMinutes(prefs.getUShort(kKeyBedtimeStart, 22 * 60), 22 * 60);
  bedtimeEndMin_ = clampMinutes(prefs.getUShort(kKeyBedtimeEnd, 7 * 60), 7 * 60);
  bedtimeBrightness_ = clampBrightness(prefs.getUChar(kKeyBedtimeBright, 40));
}

void SettingsService::setBrightness(uint8_t value) {
  brightness_ = clampBrightness(value);
  prefs.putUChar(kKeyBrightness, brightness_);
}

void SettingsService::setScreenTimeoutSec(uint32_t value) {
  screenTimeoutSec_ = clampTimeout(value);
  prefs.putUInt(kKeyScreenTimeout, screenTimeoutSec_);
}

void SettingsService::setAlwaysOn(bool value) {
  alwaysOn_ = value;
  prefs.putBool(kKeyAlwaysOn, value);
}

void SettingsService::setDeviceName(const String& value) {
  deviceName_ = clampText(value, kMaxDeviceNameLen, "LittleCube");
  prefs.putString(kKeyDeviceName, deviceName_);
}

void SettingsService::setTimezone(const String& value) {
  timezone_ = clampText(value, kMaxTimezoneLen, "UTC0");
  prefs.putString(kKeyTimezone, timezone_);
}

void SettingsService::setWeatherLocation(const String& city, float lat, float lon) {
  weatherCity_ = city.length() > kMaxCityLen ? String("") : city;
  weatherLat_ = lat;
  weatherLon_ = lon;
  prefs.putString(kKeyWeatherCity, weatherCity_);
  prefs.putFloat(kKeyWeatherLat, lat);
  prefs.putFloat(kKeyWeatherLon, lon);
}

void SettingsService::setVolumePercent(uint8_t value) {
  volumePercent_ = clampVolume(value);
  prefs.putUChar(kKeyVolume, volumePercent_);
}

void SettingsService::setThemeIndex(uint8_t value) {
  const uint8_t next = value < theme::kThemeCount ? value : 0;
  if (next == themeIndex_) {
    return;  // an idle re-set must never touch flash
  }
  themeIndex_ = next;
  prefs.putUChar(kKeyTheme, themeIndex_);
}

// The out-of-range branch here is a caller bug, not the downgrade scenario:
// load() already guarantees clockFace_ starts in range, so a value that
// needs clamping can only have come from this call's argument. Logged
// (unlike load()'s silent clamp of untrusted NVS data) so a bad caller does
// not turn into a silent mystery with no host test suite to catch it.
void SettingsService::setClockFace(uint8_t value) {
  if (value >= clockfaces::kFaceCount) {
    Serial.printf("[settings] warn: setClockFace(%u) out of range (max %u), using default\n",
                  (unsigned)value, (unsigned)(clockfaces::kFaceCount - 1));
  }
  const uint8_t next = value < clockfaces::kFaceCount ? value : 0;
  if (next == clockFace_) {
    return;  // an idle re-set must never touch flash
  }
  clockFace_ = next;
  prefs.putUChar(kKeyClockFace, clockFace_);
}

void SettingsService::setBedtimeEnabled(bool value) {
  bedtimeEnabled_ = value;
  prefs.putBool(kKeyBedtimeOn, value);
}

void SettingsService::setBedtimeWindow(uint16_t startMin, uint16_t endMin) {
  bedtimeStartMin_ = clampMinutes(startMin, bedtimeStartMin_);
  bedtimeEndMin_ = clampMinutes(endMin, bedtimeEndMin_);
  prefs.putUShort(kKeyBedtimeStart, bedtimeStartMin_);
  prefs.putUShort(kKeyBedtimeEnd, bedtimeEndMin_);
}

void SettingsService::setBedtimeBrightness(uint8_t value) {
  bedtimeBrightness_ = clampBrightness(value);
  prefs.putUChar(kKeyBedtimeBright, bedtimeBrightness_);
}
