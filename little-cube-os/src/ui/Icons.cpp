#include "Icons.h"

#include <Arduino_GFX_Library.h>

#include <string.h>

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

// cos/sin scaled to 127 for the 8 compass directions, divided back down by
// 128 wherever they're used. A table keeps the sun's rays symmetric and
// integer-only — no float trig in a render path.
constexpr int8_t kRayCos[8] = {127, 90, 0, -90, -127, -90, 0, 90};
constexpr int8_t kRaySin[8] = {0, 90, 127, 90, 0, -90, -127, -90};

void sunDisc(Arduino_GFX& gfx, int16_t cx, int16_t cy, int16_t r, int16_t t,
             uint16_t color) {
  gfx.fillCircle(cx, cy, r, color);
  const int16_t inner = r + t;
  const int16_t outer = r + t * 3;
  for (int i = 0; i < 8; i++) {
    const int16_t x0 = cx + static_cast<int16_t>(inner * kRayCos[i] / 128);
    const int16_t y0 = cy + static_cast<int16_t>(inner * kRaySin[i] / 128);
    const int16_t x1 = cx + static_cast<int16_t>(outer * kRayCos[i] / 128);
    const int16_t y1 = cy + static_cast<int16_t>(outer * kRaySin[i] / 128);
    // Each ray is a filled quad (two triangles) spanning perpendicular to
    // its own direction. A drawLine-per-offset loop was tried here and
    // rejected: offsetting along x only misses diagonal rays entirely
    // (Bresenham steps that touch only at corners render as a dotted
    // chain, not a solid band), and a perpendicular *line* offset still
    // hits the same problem once the perpendicular has magnitude sqrt(2).
    // A filled triangle has no such gap at any angle.
    //
    // The rendered band is 2*floor(127t/256) + 1 px, not t: t for odd t,
    // t-1 for even (56 and 64 render identical ray thickness despite t
    // going 3->4). Rounding half-up instead overshoots to t+1, which is
    // worse, so this is left alone rather than "fixed".
    //
    // Diagonal rays render 15-25% heavier than the axis rays (axis offset
    // is exact, diagonal offset is the same integer stepped along a
    // sqrt(2)-longer perpendicular). Stepping the diagonal offset down one
    // integer makes them ~24% *too thin* instead — the error is symmetric
    // and not fixable without subpixel coverage. Leave it.
    int16_t hx = static_cast<int16_t>(-kRaySin[i] * t / 256);
    int16_t hy = static_cast<int16_t>(kRayCos[i] * t / 256);
    if (hx == 0 && hy == 0) {
      // t == 2 (sizes 21-47): both offsets truncate to 0 and the quad
      // collapses to a zero-area line. Force a +-1 perpendicular step so
      // the ray stays a filled shape instead of vanishing.
      hx = static_cast<int16_t>(kRaySin[i] > 0 ? -1 : (kRaySin[i] < 0 ? 1 : 0));
      hy = static_cast<int16_t>(kRayCos[i] > 0 ? 1 : (kRayCos[i] < 0 ? -1 : 0));
    }
    gfx.fillTriangle(x0 + hx, y0 + hy, x0 - hx, y0 - hy, x1 - hx, y1 - hy, color);
    gfx.fillTriangle(x0 + hx, y0 + hy, x1 + hx, y1 + hy, x1 - hx, y1 - hy, color);
  }
}

// Three overlapping circles on a flat base. Shared by the cloudy, rain, snow
// and thunder glyphs so they read as one family.
void cloudPuff(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, uint16_t color) {
  const int16_t r = w / 4;
  const int16_t baseY = y + r + r / 2;
  // Crown radius is r + r/4, not the more dramatic r + r/3 it might look
  // like it wants: centring the crown so its top edge lands exactly on y
  // (required to stop it overflowing above the box) pushes its bottom edge
  // down too, and r + r/3 pushes that bottom edge past the box in the
  // WxPartlyCloudy call, which starts this helper higher inside its box
  // than the other five callers do.
  const int16_t crownR = r + r / 4;
  gfx.fillCircle(x + r, baseY, r, color);
  gfx.fillCircle(x + w - r - 1, baseY, r, color);  // w - r would land one past the right edge
  gfx.fillCircle(x + w / 2, y + crownR, crownR, color);
  // Height r + 1, not r: the shoulder circles reach row baseY + r, but a
  // fillRect of height r only covers rows baseY..baseY+r-1, leaving the
  // cloud's bottom row as two narrow nubs with a gap between them.
  gfx.fillRect(x + r, baseY, w - 2 * r, r + 1, color);
}

struct ConditionEntry {
  const char* name;
  IconId id;
};

