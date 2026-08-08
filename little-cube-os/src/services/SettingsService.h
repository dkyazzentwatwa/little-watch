#pragma once

#include <Arduino.h>

#include "../board_config.h"

// The assistant has two intentionally distinct network paths. Api preserves
// the existing record -> STT -> Responses -> TTS flow; Realtime uses the
// speech-to-speech WebSocket API. Keep the stored representation explicit so
// corrupt or future NVS values can safely fall back to Api.
enum class AssistantBackend : uint8_t {
  Api = 0,
  Realtime = 1,
};

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

  // Microphone capture chain. These were runtime-only and silently reset on
  // every boot; they persist now so the Sound screen and the `recordings`
  // serial family agree across a restart.
  uint8_t micGain() const { return micGain_; }        // 0..7, 6 dB per step
  void setMicGain(uint8_t value);
  bool recordNormalize() const { return recordNormalize_; }
  void setRecordNormalize(bool value);
  bool recordGate() const { return recordGate_; }
  void setRecordGate(bool value);

  // OpenAI API key for the Assistant (docs/superpowers/specs/
  // 2026-07-25-voice-assistant-design.md). NVS only — never SD, never
  // printed, never echoed over serial.
  String openaiKey() const { return openaiKey_; }
  void setOpenaiKey(const String& value);
  bool hasOpenaiKey() const { return openaiKey_.length() > 0; }

  AssistantBackend assistantBackend() const { return assistantBackend_; }
  void setAssistantBackend(AssistantBackend value);

  // Active UI palette index (0..theme::kThemeCount-1); see ui/Theme.h.
  uint8_t themeIndex() const { return themeIndex_; }
  void setThemeIndex(uint8_t value);

  // Selected clock face (0..clockfaces::kFaceCount-1); see ui/ClockFaces.h.
  uint8_t clockFace() const { return clockFace_; }
  void setClockFace(uint8_t value);

  // How the video player lays itself out. Defaults to Upright: the cube is
  // worn on a wrist more often than it is turned sideways in the hand, and a
  // wrist cannot be rotated to meet the picture. Sampled once per playback by
  // VideoPlayer, so changing it mid-video takes effect on the next start.
  VideoOrientation videoOrientation() const { return videoOrientation_; }
  void setVideoOrientation(VideoOrientation value);

  // Bedtime window (spec §37): a nightly brightness ceiling, never a floor.
  // Times are minutes since midnight and the window wraps midnight whenever
  // start > end (22:00 -> 07:00 is the default).
  bool bedtimeEnabled() const { return bedtimeEnabled_; }
  void setBedtimeEnabled(bool value);

  uint16_t bedtimeStartMin() const { return bedtimeStartMin_; }
  uint16_t bedtimeEndMin() const { return bedtimeEndMin_; }
  void setBedtimeWindow(uint16_t startMin, uint16_t endMin);

  uint8_t bedtimeBrightness() const { return bedtimeBrightness_; }
  void setBedtimeBrightness(uint8_t value);

 private:
  void load();

  uint8_t brightness_ = DEFAULT_BRIGHTNESS;
  uint32_t screenTimeoutSec_ = 60;
  bool alwaysOn_ = false;
  String deviceName_ = "LittleCube";
  String timezone_ = "UTC0";
  String weatherCity_;
  float weatherLat_ = 0.0f;
  float weatherLon_ = 0.0f;
  uint8_t volumePercent_ = 70;
  uint8_t micGain_ = 7;            // 42 dB — the level voice capture needs
  bool recordNormalize_ = true;
  bool recordGate_ = true;
  String openaiKey_;
  AssistantBackend assistantBackend_ = AssistantBackend::Api;
  uint8_t themeIndex_ = 0;
  uint8_t clockFace_ = 0;
  VideoOrientation videoOrientation_ = VideoOrientation::Upright;
  bool bedtimeEnabled_ = false;
  uint16_t bedtimeStartMin_ = 22 * 60;
  uint16_t bedtimeEndMin_ = 7 * 60;
  uint8_t bedtimeBrightness_ = 40;
};
