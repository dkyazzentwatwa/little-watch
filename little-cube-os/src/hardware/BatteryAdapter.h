#pragma once

#include <Arduino.h>

// AXP2101 PMU (XPowersLib) — battery percent, charge state, VBUS presence.
// Degrades gracefully: if the PMU is absent, percent() returns -1 and the
// status bar simply omits the battery glyph.
class BatteryAdapter {
 public:
  bool begin();
  bool ready() const { return ready_; }

  int percent();
  bool charging();
  bool vbusPresent();
  bool batteryPresent();

 private:
  bool ready_ = false;
};