// Exact matches against conditionFromWmo()'s closed output set. Matching is
// exact, so "Snow" and "Snow showers" cannot shadow each other and the order
// of this table carries no meaning. constexpr (rather than a function-local
// static const) keeps this in .rodata and lets the compiler enforce it, no
// __cxa_guard_acquire on every call.
constexpr ConditionEntry kConditionTable[] = {
    {"Clear", IconId::WxClear},
    {"Mostly clear", IconId::WxPartlyCloudy},
    {"Overcast", IconId::WxCloudy},
    {"Fog", IconId::WxFog},
    {"Drizzle", IconId::WxRain},
    {"Rain", IconId::WxRain},
    {"Showers", IconId::WxRain},
    {"Snow", IconId::WxSnow},
    {"Snow showers", IconId::WxSnow},
    {"Thunderstorm", IconId::WxThunder},
};

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
      strokeRect(gfx, x + q / 2, y + q / 2, size - q, size - q, t * 2, t, color);
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
    case IconId::WxClear: {
      // Measured floor: fits down to size 21, not the size 28 that
      // q + 3t <= size/2 alone would suggest. kRayCos/kRaySin hold 127, one
      // short of the /128 divisor they're used with, so every ray's reach
      // truncates ~1px short of its nominal radius, buying back the margin
      // that formula predicts is missing. Irrelevant at the sizes this app
      // actually uses.
      sunDisc(gfx, cx, cy, q, t, color);
      break;
    }
    case IconId::WxPartlyCloudy: {
      // sunDisc reaches r + 3t from its centre; with r = q - t that's q + 2t,
      // so the centre needs to sit at least q + 2t from the top-left corner.
      // Moved rather than shrunk, or the disc reads as a dot at small sizes.
      sunDisc(gfx, x + q + t * 2, y + q + t * 2, q - t, t, color);
      // cy - q/4 fits with zero bottom margin at 56/64/96 (luck, not a
      // guarantee) and does overflow at other sizes in this app's range, so
      // clamp against the same shoulder-circle-bottom bound cloudPuff itself
      // has to satisfy. At our three sizes cy - q/4 and cloudYMax are
      // exactly equal, so the ternary is inert only by coincidence — any
      // future change to q, stroke() or cloudW can flip it to actively
      // clamping and visibly move the cloud.
      const int16_t cloudW = size - q / 2;
      const int16_t cloudR = cloudW / 4;
      const int16_t cloudYMax = y + size - 1 - (2 * cloudR + cloudR / 2);
      const int16_t cloudY = (cy - q / 4 < cloudYMax) ? (cy - q / 4) : cloudYMax;
      cloudPuff(gfx, x + q / 2, cloudY, cloudW, color);
      break;
    }
    case IconId::WxCloudy: {
      cloudPuff(gfx, x, y + q / 2, size, color);
      break;
    }
    case IconId::WxRain: {
      cloudPuff(gfx, x, y, size, color);
      // Three slanted drops under the cloud.
      for (int i = 0; i < 3; i++) {
        const int16_t dx = x + q - t + static_cast<int16_t>(i) * q;
        for (int16_t o = 0; o < t; o++) {
          gfx.drawLine(dx + o, y + size - q, dx - q / 3 + o, y + size - 1, color);
        }
      }
      break;
    }
    case IconId::WxSnow: {
      cloudPuff(gfx, x, y, size, color);
      for (int i = 0; i < 3; i++) {
        const int16_t dx = x + q - t + static_cast<int16_t>(i) * q;
        gfx.fillCircle(dx, y + size - q / 2, t, color);
      }
      break;
    }
    case IconId::WxThunder: {
      cloudPuff(gfx, x, y, size, color);
      // Bolt: two triangles sharing the waist, so it reads at small sizes.
      const int16_t bx = cx;
      const int16_t by = y + size - q * 2;
      gfx.fillTriangle(bx + q / 2, by, bx - q / 2, by + q, bx + t, by + q, color);
      gfx.fillTriangle(bx + q / 2, by + q, bx - q / 3, y + size - 1, bx - t, by + q, color);
      break;
    }
    case IconId::WxFog: {
      cloudPuff(gfx, x, y, size, color);
      // Three staggered bars: fog is the cloud, sitting on the ground.
      // Anchored from the bottom so the stagger can't run past the box —
      // anchoring from the top (y + size - q) did, growing with size.
      for (int i = 0; i < 3; i++) {
        const int16_t by = y + size - t - static_cast<int16_t>(2 - i) * (t * 2);
        const int16_t inset = static_cast<int16_t>(i) * q / 2;
        gfx.fillRect(x + inset, by, size - inset * 2, t, color);
      }
      break;
    }
  }
}

IconId forCondition(const char* condition) {
  if (condition == nullptr) {
    return IconId::WxCloudy;
  }
  for (const ConditionEntry& e : kConditionTable) {
    if (strcmp(condition, e.name) == 0) {
      return e.id;
    }
  }
  // The service's own "Weather" unknown, or a hand-edited cache file.
  return IconId::WxCloudy;
}

}  // namespace icons
