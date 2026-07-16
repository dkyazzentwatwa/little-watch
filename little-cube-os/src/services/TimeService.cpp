#include "TimeService.h"

#include <string.h>
#include <time.h>

#include "../core/EventBus.h"
#include "../core/SystemState.h"
#include "../hardware/RtcAdapter.h"
#include "SettingsService.h"

// The PCF85063 is the source of truth and holds LOCAL wall time — the cube
// keeps correct time with no network. When the internet becomes reachable,
// SNTP (with the configured POSIX TZ) corrects the system clock once and
// the result is written back into the RTC.

namespace {
constexpr uint32_t kRtcRefreshMs = 1000;
constexpr uint32_t kSntpPollMs = 2000;
constexpr int kPlausibleYear = 2024;
}  // namespace

void TimeService::begin(RtcAdapter* rtc, SettingsService* settings, EventBus* events,
                        SystemState* state) {
  rtc_ = rtc;
  settings_ = settings;
  events_ = events;
  systemState_ = state;

  struct tm t;
  if (rtc_ != nullptr && rtc_->now(t) && (t.tm_year + 1900) >= kPlausibleYear) {
    valid_ = true;
    cached_ = t;
  }
}

void TimeService::update(uint32_t deltaMs) {
  rtcAccumMs_ += deltaMs;
  if (rtcAccumMs_ >= kRtcRefreshMs) {
    rtcAccumMs_ = 0;
    struct tm t;
    if (rtc_ != nullptr && rtc_->now(t)) {
      cached_ = t;
      if ((t.tm_year + 1900) >= kPlausibleYear) {
        valid_ = true;
      }
    }
  }

  // SNTP lifecycle: start once the internet is confirmed, then poll until
  // the system clock is plausible; write it into the RTC exactly once per
  // sync session.
  const bool online = systemState_ != nullptr && systemState_->internet;
  if (online && !sntpStarted_) {
    const String tz = settings_ != nullptr ? settings_->timezone() : String("UTC0");
    configTzTime(tz.c_str(), "pool.ntp.org", "time.nist.gov");
    sntpStarted_ = true;
    Serial.printf("[time] SNTP started (tz %s)\n", tz.c_str());
  }
  if (sntpStarted_ && !synced_) {
    sntpAccumMs_ += deltaMs;
    if (sntpAccumMs_ >= kSntpPollMs) {
      sntpAccumMs_ = 0;
      struct tm t;
      if (getLocalTime(&t, 0) && (t.tm_year + 1900) >= kPlausibleYear) {
        synced_ = true;
        valid_ = true;
        cached_ = t;
        if (rtc_ != nullptr) {
          rtc_->set(t);
        }
        Serial.printf("[time] synced %04d-%02d-%02d %02d:%02d\n", t.tm_year + 1900,
                      t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
        if (events_ != nullptr) {
          events_->publish(SystemEvent::TimeSynced);
        }
      }
    }
  }
}

bool TimeService::now(struct tm& out) {
  if (!valid_ && rtc_ != nullptr) {
    // Even an unset RTC ticks; expose it so `time set` feedback is visible.
    return rtc_->now(out);
  }
  out = cached_;
  return valid_ || rtc_ != nullptr;
}

bool TimeService::setManual(const struct tm& value) {
  if (rtc_ == nullptr || !rtc_->set(value)) {
    return false;
  }
  cached_ = value;
  valid_ = (value.tm_year + 1900) >= kPlausibleYear;
  return true;
}

void TimeService::formatHhMm(char* out, size_t outSize) {
  if (out == nullptr || outSize < 6) {
    return;
  }
  if (!valid_) {
    strncpy(out, "--:--", outSize - 1);
    out[outSize - 1] = '\0';
    return;
  }
  snprintf(out, outSize, "%02d:%02d", cached_.tm_hour, cached_.tm_min);
}
