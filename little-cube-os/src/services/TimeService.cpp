#include "TimeService.h"

#include <string.h>

// TODO(task-11): RTC read/sync, SNTP via configTime when online, TZ from
// settings, periodic SystemState clock refresh.

void TimeService::begin(RtcAdapter* rtc, SettingsService* settings, EventBus* events,
                        SystemState* state) {
  rtc_ = rtc;
  settings_ = settings;
  events_ = events;
  systemState_ = state;
}

void TimeService::update(uint32_t deltaMs) {
  (void)deltaMs;
}

bool TimeService::now(struct tm& out) {
  (void)out;
  return false;
}

void TimeService::formatHhMm(char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return;
  }
  strncpy(out, "--:--", outSize - 1);
  out[outSize - 1] = '\0';
}
