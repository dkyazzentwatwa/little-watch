#include "PowerButton.h"

#include "../board_config.h"

void PowerButton::begin() {
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
}

PowerButton::Press PowerButton::poll() {
  const bool down = digitalRead(PIN_BOOT_BUTTON) == (BOOT_BUTTON_ACTIVE_LOW ? LOW : HIGH);
  const uint32_t now = millis();

  if (down && !wasDown_) {
    wasDown_ = true;
    longFired_ = false;
    downSinceMs_ = now;
    return Press::None;
  }

  if (down && wasDown_ && !longFired_ && (now - downSinceMs_) >= BOOT_LONG_PRESS_MS) {
    longFired_ = true;
    return Press::Long;
  }

  if (!down && wasDown_) {
    wasDown_ = false;
    if (longFired_) {
      return Press::None;  // long press already reported
    }
    return Press::Short;
  }

  return Press::None;
}
