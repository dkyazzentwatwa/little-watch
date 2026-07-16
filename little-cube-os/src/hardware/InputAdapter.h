#pragma once

#include <Arduino.h>

#include "../core/InputAction.h"
#include "PowerButton.h"

class Arduino_IIC;

// Semantic input source: FT3168 touch (tap / double-tap / long-press /
// swipes, synthesized in software) plus the BOOT button (short = Back,
// long = Home). Emits InputEvent; apps never see raw coordinates except
// through the event's x/y.
class InputAdapter {
 public:
  bool begin();
  bool touchReady() const { return touchReady_; }

  // Non-blocking; returns true when a semantic event is ready this tick.
  bool poll(InputEvent& out);

 private:
  PowerButton button_;
  bool touchReady_ = false;
};
