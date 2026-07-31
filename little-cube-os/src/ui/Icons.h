#pragma once

#include <Arduino.h>

class Arduino_GFX;

// App and weather glyphs, drawn entirely from Arduino_GFX primitives — no
// bitmap assets, no extra library, and every glyph takes the active theme's
// colors so a palette switch repaints them for free.
//
// draw() renders inside a size x size box at (x, y). `bg` is the surface the
// icon sits on: several glyphs cut detail back out of a filled shape (the fold
// in Notes, the mouth of the Recorder arc), which needs the background color
// rather than a transparency the canvas does not have.
namespace icons {

enum class IconId : uint8_t {
  Today,
  Clock,
  Weather,
  News,
  Notes,
  Reader,
  Recorder,
  Assistant,
  Audio,
  Calendar,
  Video,
  Settings,
  Tools,
  WxClear,
  WxPartlyCloudy,
  WxCloudy,
  WxRain,
  WxSnow,
  WxThunder,
  WxFog,
};

void draw(Arduino_GFX& gfx, IconId id, int16_t x, int16_t y, int16_t size, uint16_t color,
          uint16_t bg);

// Maps a WeatherSnapshot condition string to its glyph.
//
// The producing table (conditionFromWmo() in the weather service) emits a
// CLOSED set of 11 strings, so this is exact matching, not substring guessing.
// A cache file edited by hand can still hold anything, hence the fallback.
IconId forCondition(const char* condition);

}  // namespace icons
