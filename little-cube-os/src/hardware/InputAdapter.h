#pragma once

#include <Arduino.h>

#include "../core/InputAction.h"
#include "PowerButton.h"

// Semantic input source (spec §9): FT3168 touch plus the BOOT button.
// Gestures are synthesized in software with thresholds proven on this
// board (board_config.h): swipe when movement exceeds SWIPE_THRESHOLD_PX
// on the dominant axis; LongPress fires *while held* at LONG_PRESS_MS if
// the finger hasn't moved; a release under both thresholds is a Tap, or a
// DoubleTap when it lands within DOUBLE_TAP_WINDOW_MS of the previous tap
// (the first tap is still delivered — consumers of DoubleTap must treat a
// preceding Tap as harmless). BOOT: short = Back, long = Home, so every
// gesture has a hardware fallback.
class InputAdapter {
 public:
  bool begin();
  bool touchReady() const { return touchReady_; }

  // Non-blocking; returns true when a semantic event is ready this tick.
  bool poll(InputEvent& out);

 private:
  bool readTouch(uint16_t& x, uint16_t& y);
  bool pollTouch(InputEvent& out);
  static InputEvent makeEvent(InputAction action, int16_t x, int16_t y);

  PowerButton button_;
  bool touchReady_ = false;

  bool touchWasDown_ = false;
  bool longFired_ = false;
  uint16_t startX_ = 0;
  uint16_t startY_ = 0;
  uint16_t lastX_ = 0;
  uint16_t lastY_ = 0;
  uint32_t downSinceMs_ = 0;
  uint32_t lastTapAtMs_ = 0;
};
