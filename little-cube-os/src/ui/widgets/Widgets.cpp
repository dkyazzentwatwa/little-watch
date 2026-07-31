#include "Widgets.h"

#include <Arduino_GFX_Library.h>

// Adafruit_GFX's Free* faces. Already on the arduino-cli --library list, and
// binary-compatible with Arduino_GFX's setFont(const GFXfont*) — both use the
// same GFXfont struct behind the shared _GFXFONT_H_ guard.
//
// Adafruit_GFX.h is included ONLY to register the library with arduino-cli's
// dependency scanner: it resolves libraries by header name, so without this it
// never adds the library root to the include path and <Fonts/...> fails.
#include <Adafruit_GFX.h>

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include "../../board_config.h"
#include "../Theme.h"

namespace widgets {

namespace {
// Built-in GFX font cell, still used by the legacy textBlock overload that
// unmigrated screens call.
constexpr int16_t kCharW = 6;
constexpr int16_t kCharH = 8;

const GFXfont* fontFor(TextStyle style) {
  switch (style) {
    case TextStyle::Display: return &FreeSansBold24pt7b;
    case TextStyle::Title: return &FreeSansBold18pt7b;
    case TextStyle::Body: return &FreeSans12pt7b;
    case TextStyle::Caption: return &FreeSans9pt7b;
  }
  return &FreeSans12pt7b;
}

void applyStyle(Arduino_GFX& gfx, TextStyle style) {
  gfx.setFont(fontFor(style));
  gfx.setTextSize(1);
}

// Every helper leaves the canvas on the built-in font, so a screen that has
// not been migrated yet keeps its top-left cursor semantics.
void restoreFont(Arduino_GFX& gfx) {
  gfx.setFont(nullptr);
}

// Cap height per style, measured once from the font data itself so it can
// never drift from the face. "H" has no descender and reaches the cap line.
int16_t capHeight(Arduino_GFX& gfx, TextStyle style) {
  static int16_t cache[4] = {0, 0, 0, 0};
  const uint8_t slot = static_cast<uint8_t>(style);
  if (cache[slot] == 0) {
    applyStyle(gfx, style);
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    gfx.getTextBounds("H", 0, 0, &x1, &y1, &w, &h);
    cache[slot] = static_cast<int16_t>(-y1);  // y1 is negative: baseline -> top
    restoreFont(gfx);
  }
  return cache[slot];
}

// Ink box for a string at the given style. x1 matters for exact centering:
// the first glyph's ink usually starts a pixel or two right of the cursor.
void inkBounds(Arduino_GFX& gfx, const char* s, TextStyle style, int16_t& x1, uint16_t& w) {
  applyStyle(gfx, style);
  // getTextBounds() APPLIES WRAPPING. With wrap left on (the default), any
  // string wider than the canvas comes back folded, so the reported width is
  // the width of the LAST wrapped line — smaller than the truth, silently.
  //
  // That is a measurement returning a number that looks plausible while being
  // wrong in the unsafe direction: footer()'s ellipsis clamp and every
  // centering helper would conclude an over-long string fits. Measure with
  // wrapping off; the caller decides what to do about strings that don't fit.
  gfx.setTextWrap(false);
  int16_t y1 = 0;
  uint16_t h = 0;
  gfx.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx.setTextWrap(true);
  restoreFont(gfx);
}

// Copies s into out, dropping trailing characters and appending an ellipsis
// until it fits maxW. Captions are short and this runs at most once per frame
// per string, so a linear measure loop beats carrying a width cache.
void fitWithEllipsis(Arduino_GFX& gfx, const char* s, int16_t maxW, TextStyle style,
                     char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  out[0] = '\0';
  if (s == nullptr || s[0] == '\0' || maxW <= 0) {
    return;
  }
  const size_t srcLen = strlen(s);
  size_t len = srcLen < outLen - 1 ? srcLen : outLen - 1;
  memcpy(out, s, len);
  out[len] = '\0';
  if (textWidth(gfx, out, style) <= maxW) {
    return;  // fits verbatim, no ellipsis needed
  }

  // Shave characters off the tail and append "..." until it fits. Cap the
  // starting length so len + "..." + NUL never exceeds outLen, however small
  // the caller's buffer is.
  const size_t maxWithEllipsis = outLen > 4 ? outLen - 4 : 0;  // "..." + NUL
  if (len > maxWithEllipsis) {
    len = maxWithEllipsis;
  }
  while (len > 0) {
    out[len] = '\0';
    strcat(out, "...");  // safe: len + 3 + 1 <= outLen by construction above
    if (textWidth(gfx, out, style) <= maxW) {
      return;
    }
    len--;
  }
  // Nothing fits alongside an ellipsis (maxW too small even for "..."):
  // leave the field blank rather than draw a fragment that reads as a
  // rendering fault.
  out[0] = '\0';
}

}  // namespace

void text(Arduino_GFX& gfx, int16_t x, int16_t topY, const char* s, TextStyle style,
          uint16_t color) {
  if (s == nullptr || s[0] == '\0') {
    return;
  }
  const int16_t baseline = topY + capHeight(gfx, style);
  applyStyle(gfx, style);
  gfx.setTextColor(color);
  gfx.setCursor(x, baseline);
  gfx.print(s);
  restoreFont(gfx);
}

void textCentered(Arduino_GFX& gfx, int16_t x, int16_t topY, int16_t w, const char* s,
                  TextStyle style, uint16_t color) {
  if (s == nullptr || s[0] == '\0') {
    return;
  }
  int16_t x1 = 0;
  uint16_t inkW = 0;
  inkBounds(gfx, s, style, x1, inkW);
  // Cancel the glyph's own left bearing so the INK is centred, not the cursor.
  const int16_t cursorX = x + (w - static_cast<int16_t>(inkW)) / 2 - x1;
  text(gfx, cursorX, topY, s, style, color);
}

void textRight(Arduino_GFX& gfx, int16_t rightX, int16_t topY, const char* s,
               TextStyle style, uint16_t color) {
  if (s == nullptr || s[0] == '\0') {
    return;
  }
  int16_t x1 = 0;
  uint16_t inkW = 0;
  inkBounds(gfx, s, style, x1, inkW);
  text(gfx, rightX - static_cast<int16_t>(inkW) - x1, topY, s, style, color);
}

int16_t textWidth(Arduino_GFX& gfx, const char* s, TextStyle style) {
  if (s == nullptr || s[0] == '\0') {
    return 0;
  }
  int16_t x1 = 0;
  uint16_t inkW = 0;
  inkBounds(gfx, s, style, x1, inkW);
  return static_cast<int16_t>(inkW);
}

int16_t lineHeight(TextStyle style) {
  return static_cast<int16_t>(fontFor(style)->yAdvance);
}

int16_t ascent(Arduino_GFX& gfx, TextStyle style) {
  return capHeight(gfx, style);
}

int16_t header(Arduino_GFX& gfx, const char* title, int16_t shiftX, int16_t shiftY) {
  const int16_t top = theme::kStatusBarHeight + 10 + shiftY;
  text(gfx, theme::kPadding + shiftX, top, title, TextStyle::Title, theme::kText);
  const int16_t ruleY = top + capHeight(gfx, TextStyle::Title) + 10;
  gfx.drawFastHLine(theme::kPadding + shiftX, ruleY, DISPLAY_WIDTH - 2 * theme::kPadding,
                    theme::kPanelAlt);
  return ruleY + 12;
}

int16_t footer(Arduino_GFX& gfx, const char* left, const char* right, int16_t shiftX,
               int16_t shiftY) {
  // Rule at -44 rather than the -28 the old call sites used: caption ink then
  // ends ~19 px above the bottom edge. That is the working assumption for
  // clearing the corner radius the old -28 footers visibly clipped against —
  // kSafeInset = 20 is asserted in prose at Theme.h:61, not a measured
  // hardware constant, so treat this as pending on-device confirmation.
  //
  // shiftX/shiftY (burn-in drift, spec §37) apply on top of both insets below
  // rather than being clamped — clamping would defeat the drift. In the worst
  // quadrant (shiftX = -2, shiftY = +2) the effective inset is 18px/17px, not
  // the nominal 20/19 the comments above and below reason about.
  const int16_t ruleY = DISPLAY_HEIGHT - 44 + shiftY;
  // The rule aligns with header()'s rule (kPadding .. width - kPadding) so the
  // two hairlines share an inset. The caption TEXT sits further in, at
  // kSafeInset, because it lives only 19px off the bottom edge — inside the
  // corner-radius zone the rule itself, 44px up, does not reach.
  const int16_t ruleLeftX = theme::kPadding + shiftX;
  const int16_t ruleRightX = DISPLAY_WIDTH - theme::kPadding + shiftX;
  const int16_t textLeftX = theme::kSafeInset + shiftX;
  const int16_t textRightX = DISPLAY_WIDTH - theme::kSafeInset + shiftX;
  gfx.drawFastHLine(ruleLeftX, ruleY, ruleRightX - ruleLeftX, theme::kPanelAlt);

  const int16_t textTop = ruleY + 8;
  const bool hasLeft = left != nullptr && left[0] != '\0';
  const bool hasRight = right != nullptr && right[0] != '\0';

  // Right wins the space, left yields: right holds action hints ("swipe:
  // next", "hold: refresh") the user needs to operate the screen, while left
  // holds status text (filenames, "3-8 of 24") that degrades fine when
  // shortened. Measure right first — if the whole band can't hold it, drop
  // it outright rather than half-draw it, same failure mode as before.
  const int16_t bandW = textRightX - textLeftX;
  int16_t rightW = hasRight ? textWidth(gfx, right, TextStyle::Caption) : 0;
  const bool drawRight = hasRight && rightW <= bandW;
  if (!drawRight) {
    rightW = 0;
  }

  // Whatever's left of the band, minus the 12px separation gap when a right
  // string actually claimed space, is the left string's budget. It's drawn
  // verbatim if it fits, ellipsized down to the budget if it doesn't, and
  // skipped entirely if the right hint alone ate the whole band.
  const int16_t leftBudget = (rightW > 0 ? textRightX - rightW - 12 : textRightX) - textLeftX;
  if (hasLeft && leftBudget > 0) {
    if (textWidth(gfx, left, TextStyle::Caption) <= leftBudget) {
      text(gfx, textLeftX, textTop, left, TextStyle::Caption, theme::kTextDim);
    } else {
      char fitted[64];
      fitWithEllipsis(gfx, left, leftBudget, TextStyle::Caption, fitted, sizeof(fitted));
      text(gfx, textLeftX, textTop, fitted, TextStyle::Caption, theme::kTextDim);
    }
  }
  if (drawRight) {
    textRight(gfx, textRightX, textTop, right, TextStyle::Caption, theme::kTextDim);
  }
  return ruleY - 12;
}

Rect button(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, int16_t h, const char* label,
            bool emphasized) {
  const uint16_t fill = emphasized ? theme::kAccent : theme::kPanel;
  const uint16_t ink = emphasized ? theme::kBg : theme::kText;
  gfx.fillRoundRect(x, y, w, h, theme::kCardRadius / 2, fill);
  const int16_t cap = capHeight(gfx, TextStyle::Body);
  textCentered(gfx, x, y + (h - cap) / 2, w, label, TextStyle::Body, ink);
  return Rect{x, y, w, h};
}

Rect listItem(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, const char* primary,
              const char* secondary, bool selected) {
  const int16_t h = secondary != nullptr ? 58 : 44;
  if (selected) {
    gfx.fillRoundRect(x, y, w, h, 8, theme::kPanelAlt);
  }
  text(gfx, x + 10, y + 9, primary, TextStyle::Body, theme::kText);
  if (secondary != nullptr) {
    text(gfx, x + 10, y + 34, secondary, TextStyle::Caption, theme::kTextDim);
  }
  return Rect{x, y, w, h};
}

int16_t textBlock(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, const char* body,
                  TextStyle style, uint16_t color, uint8_t maxLines) {
  if (body == nullptr || w <= 0) {
    return y;
  }
  // Greedy wrap measured against real glyph widths — proportional faces break
  // the fixed-cell "chars per line" arithmetic the uint8_t overload uses.
  const int16_t lineH = lineHeight(style);
  const size_t len = strlen(body);
  size_t pos = 0;
  uint8_t drawn = 0;
  char line[128];

  while (pos < len && drawn < maxLines) {
    size_t take = 0;
    size_t lastSpace = 0;
    size_t fits = 0;
    while (pos + take < len && body[pos + take] != '\n' && take < sizeof(line) - 1) {
      take++;
      memcpy(line, body + pos, take);
      line[take] = '\0';
      if (textWidth(gfx, line, style) > w) {
        break;
      }
      fits = take;
      if (body[pos + take] == ' ') {
        lastSpace = take;
      }
    }
    // Prefer a word break, but never stall: a single word wider than w is cut.
    size_t lineLen = fits;
    if (pos + fits < len && body[pos + fits] != '\n' && body[pos + fits] != ' ' &&
        lastSpace > 0) {
      lineLen = lastSpace;
    }
    if (lineLen == 0) {
      lineLen = fits > 0 ? fits : 1;
    }
    memcpy(line, body + pos, lineLen);
    line[lineLen] = '\0';
    text(gfx, x, y, line, style, color);
    y += lineH;
    drawn++;

    pos += lineLen;
    while (pos < len && body[pos] == ' ') {
      pos++;
    }
    if (pos < len && body[pos] == '\n') {
      pos++;
    }
  }
  return y;
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
  const int16_t tw = textWidth(gfx, message, TextStyle::Body);
  const int16_t w = tw + 36;
  const int16_t h = 44;
  const int16_t x = (DISPLAY_WIDTH - w) / 2;
  const int16_t y = DISPLAY_HEIGHT - h - 16;
  gfx.fillRoundRect(x, y, w, h, h / 2, theme::kPanelAlt);
  textCentered(gfx, x, y + (h - capHeight(gfx, TextStyle::Body)) / 2, w, message,
               TextStyle::Body, theme::kText);
}

Rect modalConfirm(Arduino_GFX& gfx, const char* title, const char* body, Rect& cancelOut) {
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  const int16_t h = 220;
  const int16_t x = theme::kPadding;
  const int16_t y = (DISPLAY_HEIGHT - h) / 2;

  gfx.fillRoundRect(x, y, w, h, theme::kCardRadius, theme::kPanelAlt);
  text(gfx, x + 16, y + 18, title, TextStyle::Body, theme::kText);
  textBlock(gfx, x + 16, y + 56, w - 32, body, TextStyle::Caption, theme::kTextDim, 4);

  const int16_t bw = (w - 48) / 2;
  const int16_t by = y + h - 64;
  cancelOut = button(gfx, x + 16, by, bw, 48, "Cancel", false);
  return button(gfx, x + 32 + bw, by, bw, 48, "Confirm", true);
}

}  // namespace widgets
