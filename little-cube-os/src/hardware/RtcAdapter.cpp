#include "RtcAdapter.h"

// TODO(task-11): SensorLib PCF85063 bring-up on Wire (SDA 15 / SCL 14).

bool RtcAdapter::begin() {
  ready_ = false;
  return ready_;
}

bool RtcAdapter::now(struct tm& out) {
  (void)out;
  return false;
}

bool RtcAdapter::set(const struct tm& value) {
  (void)value;
  return false;
}
