#include "InputAdapter.h"

// Vendored library include proves the --library plumbing from day one.
#include <Arduino_DriveBus_Library.h>

// TODO(task-3): FT3168 bring-up (retry x5) + gesture state machine using
// SWIPE_THRESHOLD_PX / LONG_PRESS_MS / DOUBLE_TAP_WINDOW_MS from
// board_config.h. Raw coordinates map 1:1 to screen pixels at rotation 0.

bool InputAdapter::begin() {
  button_.begin();
  touchReady_ = false;
  return touchReady_;
}

bool InputAdapter::poll(InputEvent& out) {
  switch (button_.poll()) {
    case PowerButton::Press::Short:
      out.action = InputAction::Back;
      out.x = 0;
      out.y = 0;
      out.timestampMs = millis();
      return true;
    case PowerButton::Press::Long:
      out.action = InputAction::Home;
      out.x = 0;
      out.y = 0;
      out.timestampMs = millis();
      return true;
    case PowerButton::Press::None:
      break;
  }
  return false;
}
