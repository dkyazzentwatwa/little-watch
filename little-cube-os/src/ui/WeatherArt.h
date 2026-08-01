#pragma once

#include <Arduino.h>

// wttr.in-style ASCII weather art for the built-in 6x8 bitmap font.
//
// WHY THIS LIVES IN ui/ AND NOT IN WeatherApp: it is a shared drawing asset,
// the same category as Icons.cpp's vector glyphs and ClockFaces.cpp's digit
// tables. The Today glance screen shows the same conditions and needs the same
// marks; a table private to WeatherApp would have to be copied to get there,
// and two copies of an art table drift.
//
// THE GRID. Every entry is exactly kRows lines of exactly kCols characters,
// space-padded, so a caller can print each line at a fixed x and the whole
// block occupies 6*kCols*sizeX by 8*kRows*sizeY pixels. No line has trailing
// content beyond kCols and none is short — the layout arithmetic in
// WeatherApp.cpp depends on that being true for every condition.
//
// CHARACTER SET, verified against GFX_Library_for_Arduino's
// src/font/glcdfont.h rather than assumed:
//   ' ' \ / - . ( ) _ ` ' " * ?
// Notably ABSENT is '|' (0x7C): this font's vertical bar is a BROKEN bar —
// rows 3 and 7 of its 8 are blank — so anything relying on it to join across
// stacked rows renders as dashes. None of the art below needs a vertical join;
// the cloud outlines are built from '(' ')' '_' '-' '.', which read correctly
// as isolated marks. The one glyph in the table that IS a full-height solid
// column is 0xB3, and it is deliberately unused here.
//
// The ink positions inside a cell are what make the sun disc work: '.' inks
// rows 5-6 (low in the cell) and '-' inks row 3 (middle), so ".-." curves up;
// '`' and '\'' ink rows 0-3 (high), so "`-'" curves down. '_' inks row 6 only.
namespace weatherart {

constexpr uint8_t kRows = 5;
constexpr uint8_t kCols = 9;

// Art block for a condition string. Returns kRows NUL-terminated lines of
// kCols characters; never nullptr.
//
// EXACT matching, not substring: conditionFromWmo() in the weather service
// emits a CLOSED set of 11 strings ("Clear", "Mostly clear", "Overcast",
// "Fog", "Drizzle", "Rain", "Snow", "Showers", "Snow showers",
// "Thunderstorm", "Weather"), the same closed set icons::forCondition()
// matches against. A hand-edited cache file can still hold anything, hence
// the fallback.
const char* const* forCondition(const char* condition);

// Three-character uppercase marker for the same condition, for rows too tight
// to hold the art or the full condition text. Always exactly 3 characters, so
// a column of them stays aligned and nothing truncates — the forecast rows
// used to print the condition text here and it clipped to "Overcas" on the
// device.
const char* codeForCondition(const char* condition);

}  // namespace weatherart
