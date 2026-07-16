#include "DisplayAdapter.h"

// TODO(task-2): real bring-up — XCA9554 expander reset dance, QSPI bus,
// SH8601 init, Arduino_Canvas allocation with PSRAM verification.

bool DisplayAdapter::begin() {
  ready_ = false;
  return ready_;
}

void DisplayAdapter::present() {
  if (!ready_ || !dirty_) {
    return;
  }
  dirty_ = false;
}

void DisplayAdapter::setBrightness(uint8_t value) {
  brightness_ = value;
}
