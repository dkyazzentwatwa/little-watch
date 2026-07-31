#include "AmoledProtection.h"

#include "../board_config.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/InputAdapter.h"
#include "../services/SettingsService.h"
#include "../services/TimeService.h"

namespace {

// The dim step is a visible warning that the screen is about to go dark, so
// it lands 10 s before the blank rather than replacing it.
constexpr uint32_t kDimToOffMs = 10000;

// Dim is "reduced brightness for persistent content" (spec §37), not a
// pre-blank: it stays lit enough to read a clock across a room.
constexpr uint8_t kDimDivisor = 4;
constexpr uint8_t kDimFloor = 8;

// One shift step per minute. Fast enough that no glyph sits on one pixel long
// enough to matter, slow enough that the drift is never visible in use.
constexpr uint32_t kShiftPeriodMs = 60000;

// The wake swallow must not outlive the gesture it is waiting for.
constexpr uint32_t kSwallowExpiryMs = 1000;

// A day, comfortably above the longest timeout SettingsService will store.
constexpr uint32_t kIdleCeilingMs = 24UL * 60 * 60 * 1000;

// The bedtime check reads the clock, so it runs on its own slow timer rather
// than every frame.
constexpr uint32_t kBedtimeCheckMs = 30000;

// +/-2 px rectangular walk. Checked against the 368x448 StatusBar layout:
// the leftmost glyph starts at kPadding (12) and the rightmost battery tip
// ends 10 px short of the right edge, and the tallest status glyph reaches
// y=24 inside a 28 px bar — so 2 px in any direction clips nothing.
constexpr int16_t kShiftSteps[4][2] = {{-2, -2}, {2, -2}, {2, 2}, {-2, 2}};

}  // namespace

void AmoledProtection::begin(DisplayAdapter* display, SettingsService* settings,
                             InputAdapter* input, TimeService* time) {
  display_ = display;
  settings_ = settings;
  input_ = input;
  time_ = time;
  // Evaluate the bedtime window on the very first update() instead of 30 s in,
  // so a device powered on at 2 a.m. comes up already dimmed.
  bedtimeAccumMs_ = kBedtimeCheckMs;
}

// swallowNextEvent is true only for a wake triggered by the touch-down edge in
// update(): that press has not emitted its semantic event yet, and when it does
// the event is a wake, not a command. A wake from onActivity() has already
// consumed its own event, so there is nothing left to eat — arming the flag
// there would swallow the user's next deliberate tap instead.
void AmoledProtection::wake(bool swallowNextEvent) {
  state_ = State::Active;
  idleMs_ = 0;
  // A free shift advance on every wake: the screen is being redrawn from
  // scratch anyway, so the step costs nothing visually.
  advanceShift();
  if (swallowNextEvent) {
    swallowPending_ = true;
    swallowAgeMs_ = 0;
  }
}

void AmoledProtection::advanceShift() {
  shiftIndex_ = static_cast<uint8_t>((shiftIndex_ + 1) & 0x03);
  shiftX_ = kShiftSteps[shiftIndex_][0];
  shiftY_ = kShiftSteps[shiftIndex_][1];
  shiftChanged_ = true;
  shiftAccumMs_ = 0;
}

bool AmoledProtection::consumeShiftChanged() {
  const bool changed = shiftChanged_;
  shiftChanged_ = false;
  return changed;
}

// Same wake path onActivity() takes from Off, minus the swallow-verdict
// logic that only makes sense for a real input event. Safe to call every
// frame: once state_ is Active the branch below is a no-op.
void AmoledProtection::keepAwake() {
  idleMs_ = 0;
  if (state_ == State::Dim || state_ == State::Off) {
    wake(false);
    applyBrightness();
  }
}

bool AmoledProtection::onActivity() {
  if (state_ == State::Off) {
    wake(false);
    return false;  // this gesture only turned the screen back on
  }
  idleMs_ = 0;
  if (swallowPending_) {
    // The tail of a wake press: the screen came back on the touch-down edge
    // and this is the gesture the release produced. It is a wake, not a
    // command, and must not press what was underneath the finger.
    swallowPending_ = false;
    return false;
  }
  return true;
}

