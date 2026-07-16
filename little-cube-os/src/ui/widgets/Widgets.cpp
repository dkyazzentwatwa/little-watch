#include "Widgets.h"

// TODO(task-5): real drawing on Arduino_GFX with theme constants.

namespace widgets {

Rect button(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, int16_t h, const char* label,
            bool emphasized) {
  (void)gfx;
  (void)label;
  (void)emphasized;
  return Rect{x, y, w, h};
}

Rect listItem(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, const char* primary,
              const char* secondary, bool selected) {
  (void)gfx;
  (void)primary;
  (void)secondary;
  (void)selected;
  return Rect{x, y, w, 0};
}

void textBlock(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, const char* text,
               uint8_t textSize, uint16_t color) {
  (void)gfx;
  (void)x;
  (void)y;
  (void)w;
  (void)text;
  (void)textSize;
  (void)color;
}

void card(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, int16_t h) {
  (void)gfx;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
}

void toast(Arduino_GFX& gfx, const char* message) {
  (void)gfx;
  (void)message;
}

Rect modalConfirm(Arduino_GFX& gfx, const char* title, const char* body, Rect& cancelOut) {
  (void)gfx;
  (void)title;
  (void)body;
  cancelOut = Rect{};
  return Rect{};
}

}  // namespace widgets
