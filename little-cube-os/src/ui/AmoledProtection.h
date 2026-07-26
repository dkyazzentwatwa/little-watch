#pragma once

#include <Arduino.h>

class DisplayAdapter;
class SettingsService;
class InputAdapter;
class TimeService;

// AMOLED burn-in defense (spec §37). Three states — Active / Dim / Off —
// driven purely by the idle clock; always-on, the bedtime window and pixel
// shift are modifiers layered on top, not states of their own.
//
// This is the SOLE caller of DisplayAdapter::setBrightness() after boot. Apps
// change SettingsService::setBrightness() and the panel follows within a
// frame; a second writer would fight this one over every dim and blank
// transition and win at random.
//
// Persistent chrome must offset by shiftX()/shiftY() every render.
class AmoledProtection {
 public:
  void begin(DisplayAdapter* display, SettingsService* settings, InputAdapter* input,
             TimeService* time);
  void update(uint32_t deltaMs);

  // Call on every input event. Returns true when the event should reach the
  // app, false when it was consumed purely to wake a blanked screen. The
  // verdict is returned rather than the sleep state exposed for the caller to
  // test first: that makes it impossible to observe the latch without also
  // clearing it, which is what let wake taps leak through before.
  bool onActivity();

  // Diagnostics only — never branch the input drain on this. By the time it
  // can be read, onActivity() has already decided.
  bool screenOff() const { return state_ == State::Off; }

  int16_t shiftX() const { return shiftX_; }
  int16_t shiftY() const { return shiftY_; }

  // True once per shift step. Apps early-out of render() unless
  // SystemState::version moved, so the kernel turns this into a version bump
  // and reuses that documented dirty check instead of teaching all 12 apps a
  // new invalidate() hook.
  bool consumeShiftChanged();

 private:
  enum class State : uint8_t { Active, Dim, Off };

  void wake(bool swallowNextEvent);
  void advanceShift();
  void applyBrightness();
  bool insideBedtimeWindow();

  DisplayAdapter* display_ = nullptr;
  SettingsService* settings_ = nullptr;
  InputAdapter* input_ = nullptr;
  TimeService* time_ = nullptr;

  State state_ = State::Active;
  uint32_t idleMs_ = 0;
  // 0xFF is a sentinel, not a brightness: it forces the first update() to
  // write through even if the target happens to equal the boot value.
  uint8_t lastApplied_ = 0xFF;

  // Armed by a wake so the gesture the wake press eventually emits cannot
  // also press whatever was underneath the finger.
  bool swallowPending_ = false;
  uint32_t swallowAgeMs_ = 0;

  uint32_t shiftAccumMs_ = 0;
  uint8_t shiftIndex_ = 0;
  bool shiftChanged_ = false;
  int16_t shiftX_ = 0;
  int16_t shiftY_ = 0;

  uint32_t bedtimeAccumMs_ = 0;
  bool bedtimeActive_ = false;
};