// The window is inclusive of start and exclusive of end, and flips its test
// when it wraps midnight (the 22:00 -> 07:00 default does).
bool AmoledProtection::insideBedtimeWindow() {
  if (settings_ == nullptr || !settings_->bedtimeEnabled() || time_ == nullptr) {
    return false;
  }
  // TimeService::now() falls back to a blocking I2C read of the RTC when the
  // clock is not yet valid. That must never happen anywhere near the frame,
  // so an unset clock simply means no bedtime rather than a stall.
  if (!time_->valid()) {
    return false;
  }
  struct tm t;
  if (!time_->now(t)) {
    return false;
  }
  const uint16_t minutes = static_cast<uint16_t>(t.tm_hour * 60 + t.tm_min);
  const uint16_t start = settings_->bedtimeStartMin();
  const uint16_t end = settings_->bedtimeEndMin();
  if (start == end) {
    return false;  // zero-length window
  }
  return start < end ? (minutes >= start && minutes < end)
                     : (minutes >= start || minutes < end);
}

void AmoledProtection::applyBrightness() {
  if (display_ == nullptr || settings_ == nullptr) {
    return;
  }
  // The baseline is always the stored setting, never display_->brightness():
  // that reads back whatever this class last wrote, which is legitimately 0
  // while blanked and would ratchet the panel permanently dark.
  int target = settings_->brightness();

  if (bedtimeActive_) {
    const int cap = settings_->bedtimeBrightness();
    if (cap < target) {
      target = cap;  // bedtime only ever lowers
    }
  }

  if (state_ == State::Off) {
    target = 0;
  } else if (state_ == State::Dim) {
    target = target / kDimDivisor;
    if (target < kDimFloor) {
      target = kDimFloor;
    }
  }

  const uint8_t value = static_cast<uint8_t>(target);
  if (value != lastApplied_) {
    lastApplied_ = value;
    display_->setBrightness(value);
  }
}

void AmoledProtection::update(uint32_t deltaMs) {
  if (settings_ == nullptr) {
    return;
  }

  // A finger resting on the glass emits no semantic event between touch-down
  // and release (InputAdapter::pollTouch), so without this the idle clock runs
  // on and the screen blanks under someone reading a long note.
  const bool touching = input_ != nullptr && input_->touchDown();
  if (touching) {
    if (state_ == State::Off) {
      // Woken by the touch-down edge. Arm the swallow now: the gesture this
      // press emits on release is what turned the screen on, not a command.
      wake(true);
    }
    idleMs_ = 0;
  } else if (idleMs_ < kIdleCeilingMs) {
    // Stops climbing once it is past every threshold, so a device left alone
    // long enough cannot wrap uint32_t and wake itself back up.
    idleMs_ += deltaMs;
  }

  // The swallow only ages while the glass is clear. With a finger still down
  // the gesture is still coming, however long the press lasts; the timer is
  // there so a wake that never produces a follow-up cannot strand the flag.
  if (swallowPending_ && !touching) {
    swallowAgeMs_ += deltaMs;
    if (swallowAgeMs_ >= kSwallowExpiryMs) {
      swallowPending_ = false;
    }
  }

  // State is a pure function of the idle clock and settings, so it is
  // recomputed rather than edged — there is no path that can leave it stale.
  const uint32_t timeoutSec = settings_->screenTimeoutSec();
  if (timeoutSec == 0) {
    state_ = State::Active;  // the "never" step in the Settings UI
  } else {
    const uint32_t dimAfterMs = timeoutSec * 1000;
    const uint32_t offAfterMs = dimAfterMs + kDimToOffMs;
    if (idleMs_ >= offAfterMs && !settings_->alwaysOn()) {
      state_ = State::Off;
    } else if (idleMs_ >= dimAfterMs) {
      // Always-on still dims. Spec §37 asks for reduced brightness on
      // persistent content; it only forbids going fully dark.
      state_ = State::Dim;
    } else {
      state_ = State::Active;
    }
  }

  bedtimeAccumMs_ += deltaMs;
  if (bedtimeAccumMs_ >= kBedtimeCheckMs) {
    bedtimeAccumMs_ = 0;
    bedtimeActive_ = insideBedtimeWindow();
  }

  shiftAccumMs_ += deltaMs;
  if (shiftAccumMs_ >= kShiftPeriodMs) {
    advanceShift();
  }

  applyBrightness();
}
