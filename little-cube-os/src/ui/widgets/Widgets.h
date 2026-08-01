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

// ---------------------------------------------------------------------------
// Type system (docs/superpowers/specs/2026-07-26-ui-refresh-and-sound-design.md)
//
// Four roles mapped onto Adafruit_GFX's Free* faces. Arduino_GFX consumes the
// same GFXfont struct, and Adafruit_GFX is already passed to arduino-cli by
// scripts/build.sh, so this costs ~18 KB of flash and no new dependency.
//
// THE BASELINE RULE: with a GFXfont, Arduino_GFX's setCursor() takes the
// BASELINE, while the built-in font takes the top-left. Every helper below
// takes a TOP-LEFT topY and converts internally, so call sites keep the
// coordinates they already use. Each helper also restores setFont(nullptr)
// before returning — screens not yet migrated still render correctly with the
// built-in font.
enum class TextStyle : uint8_t {
  Display,  // FreeSansBold 24pt — clock face, one hero number per screen
  Title,    // FreeSansBold 18pt — screen headers, card names
  Body,     // FreeSans 12pt     — list rows, buttons, running text
  Caption,  // FreeSans 9pt      — hints, timestamps, secondary lines
};

void text(Arduino_GFX& gfx, int16_t x, int16_t topY, const char* s, TextStyle style,
          uint16_t color);
void textCentered(Arduino_GFX& gfx, int16_t x, int16_t topY, int16_t w, const char* s,
                  TextStyle style, uint16_t color);
void textRight(Arduino_GFX& gfx, int16_t rightX, int16_t topY, const char* s,
               TextStyle style, uint16_t color);

// Ink width in pixels. Proportional glyphs make the old strlen*6*size formula
// wrong, and centering depends on this being exact — hence the canvas.
int16_t textWidth(Arduino_GFX& gfx, const char* s, TextStyle style);
int16_t lineHeight(TextStyle style);
// Cap height: distance from the top-left the callers pass down to the baseline.
int16_t ascent(Arduino_GFX& gfx, TextStyle style);

// Shared screen header: title plus a hairline rule. Returns a padded
// content-start just below the rule, so screens lay their content out from
// there instead of each hardcoding a different top margin.
int16_t header(Arduino_GFX& gfx, const char* title, int16_t shiftX, int16_t shiftY);

// Shared bottom band, the mirror of header(): a hairline rule with left- and
// optionally right-aligned caption text. The caption text is inset by
// kSafeInset so nothing lands in the bezel's corner radius. `right` may be
// nullptr. `right` holds action hints and wins the available space; `left`
// (status text — filenames, "3-8 of 24", progress) yields, truncating with
// an ellipsis when the two would collide.
//
// TWO BUGS THIS EXISTS TO KILL, both of which every hand-placed footer had:
//   1. Caption text at x = kPadding (12) sits INSIDE the rounded corner at the
//      bottom of the panel and gets clipped — hence kSafeInset for the text.
//      The rule itself stays at kPadding: 44px up, it clears the radius, and
//      matching header() keeps the two hairlines aligned. The exact
//      clearance is a working assumption (kSafeInset is asserted in prose at
//      Theme.h:61, not measured against the physical bezel) pending
//      on-device confirmation.
//   2. Footers are persistent chrome and must drift with the burn-in offsets
//      (spec §37) — not one of them passed shiftX/shiftY.
// Returns the bottom of the caller's content budget, already padded clear of
// the rule — the mirror of header(), which returns a padded content-start.
int16_t footer(Arduino_GFX& gfx, const char* left, const char* right, int16_t shiftX,
               int16_t shiftY);

Rect button(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, int16_t h, const char* label,
            bool emphasized = false);
Rect listItem(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, const char* primary,
              const char* secondary, bool selected = false);
// Word-wrapped paragraph. The TextStyle form measures real glyph widths; the
// uint8_t form is the pre-font original, kept working while screens migrate.
// Both return the y below the last line drawn.
int16_t textBlock(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, const char* text,
                  TextStyle style, uint16_t color, uint8_t maxLines = 255);
void textBlock(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, const char* text,
               uint8_t textSize, uint16_t color);
// How many characters of `text` a textBlock() of the same width, style and
// line cap would consume. Shares textBlock's walker, so the two can never
// disagree about where a page ends. Draws nothing; the returned offset is the
// resume point for the next page (leading spaces and one newline already
// skipped, exactly as the drawing pass skips them).
size_t measureBlock(Arduino_GFX& gfx, const char* text, int16_t w, TextStyle style,
                    uint8_t maxLines);
void card(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, int16_t h);
void toast(Arduino_GFX& gfx, const char* message);
Rect modalConfirm(Arduino_GFX& gfx, const char* title, const char* body, Rect& cancelOut);

}  // namespace widgets
