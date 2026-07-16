#pragma once

#include <Arduino.h>

// Semantic input layer (spec §9). Apps consume these actions, never raw
// touch coordinates. The InputAdapter synthesizes them from FT3168 touches
// and the BOOT button; future sources (encoder, IMU) map to the same enum.
enum class InputAction : uint8_t {
  None,
  Tap,
  DoubleTap,
  LongPress,
  SwipeLeft,
  SwipeRight,
  SwipeUp,
  SwipeDown,
  Back,
  Home,
  Confirm,
  Cancel,
};

struct InputEvent {
  InputAction action = InputAction::None;
  int16_t x = 0;
  int16_t y = 0;
  uint32_t timestampMs = 0;
};

const char* inputActionName(InputAction action);
