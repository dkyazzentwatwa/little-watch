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

  // True while a finger is on the glass, including between the down edge and
  // the gesture emitted on release. AmoledProtection reads this so a resting
  // finger counts as activity and the screen does not blank under it — no
  // semantic event is produced during that span (spec §37).
  bool touchDown() const { return touchWasDown_; }

  // I2C-silent idle (mic-buzz root cause): every FT3168 poll is ~1 ms of
  // traffic on the Wire bus shared with the ES8311, and it couples into the
  // analog mic as a loud comb at the loop rate. Polling is therefore gated on
  // the touch INT line (PIN_TOUCH_INT): no I2C at all until INT reports
  // contact, continuous polls only while a touch session is in flight. If INT
  // never fires (dead wire on a unit), a slow timed fallback keeps touch
  // alive — TOUCH_INT_FALLBACK_POLL_MS. `input status` reports the mode.
  bool touchIntSeen() const { return intSeen_; }
  uint32_t touchIntEdges() const;

  // While a take runs the kernel suppresses the not-yet-proven-INT fallback
  // poll so an untouched-since-boot cube still records with a silent bus. On
  // a unit whose INT line is dead this makes takes touch-deaf (serial stop
  // still works); everywhere else the ISR keeps taps working instantly.
  void setIntFallbackSuppressed(bool suppressed) { fallbackSuppressed_ = suppressed; }

  void setDebugLog(bool on) { debugLog_ = on; }
  bool debugLog() const { return debugLog_; }

 private:
  bool readTouch(uint16_t& x, uint16_t& y);
  bool pollTouch(InputEvent& out);
  // Non-static: it consults debugLog_ before emitting the trace line.
  InputEvent makeEvent(InputAction action, int16_t x, int16_t y);

  PowerButton button_;
  bool touchReady_ = false;

  bool touchWasDown_ = false;
  bool intSeen_ = false;  // an INT edge has been observed since boot
  bool fallbackSuppressed_ = false;
  bool debugLog_ = false;
  bool longFired_ = false;
  uint16_t startX_ = 0;
  uint16_t startY_ = 0;
  uint16_t lastX_ = 0;
  uint16_t lastY_ = 0;
  uint32_t downSinceMs_ = 0;
  uint32_t lastTapAtMs_ = 0;
  uint32_t lastFallbackPollMs_ = 0;
};
