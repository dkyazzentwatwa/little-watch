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

  // Call after SettingsService::setTimezone(). The RTC stores LOCAL wall
  // time, so a new zone makes its contents wrong immediately: this re-applies
  // the POSIX TZ, rewrites the RTC from the system clock when that clock is
  // already good, and re-runs the SNTP cycle. Without it a timezone change
  // does nothing until the next power cycle.
  void onTimezoneChanged();

  // Formats "HH:MM" into out (at least 6 bytes).
  void formatHhMm(char* out, size_t outSize);

 private:
  void applyTimezone();
  // Re-derives local wall time from the system clock (UTC + TZ) and writes it
  // to the RTC. Non-blocking: getLocalTime with a 0 ms timeout.
  bool syncRtcFromSystemClock(const char* why);

  RtcAdapter* rtc_ = nullptr;
  SettingsService* settings_ = nullptr;
  EventBus* events_ = nullptr;
  SystemState* systemState_ = nullptr;
  struct tm cached_ = {};
  String appliedTz_;
  uint32_t rtcAccumMs_ = 0;
  uint32_t sntpAccumMs_ = 0;
  uint32_t resyncAccumMs_ = 0;
  bool sntpStarted_ = false;
  bool synced_ = false;
  bool valid_ = false;
};
