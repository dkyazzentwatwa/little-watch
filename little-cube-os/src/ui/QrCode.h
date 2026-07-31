#pragma once

#include <Arduino.h>

class Arduino_GFX;

// QR rendering, wrapping the espressif/qrcode encoder that ships inside the
// ESP32 Arduino core (the same one WiFiProv uses internally) rather than a
// separate library — it already does everything this needs, so nothing new
// gets added to the three-site library registration in CLAUDE.md. This
// header and its .cpp are the ONLY place that encoder's header is included,
// so swapping it out later touches one file.
namespace qrcode {

// Draws a QR for `text` inside a maxSizePx square at (x, y), picking the
// largest module size that fits including the mandatory 4-module quiet zone.
//
// Returns false when the text does not fit the configured version cap, when
// maxSizePx cannot afford at least 2px per module (below that a phone camera
// cannot reliably resolve modules on this panel), when `bg` is too dark to
// read as a quiet zone, or when text is empty. The caller must draw its own
// fallback — a truncated or scaled QR is not a degraded QR, it is an
// unscannable one.
//
// `bg` must be a LIGHT color and is checked, not just assumed: QR scanners
// require dark modules on a light field, so inheriting theme::kBg on a dark
// palette would otherwise produce a code no phone can read. The caller
// passes the quiet-zone color explicitly rather than the draw call
// inheriting it from the active theme.
bool draw(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t maxSizePx, const char* text,
          uint16_t fg, uint16_t bg);

}  // namespace qrcode
