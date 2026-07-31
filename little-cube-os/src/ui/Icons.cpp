#include "Icons.h"

#include <Arduino_GFX_Library.h>

namespace icons {

namespace {

// Stroke weight scales with the box so an icon stays balanced at any size.
int16_t stroke(int16_t size) {
  const int16_t t = size / 16;
  return t < 2 ? 2 : t;
}

// Concentric-circle ring: fillArc's angle convention varies by build, but a
// full 0..360 sweep is unambiguous, so rings use it and nothing else does.
void ring(Arduino_GFX& gfx, int16_t cx, int16_t cy, int16_t r, int16_t thickness,
          uint16_t color) {
  gfx.fillArc(cx, cy, r, r - thickness, 0, 360, color);
}

void strokeRect(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, int16_t h, int16_t radius,
                int16_t thickness, uint16_t color) {
  for (int16_t i = 0; i < thickness; i++) {
    gfx.drawRoundRect(x + i, y + i, w - 2 * i, h - 2 * i, radius, color);
  }
}

}  // namespace

void draw(Arduino_GFX& gfx, IconId id, int16_t x, int16_t y, int16_t size, uint16_t color,
          uint16_t bg) {
  const int16_t t = stroke(size);
  const int16_t cx = x + size / 2;
  const int16_t cy = y + size / 2;
  const int16_t q = size / 4;

  switch (id) {
    case IconId::Today: {
      // Agenda card: outline plus three ruled lines, top one accented full width.
      strokeRect(gfx, x + q / 2, y + q / 2, size - q, size - q, t * 2, t, color);
      const int16_t lx = x + q;
      int16_t ly = y + size / 2 - t * 2;
      for (uint8_t i = 0; i < 3; i++) {
        gfx.fillRect(lx, ly, (i == 0 ? size - 2 * q : (size - 2 * q) * 2 / 3), t, color);
        ly += t * 3;
      }
      break;
    }
    case IconId::Clock: {
      ring(gfx, cx, cy, size / 2 - t, t, color);
      gfx.fillRect(cx - t / 2, cy - size / 4, t, size / 4, color);          // minute hand
      gfx.fillRect(cx - t / 2, cy - t / 2, size / 5, t, color);             // hour hand
      gfx.fillCircle(cx, cy, t, color);
      break;
    }
    case IconId::Weather: {
      // Sun peeking behind a cloud; the cloud is punched out of the sun in bg
      // so the two shapes read as separate objects rather than one blob.
      gfx.fillCircle(x + size - q, y + q, q - t, color);
      gfx.fillCircle(x + q + t, cy + t, q - t, bg);
      gfx.fillCircle(cx + t, cy - t / 2, q - t, bg);
      gfx.fillRect(x + q / 2, cy, size - q - t, q, bg);
      gfx.fillCircle(x + q + t, cy + t, q - t * 2, color);
      gfx.fillCircle(cx + t, cy - t / 2, q - t * 2, color);
      gfx.fillRect(x + q, cy + t, size - q * 2, q - t, color);
      break;
    }
    case IconId::News: {
      strokeRect(gfx, x + q / 2, y + q / 2, size - q, size - q, t, t, color);
      gfx.fillRect(x + q, y + q, (size - 2 * q) / 2, q - t, color);  // masthead block
      int16_t ny = y + q;
      for (uint8_t i = 0; i < 3; i++) {
        gfx.fillRect(cx + t, ny, (size - 2 * q) / 2 - t, t, color);
        ny += t * 3;
      }
      gfx.fillRect(x + q, cy + q / 2, size - 2 * q, t, color);
      break;
    }
    case IconId::Notes: {
      // Page with a folded top-right corner, cut back out in bg.
      gfx.fillRoundRect(x + q / 2, y + q / 2, size - q, size - q, t * 2, color);
      gfx.fillTriangle(x + size - q - t * 3, y + q / 2, x + size - q / 2, y + q / 2,
                       x + size - q / 2, y + q / 2 + t * 3, bg);
      int16_t py = cy - t * 2;
      for (uint8_t i = 0; i < 3; i++) {
        gfx.fillRect(x + q, py, (i == 2 ? (size - 2 * q) / 2 : size - 2 * q), t, bg);
        py += t * 3;
      }
      break;
    }
    case IconId::Reader: {
      // Open book: two pages meeting at a gutter.
      const int16_t pw = (size - q) / 2 - t;
      const int16_t ph = size - q - t;
      strokeRect(gfx, x + q / 2, y + q / 2 + t, pw, ph, t, t, color);
      strokeRect(gfx, cx + t, y + q / 2 + t, pw, ph, t, t, color);
      gfx.fillRect(cx - t / 2, y + q / 2, t, ph + t * 2, color);  // spine
      break;
    }
    case IconId::Recorder: {
      // Capsule head, pickup arc below it, stand and base.
      const int16_t hw = size / 5;
      gfx.fillRoundRect(cx - hw / 2, y + q / 2, hw, size / 2 - t, hw / 2, color);
      ring(gfx, cx, cy, size / 3, t, color);
      gfx.fillRect(x, y, size, size / 2 + t, bg);  // erase the arc's upper half
      gfx.fillRoundRect(cx - hw / 2, y + q / 2, hw, size / 2 - t, hw / 2, color);
      gfx.fillRect(cx - t / 2, cy + size / 3 - t, t, q / 2, color);
      gfx.fillRect(cx - q / 2, y + size - q / 2 - t, q, t, color);
      break;
    }
    case IconId::Assistant: {
      // Speech bubble with a tail and three dots punched out.
      gfx.fillRoundRect(x + q / 2, y + q / 2, size - q, size - q - t * 2, t * 3, color);
      gfx.fillTriangle(x + q, y + size - q - t * 2, x + q + t * 4, y + size - q - t * 2,
                       x + q, y + size - q / 2, color);
      const int16_t dy = y + q / 2 + (size - q - t * 2) / 2;
      gfx.fillCircle(cx - q / 2, dy, t, bg);
      gfx.fillCircle(cx, dy, t, bg);
      gfx.fillCircle(cx + q / 2, dy, t, bg);
      break;
    }
    case IconId::Audio: {
      // Speaker cone plus two emission arcs.
      gfx.fillRect(x + q / 2, cy - q / 3, q / 2, q * 2 / 3, color);
      gfx.fillTriangle(x + q, cy, x + q + q / 2, y + q / 2, x + q + q / 2, y + size - q / 2,
                       color);
      ring(gfx, x + q + q / 2, cy, q, t, color);
      ring(gfx, x + q + q / 2, cy, q * 3 / 2, t, color);
      gfx.fillRect(x, y, q + q / 2, size, bg);  // keep arcs to the right side
      gfx.fillRect(x + q / 2, cy - q / 3, q / 2, q * 2 / 3, color);
      gfx.fillTriangle(x + q, cy, x + q + q / 2, y + q / 2, x + q + q / 2, y + size - q / 2,
                       color);
      break;
    }
    case IconId::Calendar: {
      strokeRect(gfx, x + q / 2, y + q / 2 + t, size - q, size - q - t, t * 2, t, color);
      gfx.fillRect(x + q / 2, y + q / 2 + t, size - q, q / 2, color);  // header band
      gfx.fillRect(x + q, y + q / 4, t, q / 2, color);                 // hanging rings
      gfx.fillRect(x + size - q - t, y + q / 4, t, q / 2, color);
      for (uint8_t r = 0; r < 2; r++) {
        for (uint8_t c = 0; c < 3; c++) {
          gfx.fillRect(x + q + c * (q * 2 / 3), cy + r * (q * 2 / 3), t * 2, t * 2, color);
        }
      }
      break;
    }
    case IconId::Video: {
      // A screen with a play triangle.
      gfx.drawRoundRect(x, y + q / 2, size, size - q, t * 2, color);
      gfx.drawRoundRect(x + 1, y + q / 2 + 1, size - 2, size - q - 2, t * 2, color);
      gfx.fillTriangle(x + q + t, cy - q + t, x + q + t, cy + q - t, x + size - q, cy,
                       color);
      break;
    }
    case IconId::Settings: {
      // Gear approximated by a ring, four spokes and a hub — cheaper and
      // cleaner at this size than real teeth.
      ring(gfx, cx, cy, size / 2 - t, t, color);
      gfx.fillRect(cx - t, y + t / 2, t * 2, q / 2, color);
      gfx.fillRect(cx - t, y + size - q / 2 - t / 2, t * 2, q / 2, color);
      gfx.fillRect(x + t / 2, cy - t, q / 2, t * 2, color);
      gfx.fillRect(x + size - q / 2 - t / 2, cy - t, q / 2, t * 2, color);
      ring(gfx, cx, cy, q / 2 + t, t, color);
      break;
    }
    case IconId::Tools: {
      const int16_t cell = (size - q) / 2 - t;
      gfx.fillRoundRect(x + q / 2, y + q / 2, cell, cell, t, color);
      gfx.fillRoundRect(cx + t / 2, y + q / 2, cell, cell, t, color);
      gfx.fillRoundRect(x + q / 2, cy + t / 2, cell, cell, t, color);
      strokeRect(gfx, cx + t / 2, cy + t / 2, cell, cell, t, t, color);
      break;
    }
  }
}

}  // namespace icons
