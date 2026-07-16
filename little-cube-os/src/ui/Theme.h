#pragma once

#include <Arduino.h>

#include "../board_config.h"

// Visual language for a calm 1.8" AMOLED: dark background (OLED-friendly),
// one accent, generous type. All colors are RGB565 from board_config.h so
// the palette stays in one place.
namespace theme {

constexpr uint16_t kBg = COLOR_BG;
constexpr uint16_t kPanel = COLOR_PANEL;
constexpr uint16_t kPanelAlt = COLOR_PANEL_2;
constexpr uint16_t kText = COLOR_TEXT;
constexpr uint16_t kTextDim = COLOR_DIM;
constexpr uint16_t kAccent = COLOR_ACCENT;
constexpr uint16_t kGood = COLOR_GOOD;
constexpr uint16_t kWarn = COLOR_WARN;
constexpr uint16_t kBad = COLOR_BAD;

// Layout constants (pixels at 368x448).
constexpr int16_t kStatusBarHeight = 28;
constexpr int16_t kPadding = 12;
constexpr int16_t kCardRadius = 14;
constexpr int16_t kTouchTargetMin = 48;

// Text sizes for the built-in 6x8 GFX font (multiplied).
constexpr uint8_t kTextSizeSmall = 2;   // 12x16
constexpr uint8_t kTextSizeBody = 3;    // 18x24
constexpr uint8_t kTextSizeTitle = 4;   // 24x32
constexpr uint8_t kTextSizeClock = 7;   // 42x56

}  // namespace theme
