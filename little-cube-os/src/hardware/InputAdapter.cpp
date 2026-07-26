#include "InputAdapter.h"

#include <Arduino_DriveBus_Library.h>
#include <Wire.h>

#include <memory>

#include "../board_config.h"

namespace {

std::shared_ptr<Arduino_IIC_DriveBus> touchBus;
std::unique_ptr<Arduino_FT3x68> touchDevice;

// INT edge latch. The ISR catches every FT3168 interrupt regardless of loop
// cadence, so a tap can never be missed between polls; the counter feeds
// `input status` so a unit's INT wiring can be verified from serial.
volatile bool s_touchIntFlag = false;
volatile uint32_t s_touchIntCount = 0;

void IRAM_ATTR touchIntIsr() {
  s_touchIntFlag = true;
  s_touchIntCount = s_touchIntCount + 1;
}

// Raw FT3168 coordinates map 1:1 to SH8601 screen pixels at rotation 0 —
// clamp only, never swap axes (swapped Cardputer ports had to be patched
// back to identity on this board).
void clampTouch(int32_t rawX, int32_t rawY, uint16_t& outX, uint16_t& outY) {
  int32_t x = rawX;
  int32_t y = rawY;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= DISPLAY_WIDTH) x = DISPLAY_WIDTH - 1;
  if (y >= DISPLAY_HEIGHT) y = DISPLAY_HEIGHT - 1;
  outX = static_cast<uint16_t>(x);
  outY = static_cast<uint16_t>(y);
}

}  // namespace

bool InputAdapter::begin() {
  button_.begin();

  touchBus = std::make_shared<Arduino_HWIIC>(PIN_TOUCH_SDA, PIN_TOUCH_SCL, &Wire);
  touchDevice.reset(new Arduino_FT3x68(touchBus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE,
                                       PIN_TOUCH_INT));
  for (uint8_t tries = 0; tries < 5; tries++) {
    if (touchDevice->begin()) {
      touchDevice->IIC_Write_Device_State(
          touchDevice->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
          touchDevice->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
      // INT-gated polling (see header). FALLING catches both level-hold and
      // per-report pulse INT modes; the pullup keeps an open-drain line sane.
      pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
      attachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT), touchIntIsr, FALLING);
      Serial.printf("[touch] FT3168 init ok id=0x%X\n",
                    static_cast<unsigned>(touchDevice->IIC_Read_Device_ID()));
      touchReady_ = true;
      return true;
    }
    Serial.println("[touch] FT3168 init retry");
    delay(250);
  }
  Serial.println("[touch] FT3168 init failed; BOOT button remains available");
  touchReady_ = false;
  return false;
}

bool InputAdapter::readTouch(uint16_t& x, uint16_t& y) {
  if (!touchReady_ || !touchDevice) {
    return false;
  }
  const int32_t fingers = static_cast<int32_t>(touchDevice->IIC_Read_Device_Value(
      touchDevice->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER));
  if (fingers <= 0) {
    return false;
  }
  const int32_t rawX = static_cast<int32_t>(touchDevice->IIC_Read_Device_Value(
      touchDevice->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X));
  const int32_t rawY = static_cast<int32_t>(touchDevice->IIC_Read_Device_Value(
      touchDevice->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y));
  clampTouch(rawX, rawY, x, y);
  return true;
}

InputEvent InputAdapter::makeEvent(InputAction action, int16_t x, int16_t y) {
  InputEvent event;
  event.action = action;
  event.x = x;
  event.y = y;
  event.timestampMs = millis();
  // Off by default: this is a USB-CDC write on the input path, so a host that
  // holds the port open without draining it stalls every gesture. Toggle it
  // with `input debug on` when diagnosing touch.
  if (debugLog_) {
    Serial.printf("[input] %s at (%d,%d)\n", inputActionName(action), x, y);
  }
  return event;
}

uint32_t InputAdapter::touchIntEdges() const {
  return s_touchIntCount;
}

bool InputAdapter::pollTouch(InputEvent& out) {
  if (!touchReady_) {
    return false;
  }
  // I2C-silent idle (see header): read the controller only while a touch
  // session is in flight or the INT line reports fresh contact.
  bool wantRead = touchWasDown_;
  if (s_touchIntFlag) {
    s_touchIntFlag = false;
    intSeen_ = true;
    wantRead = true;
  }
  if (!wantRead && digitalRead(PIN_TOUCH_INT) == LOW) {
    wantRead = true;  // level backstop: INT still asserted or edge lost
  }
  if (!wantRead && !intSeen_ && !fallbackSuppressed_) {
    // INT unproven on this unit: slow timed polling so touch cannot go dead.
    const uint32_t nowMs = millis();
    if (nowMs - lastFallbackPollMs_ >= TOUCH_INT_FALLBACK_POLL_MS) {
      lastFallbackPollMs_ = nowMs;
      wantRead = true;
    }
  }
  if (!wantRead) {
    return false;
  }
  uint16_t x = 0;
  uint16_t y = 0;
  const bool down = readTouch(x, y);
  const uint32_t now = millis();

  if (down && !touchWasDown_) {
    touchWasDown_ = true;
    longFired_ = false;
    startX_ = lastX_ = x;
    startY_ = lastY_ = y;
    downSinceMs_ = now;
    return false;
  }

  if (down && touchWasDown_) {
    lastX_ = x;
    lastY_ = y;
    const int16_t dx = static_cast<int16_t>(lastX_) - static_cast<int16_t>(startX_);
    const int16_t dy = static_cast<int16_t>(lastY_) - static_cast<int16_t>(startY_);
    const bool stationary = abs(dx) <= SWIPE_THRESHOLD_PX && abs(dy) <= SWIPE_THRESHOLD_PX;
    if (!longFired_ && stationary && (now - downSinceMs_) >= LONG_PRESS_MS) {
      longFired_ = true;
      out = makeEvent(InputAction::LongPress, lastX_, lastY_);
      return true;
    }
    return false;
  }

  if (!down && touchWasDown_) {
    touchWasDown_ = false;
    if (longFired_) {
      return false;  // long press already delivered while held
    }
    const int16_t dx = static_cast<int16_t>(lastX_) - static_cast<int16_t>(startX_);
    const int16_t dy = static_cast<int16_t>(lastY_) - static_cast<int16_t>(startY_);

    if (abs(dx) > SWIPE_THRESHOLD_PX || abs(dy) > SWIPE_THRESHOLD_PX) {
      InputAction action;
      if (abs(dx) > abs(dy)) {
        action = dx > 0 ? InputAction::SwipeRight : InputAction::SwipeLeft;
      } else {
        action = dy > 0 ? InputAction::SwipeDown : InputAction::SwipeUp;
      }
      out = makeEvent(action, lastX_, lastY_);
      return true;
    }

    const bool isDouble = (now - lastTapAtMs_) <= DOUBLE_TAP_WINDOW_MS && lastTapAtMs_ != 0;
    lastTapAtMs_ = isDouble ? 0 : now;  // a double-tap consumes the chain
    out = makeEvent(isDouble ? InputAction::DoubleTap : InputAction::Tap, lastX_, lastY_);
    return true;
  }

  return false;
}

bool InputAdapter::poll(InputEvent& out) {
  switch (button_.poll()) {
    case PowerButton::Press::Short:
      out = makeEvent(InputAction::Back, 0, 0);
      return true;
    case PowerButton::Press::Long:
      out = makeEvent(InputAction::Home, 0, 0);
      return true;
    case PowerButton::Press::None:
      break;
  }
  return pollTouch(out);
}
