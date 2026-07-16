#pragma once

#include <Arduino.h>
#include <time.h>

class SensorPCF85063;

// PCF85063 real-time clock on the shared Wire bus. The RTC is the device's
// source of truth for time; TimeService corrects it from SNTP when online.
class RtcAdapter {
 public:
  bool begin();
  bool ready() const { return ready_; }

  bool now(struct tm& out);
  bool set(const struct tm& value);

 private:
  SensorPCF85063* rtc_ = nullptr;
  bool ready_ = false;
};
