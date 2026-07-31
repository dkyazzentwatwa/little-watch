#include "ClockFaces.h"

#include <Arduino_GFX_Library.h>

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

}  // namespace

const char* name(FaceId id) {
  switch (id) {
    case FaceId::BigDigital: return "Digital";
    case FaceId::Stacked: return "Stacked";
    case FaceId::Word: return "Words";
    case FaceId::Blinky: return "Blinky";
    case FaceId::BigEyes: return "Big Eyes";
    case FaceId::MoodCube: return "Mood Cube";
  }
  return "Digital";
}

uint32_t render(Arduino_GFX& gfx, FaceId id, const FaceContext& ctx) {
  switch (id) {
    case FaceId::BigDigital: return renderBigDigital(gfx, ctx);
    case FaceId::Stacked: return renderStacked(gfx, ctx);
    case FaceId::Word: return renderWord(gfx, ctx);
    // Blinky, BigEyes and MoodCube are named and selectable (SettingsService
    // already persists all six indices) but their renderers land in Task 6.
    // They fall back to the default face deliberately: an enum member that
    // rendered nothing would look like a crashed app, not a stub.
    case FaceId::Blinky:
    case FaceId::BigEyes:
    case FaceId::MoodCube: return renderBigDigital(gfx, ctx);
  }
  return renderBigDigital(gfx, ctx);
}

}  // namespace clockfaces
