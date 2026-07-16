#include "RtcAdapter.h"

#include <Wire.h>

#include <SensorPCF85063.hpp>

#include "../board_config.h"

// PCF85063 keeps LOCAL wall time (what an alarm-clock user expects); the
// timezone setting only matters for NTP correction, never for display.

namespace {
SensorPCF85063 rtc;
}

bool RtcAdapter::begin() {
  ready_ = rtc.begin(Wire, PIN_TOUCH_SDA, PIN_TOUCH_SCL);
  Serial.printf("[rtc] PCF85063 %s\n", ready_ ? "ok" : "not found");
  return ready_;
}

bool RtcAdapter::now(struct tm& out) {
  if (!ready_) {
    return false;
  }
  RTC_DateTime dt = rtc.getDateTime();
  out = {};
  out.tm_year = dt.getYear() - 1900;
  out.tm_mon = dt.getMonth() - 1;
  out.tm_mday = dt.getDay();
  out.tm_hour = dt.getHour();
  out.tm_min = dt.getMinute();
  out.tm_sec = dt.getSecond();
  return true;
}

bool RtcAdapter::set(const struct tm& value) {
  if (!ready_) {
    return false;
  }
  rtc.setDateTime(RTC_DateTime(value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
                               value.tm_hour, value.tm_min, value.tm_sec));
  return true;
}
