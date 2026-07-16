#pragma once

#include <Arduino.h>

class Arduino_GFX;

// Small immediate-mode widget helpers shared by all apps: draw into the
// frame canvas using theme constants. Buttons/list items return their hit
// rects so apps can match tap coordinates without layout duplication.
namespace widgets {

struct Rect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  bool contains(int16_t px, int16_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

Rect button(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, int16_t h, const char* label,
            bool emphasized = false);
Rect listItem(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, const char* primary,
              const char* secondary, bool selected = false);
void textBlock(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, const char* text,
               uint8_t textSize, uint16_t color);
void card(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, int16_t h);
void toast(Arduino_GFX& gfx, const char* message);
Rect modalConfirm(Arduino_GFX& gfx, const char* title, const char* body, Rect& cancelOut);

}  // namespace widgets
