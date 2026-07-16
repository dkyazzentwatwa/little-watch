#pragma once

#include <Arduino.h>

class Arduino_ESP32QSPI;
class Arduino_SH8601;
class Arduino_Canvas;
class Arduino_GFX;

// SH8601 AMOLED behind a full-frame PSRAM canvas. Apps draw into canvas()
// every frame; present() flushes to the panel only when something changed.
// Brightness is the AMOLED command (0 = panel off) — there is no backlight.
class DisplayAdapter {
 public:
  bool begin();
  bool ready() const { return ready_; }

  Arduino_GFX* canvas() { return reinterpret_cast<Arduino_GFX*>(canvas_); }
  void markDirty() { dirty_ = true; }
  void present();

  void setBrightness(uint8_t value);
  uint8_t brightness() const { return brightness_; }

 private:
  Arduino_ESP32QSPI* bus_ = nullptr;
  Arduino_SH8601* panel_ = nullptr;
  Arduino_Canvas* canvas_ = nullptr;
  uint8_t brightness_ = 0;
  bool dirty_ = false;
  bool ready_ = false;
};
