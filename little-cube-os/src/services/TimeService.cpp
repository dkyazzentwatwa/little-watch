#include "TimeService.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/EventBus.h"
#include "../core/SystemState.h"
#include "../hardware/RtcAdapter.h"
#include "SettingsService.h"

// The PCF85063 is the source of truth and holds LOCAL wall time — the cube
// keeps correct time with no network. When the internet becomes reachable,
// SNTP (with the configured POSIX TZ) corrects the system clock and the
// result is written back into the RTC.
//
// "Written back" happens repeatedly, not once. Because the RTC stores LOCAL
// time, every change to the offset between UTC and local — a new timezone, or
// a DST boundary arriving — makes its contents wrong, and a one-shot sync
// would leave the cube an hour out until the next power cycle.

namespace {
constexpr uint32_t kRtcRefreshMs = 1000;
constexpr uint32_t kSntpPollMs = 2000;
// Hourly. DST transitions land on the hour, so this bounds the error at an
// hour in the worst case and costs one non-blocking clock read per hour.
constexpr uint32_t kRtcResyncMs = 3600000;
constexpr int kPlausibleYear = 2024;

// EventBus handlers are plain function pointers with a context, and they run
// synchronously on the publisher's call — so this does the smallest thing
// that can possibly work: re-apply the zone. onTimezoneChanged() touches the
// TZ environment, the (non-blocking) system clock and the RTC over I2C, and
// nothing else. No filesystem, no network, no allocation of consequence.
void onSettingsChangedEvent(SystemEvent event, void* context) {
  if (event != SystemEvent::SettingsChanged || context == nullptr) {
    return;
  }
  static_cast<TimeService*>(context)->onTimezoneChanged();
}
}  // namespace

void TimeService::begin(RtcAdapter* rtc, SettingsService* settings, EventBus* events,
                        SystemState* state) {
  rtc_ = rtc;
  settings_ = settings;
  events_ = events;
  systemState_ = state;

  // Apply the zone before anything converts a timestamp.
  applyTimezone();

  // Whoever changes the timezone publishes SettingsChanged; this is the only
  // subscriber, and it is what makes a new zone land without a reboot. The
  // hourly re-derive in update() stays as the safety net for a writer that
  // forgets to publish.
  if (events_ != nullptr) {
    events_->subscribe(onSettingsChangedEvent, this);
  }

  struct tm t;
  if (rtc_ != nullptr && rtc_->now(t) && (t.tm_year + 1900) >= kPlausibleYear) {
    valid_ = true;
    cached_ = t;
  }
}

void TimeService::applyTimezone() {
  const String tz = settings_ != nullptr ? settings_->timezone() : String("UTC0");
  if (tz.length() == 0) {
    return;
  }
  // setenv + tzset rather than configTzTime: this has to work offline too,
  // and configTzTime would restart the SNTP client as a side effect.
  setenv("TZ", tz.c_str(), 1);
  tzset();
  appliedTz_ = tz;
}

bool TimeService::syncRtcFromSystemClock(const char* why) {
  struct tm t;
  // 0 ms timeout: never blocks the main loop, just reports whether the system
  // clock is set yet.
  if (!getLocalTime(&t, 0) || (t.tm_year + 1900) < kPlausibleYear) {
    return false;
  }
  cached_ = t;
  valid_ = true;
  if (rtc_ != nullptr) {
    rtc_->set(t);
  }
  // Logged every time, including the hourly no-op: one line an hour is what
  // makes "the clock is an hour out" diagnosable over serial.
  Serial.printf("[time] RTC set from system clock (%s): %04d-%02d-%02d %02d:%02d\n", why,
                t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
  return true;
}

void TimeService::onTimezoneChanged() {
  applyTimezone();
  Serial.printf("[time] timezone is now %s\n", appliedTz_.c_str());
  // If the system clock is already good this corrects the display instantly;
  // otherwise the SNTP cycle below picks it up when the cube gets online.
  syncRtcFromSystemClock("timezone change");
  sntpStarted_ = false;
  synced_ = false;
  sntpAccumMs_ = 0;
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

  // SNTP lifecycle: start once the internet is confirmed, then poll until the
  // system clock is plausible. That first hit ends the poll; the RTC is kept
  // in step afterwards by the hourly re-derive below.
  const bool online = systemState_ != nullptr && systemState_->internet;
  if (online && !sntpStarted_) {
    applyTimezone();
    configTzTime(appliedTz_.c_str(), "pool.ntp.org", "time.nist.gov");
    sntpStarted_ = true;
    Serial.printf("[time] SNTP started (tz %s)\n", appliedTz_.c_str());
  }
  if (sntpStarted_ && !synced_) {
    sntpAccumMs_ += deltaMs;
    if (sntpAccumMs_ >= kSntpPollMs) {
      sntpAccumMs_ = 0;
      if (syncRtcFromSystemClock("sntp")) {
        synced_ = true;
        if (events_ != nullptr) {
          events_->publish(SystemEvent::TimeSynced);
        }
      }
    }
  }

  // Keep the RTC in step with the system clock for as long as the cube runs.
  // This is what makes a DST boundary land, and it is the safety net for a
  // timezone changed without going through onTimezoneChanged().
  resyncAccumMs_ += deltaMs;
  if (resyncAccumMs_ >= kRtcResyncMs) {
    resyncAccumMs_ = 0;
    if (settings_ != nullptr && !appliedTz_.equals(settings_->timezone())) {
      applyTimezone();
      Serial.printf("[time] timezone changed underneath us; now %s\n", appliedTz_.c_str());
    }
    syncRtcFromSystemClock("hourly");
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
