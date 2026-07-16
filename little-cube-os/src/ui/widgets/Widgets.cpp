#include "Widgets.h"

#include <Arduino_GFX_Library.h>

#include "../../board_config.h"
#include "../Theme.h"

namespace widgets {

namespace {
constexpr int16_t kCharW = 6;  // base GFX font cell, multiplied by text size
constexpr int16_t kCharH = 8;

int16_t textWidth(const char* text, uint8_t size) {
  return static_cast<int16_t>(strlen(text)) * kCharW * size;
}
}  // namespace

Rect button(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, int16_t h, const char* label,
            bool emphasized) {
  const uint16_t fill = emphasized ? theme::kAccent : theme::kPanel;
  const uint16_t text = emphasized ? theme::kBg : theme::kText;
  gfx.fillRoundRect(x, y, w, h, theme::kCardRadius / 2, fill);
  const uint8_t size = theme::kTextSizeBody;
  const int16_t tw = textWidth(label, size);
  gfx.setTextSize(size);
  gfx.setTextColor(text);
  gfx.setCursor(x + (w - tw) / 2, y + (h - kCharH * size) / 2);
  gfx.print(label);
  return Rect{x, y, w, h};
}

Rect listItem(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, const char* primary,
              const char* secondary, bool selected) {
  const int16_t h = secondary != nullptr ? 56 : 40;
  if (selected) {
    gfx.fillRoundRect(x, y, w, h, 6, theme::kPanelAlt);
  }
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(x + 8, y + 8);
  gfx.print(primary);
  if (secondary != nullptr) {
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kTextDim);
    gfx.setCursor(x + 8, y + 32);
    gfx.print(secondary);
  }
  return Rect{x, y, w, h};
}

void textBlock(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, const char* text,
               uint8_t textSize, uint16_t color) {
  // Greedy word wrap on the fixed-width base font.
  const int16_t charsPerLine = w / (kCharW * textSize);
  if (charsPerLine <= 0 || text == nullptr) {
    return;
  }
  gfx.setTextSize(textSize);
  gfx.setTextColor(color);

  const int16_t lineH = kCharH * textSize + 2;
  int16_t cursorY = y;
  const char* p = text;
  char line[96];

  while (*p != '\0') {
    // Take up to charsPerLine chars, preferring to break at a space.
    int16_t take = 0;
    int16_t lastSpace = -1;
    while (p[take] != '\0' && p[take] != '\n' && take < charsPerLine &&
           take < static_cast<int16_t>(sizeof(line)) - 1) {
      if (p[take] == ' ') {
        lastSpace = take;
      }
      take++;
    }
    int16_t lineLen = take;
    if (p[take] != '\0' && p[take] != '\n' && lastSpace > 0) {
      lineLen = lastSpace;  // break at the last space that fits
    }
    memcpy(line, p, lineLen);
    line[lineLen] = '\0';
    gfx.setCursor(x, cursorY);
    gfx.print(line);
    cursorY += lineH;

    p += lineLen;
    while (*p == ' ') {
      p++;
    }
    if (*p == '\n') {
      p++;
    }
  }
}

void card(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, int16_t h) {
  gfx.fillRoundRect(x, y, w, h, theme::kCardRadius, theme::kPanel);
}

void toast(Arduino_GFX& gfx, const char* message) {
  const uint8_t size = theme::kTextSizeSmall;
  const int16_t tw = textWidth(message, size);
  const int16_t w = tw + 32;
  const int16_t h = 36;
  const int16_t x = (DISPLAY_WIDTH - w) / 2;
  const int16_t y = DISPLAY_HEIGHT - h - 16;
  gfx.fillRoundRect(x, y, w, h, h / 2, theme::kPanelAlt);
  gfx.setTextSize(size);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(x + 16, y + (h - kCharH * size) / 2);
  gfx.print(message);
}

Rect modalConfirm(Arduino_GFX& gfx, const char* title, const char* body, Rect& cancelOut) {
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  const int16_t h = 220;
  const int16_t x = theme::kPadding;
  const int16_t y = (DISPLAY_HEIGHT - h) / 2;

  gfx.fillRoundRect(x, y, w, h, theme::kCardRadius, theme::kPanelAlt);
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(x + 16, y + 16);
  gfx.print(title);
  textBlock(gfx, x + 16, y + 52, w - 32, body, theme::kTextSizeSmall, theme::kTextDim);

  const int16_t bw = (w - 48) / 2;
  const int16_t by = y + h - 64;
  cancelOut = button(gfx, x + 16, by, bw, 48, "Cancel", false);
  return button(gfx, x + 32 + bw, by, bw, 48, "Confirm", true);
}

}  // namespace widgets
