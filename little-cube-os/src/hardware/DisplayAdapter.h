#pragma once

#include <Arduino.h>

class Arduino_DataBus;
class Arduino_SH8601;
class Arduino_Canvas;
class Arduino_GFX;
class Adafruit_XCA9554;

// SH8601 AMOLED behind a full-frame PSRAM canvas. Apps draw into canvas()
// every frame; present() flushes to the panel only when something changed,
// capped at ~30 fps. Brightness is the AMOLED command (0 = panel dark) —
// there is no backlight. If the canvas framebuffer cannot be allocated,
// canvas() falls back to the raw panel (direct draw, no compositor).
class DisplayAdapter {
 public:
  bool begin();
  bool ready() const { return ready_; }
  bool hasCanvas() const { return canvas_ != nullptr; }

  Arduino_GFX* canvas();
  void markDirty() { dirty_ = true; }
  void present();
  // True when a frame has been drawn but not yet pushed to the panel. The
  // kernel skips re-rendering while this holds: flushing is capped at ~30 fps
  // but the loop runs far faster, so anything drawn meanwhile is overwritten
  // before it is ever seen.
  bool flushPending() const { return dirty_; }

  void setBrightness(uint8_t value);
  uint8_t brightness() const { return brightness_; }

  // Minimal centered boot/status screen, usable before the UI stack is up.
  void splash(const char* title, const char* subtitle);

 private:
  Arduino_DataBus* bus_ = nullptr;
  Arduino_SH8601* panel_ = nullptr;
  Arduino_Canvas* canvas_ = nullptr;
  Adafruit_XCA9554* expander_ = nullptr;
  uint8_t brightness_ = 0;
  bool dirty_ = false;
  bool ready_ = false;
  uint32_t lastFlushMs_ = 0;
};
