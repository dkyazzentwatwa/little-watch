#pragma once

#include <Arduino.h>

class DisplayAdapter;
class SettingsService;

// AMOLED burn-in defense (spec §37): screen timeout, auto-dim, bedtime
// brightness, pixel shifting of persistent elements, configurable
// always-on. Every render reads shiftX()/shiftY() for persistent chrome.
class AmoledProtection {
 public:
  void begin(DisplayAdapter* display, SettingsService* settings);
  void update(uint32_t deltaMs);

  // Call on any user input to reset the idle clock (and wake if asleep).
  void onActivity();

  bool screenOff() const { return screenOff_; }
  int16_t shiftX() const { return shiftX_; }
  int16_t shiftY() const { return shiftY_; }

 private:
  DisplayAdapter* display_ = nullptr;
  SettingsService* settings_ = nullptr;
  uint32_t idleMs_ = 0;
  bool screenOff_ = false;
  int16_t shiftX_ = 0;
  int16_t shiftY_ = 0;
};
