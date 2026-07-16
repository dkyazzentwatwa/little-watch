#include "BatteryAdapter.h"

// TODO(task-5): AXP2101 bring-up via XPowersLib (addr 0x34, Wire 15/14) —
// enableBattDetection + voltage measures, percent/charging/vbus accessors.

bool BatteryAdapter::begin() {
  ready_ = false;
  return ready_;
}

int BatteryAdapter::percent() {
  return -1;
}

bool BatteryAdapter::charging() {
  return false;
}

bool BatteryAdapter::vbusPresent() {
  return false;
}

bool BatteryAdapter::batteryPresent() {
  return false;
}
