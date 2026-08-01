#include "ClockFaces.h"

#include <Arduino_GFX_Library.h>
#include <ctype.h>
#include <string.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "Theme.h"
#include "widgets/Widgets.h"

// Clock faces (spec §12). Each renderer draws one screenful between the
// status bar (bottom edge y = kStatusBarHeight) and the footer rule
// (y = DISPLAY_HEIGHT - 44, which reaches 402 at shiftY -2), and reports how
// soon it wants its next frame.
//
// GEOMETRY IS MEASURED, NOT EYEBALLED. Every constant below was checked
// against the real GFXfont tables via the same charBounds/getTextBounds
// arithmetic Arduino_GFX uses — over all five burn-in shift quadrants and
// exhaustive string sets: all 1441 clock readings, all 2604 date strings, all
// 144 word phrases. Arduino_GFX clips silently at the canvas edge, so an
// overflow never crashes; it just collides with the status bar or the footer.
// Recheck the extents if you move any of these.
//
// One trap if you do re-measure: Arduino_GFX::getTextBounds() applies text
// wrapping internally (wrap defaults to true), so measuring a string wider
// than the 367px canvas returns a wrapped — and wrong — width. Measure with
// wrapping disabled, or you will understate the widest word phrase by 20px.

namespace clockfaces {

namespace {

const char* kWeekdays[7] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                            "Thursday", "Friday", "Saturday"};
const char* kMonths[12] = {"January", "February", "March",     "April",   "May",      "June",
                           "July",    "August",   "September", "October", "November", "December"};

// 12-hour word form; index 0 is twelve, matching tm_hour % 12.
const char* kHourWords[12] = {"twelve", "one", "two",   "three", "four", "five",
                              "six",    "seven", "eight", "nine",  "ten",  "eleven"};

// The short label is for the Word face, whose Title-sized phrase has no room
// for the recovery half; every Caption slot gets the actionable version, which
// measures 316px against 328px of available width.
constexpr const char* kNoTime = "time not set";
constexpr const char* kNoTimeHint = "time not set - connect Wi-Fi or `time set`";

// A corrupt RTC read reaches these renderers as an ordinary struct tm — there
// is no "invalid" bit to test — so every field is range-checked at the point
// of use rather than trusted. snprintf would not overflow, but "%02d" on a
// garbage tm_hour renders as garbage on the panel.
int safeHour(const struct tm* t) {
  return (t->tm_hour >= 0 && t->tm_hour <= 23) ? t->tm_hour : 0;
}
int safeMinute(const struct tm* t) {
  return (t->tm_min >= 0 && t->tm_min <= 59) ? t->tm_min : 0;
}

// "HH:MM", or "--:--" when the clock has never been set.
void formatTime(const struct tm* t, char* out, size_t len) {
  if (t == nullptr) {
    snprintf(out, len, "--:--");
    return;
  }
  snprintf(out, len, "%02d:%02d", safeHour(t), safeMinute(t));
}

// "Sunday, July 31", or the recovery hint when the clock is unset.
void formatDate(const struct tm* t, char* out, size_t len) {
  if (t == nullptr) {
    snprintf(out, len, "%s", kNoTimeHint);
    return;
  }
  const int wday = (t->tm_wday >= 0 && t->tm_wday <= 6) ? t->tm_wday : 0;
  const int mon = (t->tm_mon >= 0 && t->tm_mon <= 11) ? t->tm_mon : 0;
  snprintf(out, len, "%s, %s %d", kWeekdays[wday], kMonths[mon], t->tm_mday);
}

// ---------------------------------------------------------------------------
// BigDigital — the default. Time centred, date under it.
//
// Measured envelope over every string and shift: x[24..343] y[148..229].
// The widest date, "Wednesday, September 12" at 218px (several dates tie on
// that width), and the 316px recovery hint both clear the safe inset.
uint32_t renderBigDigital(Arduino_GFX& gfx, const FaceContext& ctx) {
  constexpr int16_t kTimeTop = 150;
  constexpr int16_t kDateGap = 28;

  char big[8];
  formatTime(ctx.time, big, sizeof(big));
  // Dim, never pure white: spec §37 names the fixed white max-brightness
  // clock as exactly the thing that burns a panel in.
  widgets::textCentered(gfx, ctx.shiftX, kTimeTop + ctx.shiftY, DISPLAY_WIDTH, big,
                        widgets::TextStyle::Display, theme::kTextDim);

  char date[48];
  formatDate(ctx.time, date, sizeof(date));
  const int16_t dateTop =
      kTimeTop + widgets::ascent(gfx, widgets::TextStyle::Display) + kDateGap;
  widgets::textCentered(gfx, ctx.shiftX, dateTop + ctx.shiftY, DISPLAY_WIDTH, date,
                        widgets::TextStyle::Caption,
                        ctx.time != nullptr ? theme::kTextDim : theme::kWarn);
  return kFaceStatic;
}

// ---------------------------------------------------------------------------
// Stacked — hour over minute, flush left, date parked at the bottom.
//
// Measured envelope: x[18..338] y[94..348]. The 84px stride between hour and
// minute is deliberately wider than Display's 56px yAdvance — the two numbers
// need to read as separate rows, not as a wrapped paragraph.
uint32_t renderStacked(Arduino_GFX& gfx, const FaceContext& ctx) {
  constexpr int16_t kHourTop = 96;
  constexpr int16_t kRowStride = 84;
  constexpr int16_t kDateTop = 330;
  const int16_t x = theme::kSafeInset + ctx.shiftX;

  char hh[4];
  char mm[4];
  if (ctx.time != nullptr) {
    snprintf(hh, sizeof(hh), "%02d", safeHour(ctx.time));
    snprintf(mm, sizeof(mm), "%02d", safeMinute(ctx.time));
  } else {
    snprintf(hh, sizeof(hh), "--");
    snprintf(mm, sizeof(mm), "--");
  }
  widgets::text(gfx, x, kHourTop + ctx.shiftY, hh, widgets::TextStyle::Display,
                theme::kTextDim);
  widgets::text(gfx, x, kHourTop + kRowStride + ctx.shiftY, mm, widgets::TextStyle::Display,
                theme::kTextDim);

  char date[48];
  formatDate(ctx.time, date, sizeof(date));
  widgets::text(gfx, x, kDateTop + ctx.shiftY, date, widgets::TextStyle::Caption,
                ctx.time != nullptr ? theme::kTextDim : theme::kWarn);
  return kFaceStatic;
}

// ---------------------------------------------------------------------------
// Word — the time spelled out, rounded to the nearest five minutes.
//
// Bucket = ((min + 2) / 5) % 13. Bucket 0 (minutes 00-02) is "<hour>
// o'clock"; bucket 12 (minutes 58-59) is the same form with the hour rolled
// forward, which is why the two are separate cases below.
void wordPhrase(const struct tm* t, char* out, size_t len) {
  if (t == nullptr) {
    snprintf(out, len, "%s", kNoTime);
    return;
  }
  const int h12 = safeHour(t) % 12;
  const int next = (h12 + 1) % 12;
  const int bucket = ((safeMinute(t) + 2) / 5) % 13;

  static const char* kPast[5] = {"five past", "ten past", "quarter past", "twenty past",
                                 "twenty-five past"};
  static const char* kTo[5] = {"twenty-five to", "twenty to", "quarter to", "ten to",
                               "five to"};

  if (bucket == 0) {
    snprintf(out, len, "%s o'clock", kHourWords[h12]);
  } else if (bucket <= 5) {
    snprintf(out, len, "%s %s", kPast[bucket - 1], kHourWords[h12]);
  } else if (bucket == 6) {
    snprintf(out, len, "half past %s", kHourWords[h12]);
  } else if (bucket <= 11) {
    snprintf(out, len, "%s %s", kTo[bucket - 7], kHourWords[next]);
  } else {
    snprintf(out, len, "%s o'clock", kHourWords[next]);
  }
}

// Measured across all 144 phrases: the widest, "twenty-five past eleven", is
// 383px in Title against 328px of available width, so it wraps to two lines
// rather than clipping ("...twelve" is 382px — close, but not a tie). Nothing
// reaches three lines; the maxLines cap below is a hard stop so a future
// phrase cannot walk into the footer. Envelope including the unset-clock
// hint: x[19..349] y[118..246].
uint32_t renderWord(Arduino_GFX& gfx, const FaceContext& ctx) {
  constexpr int16_t kTop = 120;
  constexpr int16_t kHintTop = 232;
  constexpr uint8_t kMaxLines = 3;
  const int16_t x = theme::kSafeInset + ctx.shiftX;
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kSafeInset;

  char phrase[48];
  wordPhrase(ctx.time, phrase, sizeof(phrase));
  widgets::textBlock(gfx, x, kTop + ctx.shiftY, w, phrase, widgets::TextStyle::Title,
                     ctx.time != nullptr ? theme::kTextDim : theme::kWarn, kMaxLines);
  if (ctx.time == nullptr) {
    // The Title phrase carries only the short label here, so the recovery
    // pointer gets its own Caption line rather than being dropped.
    widgets::text(gfx, x, kHintTop + ctx.shiftY, kNoTimeHint, widgets::TextStyle::Caption,
                  theme::kWarn);
  }
  return kFaceStatic;
}

// ===========================================================================
// ASCII faces: Block, Prompt, Segment, Binary.
//
// THESE FOUR USE THE BUILT-IN 6x8 BITMAP FONT ON PURPOSE, and are the only
// screens left in the firmware that do. ASCII art is a fixed grid: every glyph
// has to occupy an identical cell or '_' and '|' stop lining up and the digits
// fall apart. The proportional Free* faces the rest of the UI migrated to
// cannot do that. Each renderer calls setFont(nullptr) itself rather than
// trusting whatever the previous widget left behind.
//
// THE GRID. With the built-in font, setCursor() takes the TOP-LEFT (not the
// baseline, as it does under a GFXfont); a cell at setTextSize(sx, sy)
// advances 6*sx across and 8*sy down; and the glyph inks the leftmost 5*sx of
// that cell — the 6th sub-column is inter-character space, left as background
// because none of this draws with an opaque text background. So a run of n
// characters occupies 6*n*sx by 8*sy pixels from the cursor. That arithmetic
// is the whole layout system below, and it is what the extents in each
// renderer's comment were computed from.
//
// GLYPH CHOICES, read out of GFX_Library_for_Arduino's src/font/glcdfont.h
// rather than assumed. That table is 256 CP437-ish entries and drawChar()
// indexes it with an unsigned char with no range test, so every code 0x00-0xFF
// is drawable — but only three of the high ones are actually useful here:
//   0xDB  Full block: all five columns 0xFF, i.e. every pixel of the 5x8 cell.
//         Vertically contiguous, so stacked rows merge into solid shapes.
//         Horizontally the usual one sub-column gap remains, which is what
//         keeps the character grid readable as a grid.
//   0xB0  Light shade: 12 lit pixels of 40, in the same cell footprint as
//         0xDB. That identical footprint is the point — filled and hollow
//         marks stay on the grid while differing ~3x in ink.
//   0xB3  The solid full-height vertical, and the only glyph in the table that
//         is one full 0xFF column and nothing else. It is NOT '|' (0x7C):
//         this font's '|' is a BROKEN bar, rows 3 and 7 of its 8 blank, so a
//         seven-segment vertical spanning two stacked cells renders as four
//         dashes instead of one bar — which reads as a display fault, not as
//         type. 0xB3 inks sub-column 3 where '|' inks sub-column 2, so it is
//         drawn one sub-column (sizeX px) further left, landing the bar
//         exactly where the ASCII '|' would have been and keeping each digit
//         symmetric about its middle column.
// ===========================================================================

constexpr uint8_t kGlyphBlock = 0xDB;  // full block
constexpr uint8_t kGlyphShade = 0xB0;  // light shade
constexpr uint8_t kGlyphBar = 0xB3;    // solid vertical, see note above

// kNoTimeHint is 42 characters. At the smallest built-in size these faces use
// (2 -> a 12x16 cell) that is 504px against a 368px canvas; size 1 would fit
// but a 6x8 cell is not readable at arm's length on a 1.8" panel. So the ASCII
// faces split the hint across two lines instead, in the case each face uses.
constexpr const char* kHintUpper1 = "TIME NOT SET";
constexpr const char* kHintUpper2 = "CONNECT WI-FI OR `TIME SET`";  // 27 chars
constexpr const char* kHintLower1 = "time not set";
constexpr const char* kHintLower2 = "connect wi-fi or `time set`";

// --- built-in-font primitives ---------------------------------------------

void asciiBegin(Arduino_GFX& gfx, uint8_t sizeX, uint8_t sizeY, uint16_t color) {
  gfx.setFont(nullptr);
  gfx.setTextSize(sizeX, sizeY);
  gfx.setTextColor(color);
  // Wrapping is on by default and every widget helper restores it that way. A
  // wrapped ASCII row would silently fold onto the line below instead of
  // clipping, so it is off for the duration of a face and restored on the way
  // out.
  gfx.setTextWrap(false);
}

// Leaves the canvas the way widgets:: expects to find it.
void asciiEnd(Arduino_GFX& gfx) {
  gfx.setTextSize(1);
  gfx.setTextWrap(true);
  gfx.setFont(nullptr);
}

void asciiAt(Arduino_GFX& gfx, int16_t x, int16_t y, const char* s) {
  gfx.setCursor(x, y);
  gfx.print(s);
}

// Advance width of n characters, which is also the box the layout reserves.
int16_t cellSpan(size_t chars, uint8_t sizeX) {
  return static_cast<int16_t>(6 * sizeX * chars);
}

void asciiCentered(Arduino_GFX& gfx, int16_t shiftX, int16_t y, const char* s,
                   uint8_t sizeX) {
  const int16_t w = cellSpan(strlen(s), sizeX);
  asciiAt(gfx, static_cast<int16_t>((DISPLAY_WIDTH - w) / 2 + shiftX), y, s);
}

// "SUNDAY, JULY 31" — the caps form Block and Segment park under the digits.
// Longest is "WEDNESDAY, SEPTEMBER 12" at 23 characters.
void formatDateUpper(const struct tm* t, char* out, size_t len) {
  formatDate(t, out, len);
  for (size_t i = 0; out[i] != '\0' && i < len; ++i) {
    out[i] = static_cast<char>(toupper(static_cast<unsigned char>(out[i])));
  }
}

// --- the shared HH:MM character grid ---------------------------------------
//
// Block and Segment lay their digits on the same 17-column map, so both are
// 306px wide at sizeX 3 and both centre at x = 31:
//   cols 0-2 = tens of hours, 4-6 = units, 8 = separator, 10-12 and 14-16 =
//   minutes; columns 3, 7, 9 and 13 are the gaps.
constexpr uint8_t kGridCols = 17;
constexpr uint8_t kDigitCol[4] = {0, 4, 10, 14};
constexpr uint8_t kSepCol = 8;
constexpr uint8_t kGridSizeX = 3;
constexpr int16_t kGridLeft = (DISPLAY_WIDTH - 6 * kGridSizeX * kGridCols) / 2;  // 31

// Index into the digit tables: 0-9, or 10 for the '-' the unset clock shows.
void gridDigits(const struct tm* t, uint8_t out[4]) {
  if (t == nullptr) {
    out[0] = out[1] = out[2] = out[3] = 10;
    return;
  }
  const int h = safeHour(t);
  const int m = safeMinute(t);
  out[0] = static_cast<uint8_t>(h / 10);
  out[1] = static_cast<uint8_t>(h % 10);
  out[2] = static_cast<uint8_t>(m / 10);
  out[3] = static_cast<uint8_t>(m % 10);
}

// Stamps one row of the four digits plus the separator into a 17-column
// buffer. `art` rows are 3 characters wide; `sep` has one character per row.
// Templated on the row count only so Block's five-row art and Segment's
// three-row art can share this without either table losing its shape.
template <int Rows>
void gridRow(char* out, const char* const art[11][Rows], const uint8_t digits[4],
             const char* sep, uint8_t row) {
  memset(out, ' ', kGridCols);
  out[kGridCols] = '\0';
  for (uint8_t i = 0; i < 4; ++i) {
    memcpy(out + kDigitCol[i], art[digits[i]][row], 3);
  }
  out[kSepCol] = sep[row];
}

// ---------------------------------------------------------------------------
// Block — the time as 3x5 block-character digits, date beneath in caps.
//
// Grid: 17 cols x 5 rows at setTextSize(3, 3), i.e. an 18x24 cell, so the
// block runs 306x120 from (31, 140). Every cell of every row is either a full
// 0xDB or blank, so the drawn extent is exactly the grid: x[31..333]
// y[140..259], and the date's widest form ("WEDNESDAY, SEPTEMBER 12", 23
// chars, 276px) centres to x[46..321] at y[296..311]. Shifts move all of that
// by at most 2px. Static — nothing here moves between minutes.
const char* const kBlockArt[11][5] = {
    {"###", "# #", "# #", "# #", "###"},  // 0
    {" # ", "## ", " # ", " # ", "###"},  // 1
    {"###", "  #", "###", "#  ", "###"},  // 2
    {"###", "  #", "###", "  #", "###"},  // 3
    {"# #", "# #", "###", "  #", "  #"},  // 4
    {"###", "#  ", "###", "  #", "###"},  // 5
    {"###", "#  ", "###", "# #", "###"},  // 6
    {"###", "  #", "  #", "  #", "  #"},  // 7
    {"###", "# #", "###", "# #", "###"},  // 8
    {"###", "# #", "###", "  #", "###"},  // 9
    {"   ", "   ", "###", "   ", "   "},  // '-', for the unset clock
};
const char* kBlockSep = " # # ";  // the colon column, one character per row

uint32_t renderBlock(Arduino_GFX& gfx, const FaceContext& ctx) {
  constexpr int16_t kTop = 140;
  constexpr int16_t kRowH = 8 * 3;  // sizeY 3
  constexpr int16_t kDateTop = 296;
  constexpr int16_t kHintTop2 = 320;

  uint8_t digits[4];
  gridDigits(ctx.time, digits);

  asciiBegin(gfx, kGridSizeX, 3, theme::kTextDim);
  char row[kGridCols + 1];
  for (uint8_t r = 0; r < 5; ++r) {
    gridRow<5>(row, kBlockArt, digits, kBlockSep, r);
    for (uint8_t c = 0; c < kGridCols; ++c) {
      if (row[c] == '#') {
        row[c] = static_cast<char>(kGlyphBlock);
      }
    }
    asciiAt(gfx, static_cast<int16_t>(kGridLeft + ctx.shiftX),
            static_cast<int16_t>(kTop + r * kRowH + ctx.shiftY), row);
  }

  gfx.setTextSize(2, 2);
  if (ctx.time != nullptr) {
    char date[48];
    formatDateUpper(ctx.time, date, sizeof(date));
    gfx.setTextColor(theme::kTextDim);
    asciiCentered(gfx, ctx.shiftX, static_cast<int16_t>(kDateTop + ctx.shiftY), date, 2);
  } else {
    gfx.setTextColor(theme::kWarn);
    asciiCentered(gfx, ctx.shiftX, static_cast<int16_t>(kDateTop + ctx.shiftY), kHintUpper1, 2);
    asciiCentered(gfx, ctx.shiftX, static_cast<int16_t>(kHintTop2 + ctx.shiftY), kHintUpper2, 2);
  }
  asciiEnd(gfx);
  return kFaceStatic;
}

// ---------------------------------------------------------------------------
// Prompt — a fake shell session. The blinking cursor is the only motion in the
// entire face set.
//
// All lines are flush left at kSafeInset. At size 2 (12x16 cell) the widest
// line is the date, "Wednesday, September 12" at 23 chars = 276px, reaching
// x = 296; the unset-clock hint's second line is 27 chars = 324px, reaching
// x = 344. The time is size 7 (42x56), 5 chars = 210px. Envelope x[20..344]
// y[110..303] before shifts.
constexpr const char* kShellPrompt = "cube:~$";  // 7 chars; cursor sits at +8

uint32_t renderPrompt(Arduino_GFX& gfx, const FaceContext& ctx) {
  constexpr uint8_t kSize = 2;
  constexpr int16_t kCmdTop = 110;
  constexpr int16_t kTimeTop = 150;
  constexpr int16_t kDateTop = 228;
  constexpr int16_t kHintTop2 = 252;
  constexpr int16_t kPromptTop = 288;
  constexpr uint32_t kBlinkHalfMs = 500;  // 1 Hz: 500 ms lit, 500 ms dark
  const int16_t x = theme::kSafeInset + ctx.shiftX;
  const int16_t argX = static_cast<int16_t>(x + cellSpan(8, kSize));

  asciiBegin(gfx, kSize, kSize, theme::kAccent);
  asciiAt(gfx, x, static_cast<int16_t>(kCmdTop + ctx.shiftY), kShellPrompt);
  gfx.setTextColor(theme::kText);
  asciiAt(gfx, argX, static_cast<int16_t>(kCmdTop + ctx.shiftY), "date +%H:%M");

  char big[8];
  formatTime(ctx.time, big, sizeof(big));
  gfx.setTextSize(7, 7);
  gfx.setTextColor(theme::kTextDim);  // never kText: spec §37, this is the
                                      // largest persistent element on screen
  asciiAt(gfx, x, static_cast<int16_t>(kTimeTop + ctx.shiftY), big);

  gfx.setTextSize(kSize, kSize);
  if (ctx.time != nullptr) {
    char date[48];
    formatDate(ctx.time, date, sizeof(date));
    asciiAt(gfx, x, static_cast<int16_t>(kDateTop + ctx.shiftY), date);
  } else {
    gfx.setTextColor(theme::kWarn);
    asciiAt(gfx, x, static_cast<int16_t>(kDateTop + ctx.shiftY), kHintLower1);
    asciiAt(gfx, x, static_cast<int16_t>(kHintTop2 + ctx.shiftY), kHintLower2);
  }

  gfx.setTextColor(theme::kAccent);
  asciiAt(gfx, x, static_cast<int16_t>(kPromptTop + ctx.shiftY), kShellPrompt);
  const uint32_t phase = ctx.animMs % (2 * kBlinkHalfMs);
  if (phase < kBlinkHalfMs) {
    const char cursor[2] = {static_cast<char>(kGlyphBlock), '\0'};
    asciiAt(gfx, argX, static_cast<int16_t>(kPromptTop + ctx.shiftY), cursor);
  }
  asciiEnd(gfx);

  // Milliseconds until the cursor next changes state: 1..500, never 0.
  // Returning 0 would repaint the full 322 KB PSRAM canvas at 30 fps to
  // animate a 10x16 rectangle; this asks for ~2 frames a second instead, and
  // only while this face is on screen.
  return kBlinkHalfMs - (ctx.animMs % kBlinkHalfMs);
}

// ---------------------------------------------------------------------------
// Segment — seven-segment digits drawn from '_' and the solid vertical bar,
// three rows per digit, on the same 17-column map as Block.
//
// setTextSize(3, 5) — an 18x40 cell — rather than a square multiplier: at a
// uniform 3 the three rows are only 72px tall against 39px wide, which reads
// as squashed. At (3, 5) the digit ink is 39x90, close to a real seven-segment
// aspect. Where the ink lands inside a cell is glyph-dependent and is why the
// extents below are not simply the grid box: '_' inks row 6 of 8 (y+30..y+34),
// the bar inks all eight rows (y..y+39), '.' inks rows 5-6 (y+25..y+34).
// Envelope x[37..327] y[146..235] from a (31, 116) origin, date at y[280..295].
const char* const kSegArt[11][3] = {
    {" _ ", "| |", "|_|"},  // 0
    {"   ", "  |", "  |"},  // 1
    {" _ ", " _|", "|_ "},  // 2
    {" _ ", " _|", " _|"},  // 3
    {"   ", "|_|", "  |"},  // 4
    {" _ ", "|_ ", " _|"},  // 5
    {" _ ", "|_ ", "|_|"},  // 6
    {" _ ", "  |", "  |"},  // 7
    {" _ ", "|_|", "|_|"},  // 8
    {" _ ", "|_|", " _|"},  // 9
    {"   ", " _ ", "   "},  // '-'
};
const char* kSegSep = " ..";  // dot on the middle and bottom rows

uint32_t renderSegment(Arduino_GFX& gfx, const FaceContext& ctx) {
  constexpr uint8_t kSizeY = 5;
  constexpr int16_t kTop = 116;
  constexpr int16_t kRowH = 8 * kSizeY;
  constexpr int16_t kDateTop = 280;
  constexpr int16_t kHintTop2 = 304;

  uint8_t digits[4];
  gridDigits(ctx.time, digits);

  asciiBegin(gfx, kGridSizeX, kSizeY, theme::kTextDim);
  char row[kGridCols + 1];
  for (uint8_t r = 0; r < 3; ++r) {
    gridRow<3>(row, kSegArt, digits, kSegSep, r);
    const int16_t rowY = static_cast<int16_t>(kTop + r * kRowH + ctx.shiftY);
    for (uint8_t c = 0; c < kGridCols; ++c) {
      if (row[c] == ' ') {
        continue;
      }
      // Cell by cell rather than one print per row, because the bar carries a
      // one-sub-column nudge the cursor's own advance cannot express.
      const bool bar = (row[c] == '|');
      const int16_t cellX =
          static_cast<int16_t>(kGridLeft + ctx.shiftX + c * 6 * kGridSizeX);
      gfx.setCursor(bar ? static_cast<int16_t>(cellX - kGridSizeX) : cellX, rowY);
      gfx.write(bar ? kGlyphBar : static_cast<uint8_t>(row[c]));
    }
  }

  gfx.setTextSize(2, 2);
  if (ctx.time != nullptr) {
    char date[48];
    formatDateUpper(ctx.time, date, sizeof(date));
    asciiCentered(gfx, ctx.shiftX, static_cast<int16_t>(kDateTop + ctx.shiftY), date, 2);
  } else {
    gfx.setTextColor(theme::kWarn);
    asciiCentered(gfx, ctx.shiftX, static_cast<int16_t>(kDateTop + ctx.shiftY), kHintUpper1, 2);
    asciiCentered(gfx, ctx.shiftX, static_cast<int16_t>(kHintTop2 + ctx.shiftY), kHintUpper2, 2);
  }
  asciiEnd(gfx);
  return kFaceStatic;
}

// ---------------------------------------------------------------------------
// Binary — hours and minutes as rows of filled/hollow marks, with a decimal
// readout beneath so the face is never unreadable.
//
// 0xDB (full block) and 0xB0 (light shade) are the marks, rather than '#'/'.'
// or 'O'/'o': both occupy the identical 5x8 cell, so the two rows stay on a
// single grid and a mark never shifts position when its bit flips — only its
// density changes, ~100% against ~30%. Character marks would put a 5x7 glyph
// next to a 2x2 dot and the columns would stop reading as place values.
//
// Five marks for hours (0-23 needs five bits) and six for minutes (0-59 needs
// six), right-aligned to a common edge so the columns are true place values.
// A sixth, permanently-hollow hour bit would have looked like a fault.
// At setTextSize(6) — a 36x48 cell — the minute row is 216px wide; envelope
// x[46..321] (the date is the widest line) y[80..351].
uint32_t renderBinary(Arduino_GFX& gfx, const FaceContext& ctx) {
  constexpr uint8_t kMarkSize = 6;
  constexpr uint8_t kHourBits = 5;
  constexpr uint8_t kMinBits = 6;
  constexpr int16_t kMinX = (DISPLAY_WIDTH - 6 * kMarkSize * kMinBits) / 2;  // 76
  constexpr int16_t kHourX = kMinX + 6 * kMarkSize;                          // 112
  constexpr int16_t kHourLabelTop = 80;
  constexpr int16_t kHourRowTop = 100;
  constexpr int16_t kMinLabelTop = 166;
  constexpr int16_t kMinRowTop = 186;
  constexpr int16_t kReadoutTop = 270;
  constexpr int16_t kDateTop = 336;
  constexpr int16_t kHintTop2 = 360;

  const int h = ctx.time != nullptr ? safeHour(ctx.time) : 0;
  const int m = ctx.time != nullptr ? safeMinute(ctx.time) : 0;
  const bool set = ctx.time != nullptr;

  char hourRow[kHourBits + 1];
  for (uint8_t i = 0; i < kHourBits; ++i) {
    const bool on = set && ((h >> (kHourBits - 1 - i)) & 1) != 0;
    hourRow[i] = static_cast<char>(on ? kGlyphBlock : kGlyphShade);
  }
  hourRow[kHourBits] = '\0';

  char minRow[kMinBits + 1];
  for (uint8_t i = 0; i < kMinBits; ++i) {
    const bool on = set && ((m >> (kMinBits - 1 - i)) & 1) != 0;
    minRow[i] = static_cast<char>(on ? kGlyphBlock : kGlyphShade);
  }
  minRow[kMinBits] = '\0';

  asciiBegin(gfx, 2, 2, theme::kTextDim);
  asciiAt(gfx, static_cast<int16_t>(kHourX + ctx.shiftX),
          static_cast<int16_t>(kHourLabelTop + ctx.shiftY), "HOURS");
  asciiAt(gfx, static_cast<int16_t>(kMinX + ctx.shiftX),
          static_cast<int16_t>(kMinLabelTop + ctx.shiftY), "MINUTES");

  // One colour for both mark states: the glyphs already differ 3x in ink, and
  // brightening the set bits to kText would put a bright static pattern on the
  // longest-lived screen the device has (spec §37).
  gfx.setTextSize(kMarkSize, kMarkSize);
  asciiAt(gfx, static_cast<int16_t>(kHourX + ctx.shiftX),
          static_cast<int16_t>(kHourRowTop + ctx.shiftY), hourRow);
  asciiAt(gfx, static_cast<int16_t>(kMinX + ctx.shiftX),
          static_cast<int16_t>(kMinRowTop + ctx.shiftY), minRow);

  char readout[8];
  formatTime(ctx.time, readout, sizeof(readout));
  gfx.setTextSize(4, 4);
  asciiCentered(gfx, ctx.shiftX, static_cast<int16_t>(kReadoutTop + ctx.shiftY), readout, 4);

  gfx.setTextSize(2, 2);
  if (set) {
    char date[48];
    formatDateUpper(ctx.time, date, sizeof(date));
    asciiCentered(gfx, ctx.shiftX, static_cast<int16_t>(kDateTop + ctx.shiftY), date, 2);
  } else {
    gfx.setTextColor(theme::kWarn);
    asciiCentered(gfx, ctx.shiftX, static_cast<int16_t>(kDateTop + ctx.shiftY), kHintUpper1, 2);
    asciiCentered(gfx, ctx.shiftX, static_cast<int16_t>(kHintTop2 + ctx.shiftY), kHintUpper2, 2);
  }
  asciiEnd(gfx);
  return kFaceStatic;
}

}  // namespace

const char* name(FaceId id) {
  switch (id) {
    case FaceId::BigDigital: return "Digital";
    case FaceId::Stacked: return "Stacked";
    case FaceId::Word: return "Words";
    case FaceId::Block: return "Block";
    case FaceId::Prompt: return "Prompt";
    case FaceId::Segment: return "Segment";
    case FaceId::Binary: return "Binary";
  }
  return "Digital";
}

uint32_t render(Arduino_GFX& gfx, FaceId id, const FaceContext& ctx) {
  switch (id) {
    case FaceId::BigDigital: return renderBigDigital(gfx, ctx);
    case FaceId::Stacked: return renderStacked(gfx, ctx);
    case FaceId::Word: return renderWord(gfx, ctx);
    case FaceId::Block: return renderBlock(gfx, ctx);
    case FaceId::Prompt: return renderPrompt(gfx, ctx);
    case FaceId::Segment: return renderSegment(gfx, ctx);
    case FaceId::Binary: return renderBinary(gfx, ctx);
  }
  return renderBigDigital(gfx, ctx);
}

}  // namespace clockfaces
