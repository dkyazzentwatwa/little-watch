#pragma once

#include <Arduino.h>
#include <time.h>

class RtcAdapter;
class SettingsService;
class EventBus;
struct SystemState;

// Time keeping: PCF85063 RTC is the source of truth; SNTP corrects it when
// Wi-Fi is online. Timezone comes from settings as a POSIX TZ string.
class TimeService {
 public:
  void begin(RtcAdapter* rtc, SettingsService* settings, EventBus* events, SystemState* state);
  void update(uint32_t deltaMs);

  bool now(struct tm& out);
  bool valid() const { return valid_; }

  // Manual set (serial `time set ...`); writes the RTC.
  bool setManual(const struct tm& value);

  // Formats "HH:MM" into out (at least 6 bytes).
  void formatHhMm(char* out, size_t outSize);

 private:
  RtcAdapter* rtc_ = nullptr;
  SettingsService* settings_ = nullptr;
  EventBus* events_ = nullptr;
  SystemState* systemState_ = nullptr;
  struct tm cached_ = {};
  uint32_t rtcAccumMs_ = 0;
  uint32_t sntpAccumMs_ = 0;
  bool sntpStarted_ = false;
  bool synced_ = false;
  bool valid_ = false;
};
