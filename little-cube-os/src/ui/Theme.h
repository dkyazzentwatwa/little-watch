#pragma once

#include <Arduino.h>

#include "../board_config.h"

// Visual language for a 1.8" AMOLED. The PALETTE is now runtime-switchable:
// the nine color names below are mutable globals (not constexpr), so every
// existing `theme::kBg` / `theme::kText` call site is unchanged, but
// applyTheme() can repaint the whole UI by repointing them. Layout and
// text-size constants stay compile-time.
namespace theme {

// Active palette (RGB565). Written by applyTheme(); read everywhere.
extern uint16_t kBg;       // screen background
extern uint16_t kPanel;    // card / row fill
extern uint16_t kPanelAlt; // secondary fill, dividers
extern uint16_t kText;     // primary text
extern uint16_t kTextDim;  // secondary text
extern uint16_t kAccent;   // brand accent
extern uint16_t kGood;     // ok / connected
extern uint16_t kWarn;     // caution
extern uint16_t kBad;      // error

// A complete palette. `light` marks a light-background theme (legible dark
// text on a light bg) versus a dark-background one — the set below is
// intentionally balanced, five of each.
struct ThemeDef {
  const char* name;
  bool light;
  uint16_t bg;
  uint16_t panel;
  uint16_t panelAlt;
  uint16_t text;
  uint16_t textDim;
  uint16_t accent;
  uint16_t good;
  uint16_t warn;
  uint16_t bad;
};

constexpr uint8_t kThemeCount = 10;
extern const ThemeDef kThemes[kThemeCount];

// Copies kThemes[index] into the active globals (clamps out-of-range to 0).
// Does not touch the screen — the caller marks the UI dirty.
void applyTheme(uint8_t index);
uint8_t currentTheme();
const char* themeName(uint8_t index);
bool themeIsLight(uint8_t index);

// Layout constants (pixels at 368x448).
constexpr int16_t kStatusBarHeight = 28;
constexpr int16_t kPadding = 12;
constexpr int16_t kCardRadius = 14;
constexpr int16_t kTouchTargetMin = 48;

// Rounded-corner safe area: the physical AMOLED is a rounded square, so the
// ~top/bottom corners are eaten by the bezel radius. Persistent chrome (status
// bar, bottom controls) insets by this so nothing lands in a corner.
constexpr int16_t kSafeInset = 20;

// Text sizes for the built-in 6x8 GFX font (multiplied).
constexpr uint8_t kTextSizeSmall = 2;   // 12x16
constexpr uint8_t kTextSizeBody = 3;    // 18x24
constexpr uint8_t kTextSizeTitle = 4;   // 24x32
constexpr uint8_t kTextSizeClock = 7;   // 42x56

}  // namespace theme
