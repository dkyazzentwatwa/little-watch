#pragma once

#include <Arduino.h>

// BOOT button (GPIO 0, active-low). Debounced short/long press detector;
// the InputAdapter maps Short -> Back and Long -> Home so every gesture has
// a hardware fallback.
class PowerButton {
 public:
  enum class Press {
    None,
    Short,
    Long,
  };

  void begin();
  // Call every loop; returns a press once, on release (Short) or on
  // reaching the long-press threshold (Long).
  Press poll();

 private:
  static constexpr uint32_t kDebounceMs = 30;

  bool wasDown_ = false;
  bool longFired_ = false;
  bool lastRawDown_ = false;
  uint32_t downSinceMs_ = 0;
  uint32_t lastEdgeMs_ = 0;
};
