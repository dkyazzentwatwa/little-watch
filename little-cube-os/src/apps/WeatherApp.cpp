#include "WeatherApp.h"

#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../services/TimeService.h"
#include "../services/WeatherService.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"
#include "../ui/WeatherArt.h"
#include "../ui/widgets/Widgets.h"

// Weather, wttr.in style (spec §13). Three tap-cycled views over one snapshot.
//
// THE BODY OF THIS SCREEN IS THE BUILT-IN 6x8 BITMAP FONT, on purpose, and it
// says so at every entry point rather than trusting a widget helper to have
// left the canvas that way. ASCII art is a fixed grid: the cloud outlines only
// line up if every glyph occupies an identical cell, which the proportional
// Free* faces the rest of the UI uses cannot do. widgets::footer() and
// widgets::button() still appear here — they set and restore their own font —
// but nothing between monoBegin() and monoEnd() may call them.
//
// THE GRID. With the built-in font setCursor() takes the TOP-LEFT (not the
// baseline, as it does under a GFXfont); a cell at setTextSize(n) is 6n across
// by 8n down; the glyph inks the leftmost 5n of that cell and all 8n rows. So
// n characters from cursor x occupy [x, x + 6*n*size) horizontally and
// [y, y + 8*size) vertically. That arithmetic is the entire layout system
// below and is what the measured envelopes in each renderer's comment were
// computed from.
//
// GEOMETRY IS MEASURED, NOT EYEBALLED. Every extent quoted below was computed
// from the formula above over all five burn-in shift quadrants ((0,0) and
// (±2,±2)), all 11 condition strings conditionFromWmo() can emit, and the
// longest location string the 48-byte field can hold. Arduino_GFX clips
// silently at the canvas edge, so an overflow never crashes — it just collides
// with the status bar or the footer. Recheck if you move any constant.

namespace {

// The horizontal origin for every body line. kSafeInset, not kPadding: the
// panel is a rounded square and this leaves the same clearance the footer's
// caption band uses.
constexpr int16_t kLeft = theme::kSafeInset;  // 20

// The whole screen speaks in two body sizes: 2 (12x16) for running text,
// 3 (18x24) for the art and the one-line headlines.
constexpr uint8_t kSizeSmall = 2;
constexpr uint8_t kSizeMed = 3;

// Longest line the small size can hold from kLeft without leaving the canvas:
// (368 - 20) / 12 = 29 by the cell box, held to 27 so the +2 burn-in shift and
// the right-hand inset both stay clear.
constexpr size_t kSmallMaxChars = 27;

// --- prompt-style header ---------------------------------------------------
// A monospace command line rather than widgets::header(): it keeps the
// terminal voice the ASCII clock faces established, and the geometry is
// exactly computable, which the proportional Title face is not.
// "cube:~$" is 7 characters; the argument starts one blank cell later.
constexpr const char* kPrompt = "cube:~$";
constexpr int16_t kHeaderTop = 38;                              // 38..53 at size 2
constexpr int16_t kArgX = kLeft + 6 * kSizeSmall * 8;           // 116

// --- built-in-font primitives ----------------------------------------------

void monoBegin(Arduino_GFX& gfx, uint8_t size, uint16_t color) {
  gfx.setFont(nullptr);
  gfx.setTextSize(size);
  gfx.setTextColor(color);
  // Wrapping is on by default and every widgets:: helper restores it that way.
  // A wrapped art row would silently fold onto the line below instead of
  // clipping, so it is off for the duration of a mono block.
  gfx.setTextWrap(false);
}

// Leaves the canvas the way widgets:: expects to find it.
void monoEnd(Arduino_GFX& gfx) {
  gfx.setTextSize(1);
  gfx.setTextWrap(true);
  gfx.setFont(nullptr);
}

void monoAt(Arduino_GFX& gfx, int16_t x, int16_t y, const char* s) {
  gfx.setCursor(x, y);
  gfx.print(s);
}

// Advance width of n characters, which is also the box the layout reserves.
int16_t cellSpan(size_t chars, uint8_t size) {
  return static_cast<int16_t>(6 * size * chars);
}

void monoCentered(Arduino_GFX& gfx, int16_t shiftX, int16_t y, const char* s, uint8_t size) {
  const int16_t w = cellSpan(strlen(s), size);
  monoAt(gfx, static_cast<int16_t>((DISPLAY_WIDTH - w) / 2 + shiftX), y, s);
}

// Copies src into out, clipped to maxChars cells with a "..." tail when it does
// not fit. Fixed-cell, so this is a character count and not a pixel measure —
// which is exactly why the mono grid is easier to keep inside the canvas than
// the proportional one.
void fitMono(const char* src, char* out, size_t outLen, size_t maxChars) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  out[0] = '\0';
  if (src == nullptr) {
    return;
  }
  if (maxChars > outLen - 1) {
    maxChars = outLen - 1;
  }
  const size_t len = strlen(src);
  if (len <= maxChars) {
    memcpy(out, src, len + 1);
    return;
  }
  if (maxChars <= 3) {
    memcpy(out, src, maxChars);
    out[maxChars] = '\0';
    return;
  }
  memcpy(out, src, maxChars - 3);
  out[maxChars - 3] = '\0';
  strcat(out, "...");
}

// lroundf, not the (int)(v + 0.5f) idiom this file used to carry: that rounds
// -3.4 to -2, which puts the wrong number on a freezing day.
int roundC(float c) {
  return static_cast<int>(lroundf(c));
}

// Draws one weatherart block, top-left at (x, y). Returns nothing: the block
// is exactly kCols x kRows cells by contract (static_assert in
// WeatherArt.cpp), so the caller already knows its extent.
void drawArt(Arduino_GFX& gfx, const char* const* art, int16_t x, int16_t y, uint8_t size) {
  const int16_t rowH = static_cast<int16_t>(8 * size);
  for (uint8_t r = 0; r < weatherart::kRows; ++r) {
    monoAt(gfx, x, static_cast<int16_t>(y + r * rowH), art[r]);
  }
}

// The two no-data messages, split across two lines each because neither fits
// one 27-cell line at size 2 ("offline - no cached weather yet" is 31
// characters, "no location set - use phone setup" is 33).
//
// THEY MUST STAY DISTINCT. One is fixed by walking into Wi-Fi range; the other
// needs the phone setup flow. A single generic "no weather" would send the
// user to the wrong remedy.
constexpr const char* kOfflineLine1 = "offline -";
constexpr const char* kOfflineLine2 = "no cached weather yet";
constexpr const char* kNoLocationLine1 = "no location set -";
constexpr const char* kNoLocationLine2 = "use phone setup";

const char* kWeekdaysShort[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

}  // namespace

void WeatherApp::formatFreshness(const WeatherSnapshot& wx, char* out, size_t outLen) const {
  // A snapshot restored from cache after a reboot has no uptime stamp to
  // measure against, so its age is genuinely unknown — say so rather than
  // computing a number from millis() that would read as "just now".
  if (wx.fetchedAtUptimeMs == 0) {
    snprintf(out, outLen, "cached (before restart)");  // 23 chars
    return;
  }
  const uint32_t ageMin = (millis() - wx.fetchedAtUptimeMs) / 60000UL;
  if (ageMin < 1) {
    snprintf(out, outLen, "updated just now");
  } else if (ageMin < 60) {
    snprintf(out, outLen, "updated %lum ago", static_cast<unsigned long>(ageMin));
  } else {
    snprintf(out, outLen, "updated %luh ago", static_cast<unsigned long>(ageMin / 60));
  }
}

void WeatherApp::requestRefresh() {
  if (services_.weather != nullptr && services_.weather->refresh()) {
    Serial.println("[weather] manual refresh requested");
  }
  dirty_ = true;
}

void WeatherApp::render() {
  const SystemState& state = *services_.state;
  if (!dirty_ && state.version == lastStateVersion_) {
    return;
  }
  lastStateVersion_ = state.version;
  dirty_ = false;

  DisplayAdapter* display = services_.display;
  if (display == nullptr || !display->ready()) {
    return;
  }
  Arduino_GFX& gfx = *display->canvas();
  gfx.fillScreen(theme::kBg);

  const int16_t shiftX = services_.amoled->shiftX();
  const int16_t shiftY = services_.amoled->shiftY();
  statusBar_.render(gfx, state, shiftX, shiftY);

  const WeatherSnapshot* wx =
      services_.weather != nullptr ? &services_.weather->snapshot() : nullptr;
  const bool haveData = wx != nullptr && wx->valid;

  // Header: "cube:~$ wx --now". Longest argument is "wx --forecast" at 13
  // characters = 156px from x = 116, so ink ends at 270 (+2 shifted).
  const char* arg = "wx";
  if (haveData) {
    switch (view_) {
      case View::Now: arg = "wx --now"; break;
      case View::Forecast: arg = "wx --forecast"; break;
      case View::Details: arg = "wx --details"; break;
    }
  }
  monoBegin(gfx, kSizeSmall, theme::kAccent);
  monoAt(gfx, static_cast<int16_t>(kLeft + shiftX), static_cast<int16_t>(kHeaderTop + shiftY),
         kPrompt);
  gfx.setTextColor(theme::kText);
  monoAt(gfx, static_cast<int16_t>(kArgX + shiftX), static_cast<int16_t>(kHeaderTop + shiftY),
         arg);
  monoEnd(gfx);

  if (!haveData) {
    // Envelope: art x[103..265] y[100..219]; the widest message line,
    // "no cached weather yet" at 21 chars, centres to x[58..310] y[250..289];
    // button x[12..356] y[310..366]. All +/-2 for the shift.
    constexpr int16_t kNdArtTop = 100;
    constexpr int16_t kNdMsgTop = 250;
    constexpr int16_t kNdMsg2Top = 274;
    constexpr int16_t kNdButtonTop = 310;
    const int16_t artX =
        static_cast<int16_t>((DISPLAY_WIDTH - cellSpan(weatherart::kCols, kSizeMed)) / 2);

    monoBegin(gfx, kSizeMed, theme::kTextDim);
    drawArt(gfx, weatherart::forCondition(nullptr), static_cast<int16_t>(artX + shiftX),
            static_cast<int16_t>(kNdArtTop + shiftY), kSizeMed);
    gfx.setTextSize(kSizeSmall);
    gfx.setTextColor(theme::kWarn);
    const bool online = state.internet;
    monoCentered(gfx, shiftX, static_cast<int16_t>(kNdMsgTop + shiftY),
                 online ? kNoLocationLine1 : kOfflineLine1, kSizeSmall);
    monoCentered(gfx, shiftX, static_cast<int16_t>(kNdMsg2Top + shiftY),
                 online ? kNoLocationLine2 : kOfflineLine2, kSizeSmall);
    monoEnd(gfx);

    // A visible control, so retry is never gesture-only.
    refreshRect_ = widgets::button(gfx, static_cast<int16_t>(theme::kPadding + shiftX),
                                   static_cast<int16_t>(kNdButtonTop + shiftY),
                                   DISPLAY_WIDTH - 2 * theme::kPadding, 56, "Retry", true);
    widgets::footer(gfx, "no weather data", "hold: refresh", shiftX, shiftY);
    display->markDirty();
    return;
  }

  refreshRect_ = widgets::Rect{};  // only the Details view offers a button
  switch (view_) {
    case View::Now: renderNow(gfx, *wx, shiftX, shiftY); break;
    case View::Forecast: renderForecast(gfx, *wx, shiftX, shiftY); break;
    case View::Details: renderDetails(gfx, *wx, shiftX, shiftY); break;
  }

  // `right` wins the footer band and holds the action hint; `left` yields and
  // ellipsizes. Both actions this screen owns are named, so neither tap-cycling
  // nor refresh is discoverable only by accident.
  widgets::footer(gfx, "tap: next view", "hold: refresh", shiftX, shiftY);
  display->markDirty();
}

// ---------------------------------------------------------------------------
// Now — the hero. Location and freshness on top, the condition's art beside a
// large temperature, then the condition text and today's range.
//
// Envelope, worst case over all 11 conditions and a 47-character location:
//   location  x[20..344] y[78..93]     (27 cells at size 2, ellipsized)
//   freshness x[20..296] y[104..119]   ("cached (before restart)", 23 cells)
//   art       x[20..182] y[140..259]   (9 x 5 cells at size 3)
//   temp      x[196..346] y[180..219]  (5 cells at size 5, "-100" + degree)
//   condition x[20..236] y[286..309]   ("Thunderstorm", 12 cells at size 3)
//   range     x[20..344] y[324..339]   (27 cells at size 2, see below)
// Widest point is 346 against a 368 canvas, so the +2 shift lands at 348.
void WeatherApp::renderNow(Arduino_GFX& gfx, const WeatherSnapshot& wx, int16_t shiftX,
                           int16_t shiftY) {
  constexpr int16_t kLocTop = 78;
  constexpr int16_t kFreshTop = 104;
  constexpr int16_t kArtTop = 140;
  constexpr int16_t kTempX = 196;
  constexpr int16_t kTempTop = 180;
  constexpr uint8_t kTempSize = 5;  // 30x40 cell
  constexpr int16_t kCondTop = 286;
  constexpr int16_t kRangeTop = 324;

  char buf[64];

  monoBegin(gfx, kSizeSmall, theme::kAccent);
  fitMono(wx.location, buf, sizeof(buf), kSmallMaxChars);
  monoAt(gfx, static_cast<int16_t>(kLeft + shiftX), static_cast<int16_t>(kLocTop + shiftY),
         buf);

  formatFreshness(wx, buf, sizeof(buf));
  // kWarn, never kTextDim, for a snapshot whose age cannot be computed —
  // presenting it as ordinary would be presenting stale data as current.
  gfx.setTextColor(wx.fetchedAtUptimeMs == 0 ? theme::kWarn : theme::kTextDim);
  monoAt(gfx, static_cast<int16_t>(kLeft + shiftX), static_cast<int16_t>(kFreshTop + shiftY),
         buf);

  gfx.setTextSize(kSizeMed);
  gfx.setTextColor(theme::kAccent);
  drawArt(gfx, weatherart::forCondition(wx.condition), static_cast<int16_t>(kLeft + shiftX),
          static_cast<int16_t>(kArtTop + shiftY), kSizeMed);

  // kTextDim rather than kText: this is the largest block of ink the screen
  // holds and the screen is a glance screen (spec §37).
  snprintf(buf, sizeof(buf), "%d\xF8", roundC(wx.temperatureC));
  gfx.setTextSize(kTempSize);
  gfx.setTextColor(theme::kTextDim);
  monoAt(gfx, static_cast<int16_t>(kTempX + shiftX), static_cast<int16_t>(kTempTop + shiftY),
         buf);

  gfx.setTextSize(kSizeMed);
  gfx.setTextColor(theme::kText);
  monoAt(gfx, static_cast<int16_t>(kLeft + shiftX), static_cast<int16_t>(kCondTop + shiftY),
         wx.condition);

  // "H -100\xF8  L -100\xF8  rain 100%" is the widest this can be: 27 cells.
  snprintf(buf, sizeof(buf), "H %d\xF8  L %d\xF8  rain %d%%", roundC(wx.highC),
           roundC(wx.lowC), wx.precipitationChancePct);
  gfx.setTextSize(kSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  monoAt(gfx, static_cast<int16_t>(kLeft + shiftX), static_cast<int16_t>(kRangeTop + shiftY),
         buf);
  monoEnd(gfx);
}

// ---------------------------------------------------------------------------
// Forecast — three day rows, two lines each.
//
// The day's condition appears as weatherart's fixed 3-character code, NOT as
// its text: the text form used to be printed here and clipped on the device
// ("Overcas" was legible in a photograph). Three cells cannot truncate.
//
// Envelope, worst case:
//   label x[20..128]  (6 cells at size 3)
//   code  x[146..200] (3 cells at size 3)
//   line2 x[20..320]  (25 cells at size 2: "-100 / -100   rain 100%" plus two
//                      degree glyphs)
//   rows at y = 104, 200, 296; each row inks [rowTop, rowTop+24) then
//   [rowTop+32, rowTop+48), so the last row ends at y = 344.
void WeatherApp::renderForecast(Arduino_GFX& gfx, const WeatherSnapshot& wx, int16_t shiftX,
                                int16_t shiftY) {
  constexpr int16_t kRowTop = 104;
  constexpr int16_t kRowStride = 96;
  constexpr int16_t kLine2Offset = 32;
  constexpr int16_t kCodeX = kLeft + 6 * kSizeMed * 7;  // 146: label field is 6 cells + gap

  struct tm t;
  const bool haveDate =
      services_.time != nullptr && services_.time->valid() && services_.time->now(t);

  monoBegin(gfx, kSizeMed, theme::kText);
  for (int i = 0; i < 3; i++) {
    const int16_t rowTop = static_cast<int16_t>(kRowTop + i * kRowStride + shiftY);

    // Six cells: "Today", "Tmrw", a weekday abbreviation, or "+2d" when the
    // clock has never been set. Never longer than six.
    char label[8];
    if (i == 0) {
      snprintf(label, sizeof(label), "Today");
    } else if (i == 1) {
      snprintf(label, sizeof(label), "Tmrw");
    } else if (haveDate) {
      const int wday = (t.tm_wday >= 0 && t.tm_wday <= 6) ? t.tm_wday : 0;
      snprintf(label, sizeof(label), "%s", kWeekdaysShort[(wday + i) % 7]);
    } else {
      snprintf(label, sizeof(label), "+%dd", i);
    }

    gfx.setTextSize(kSizeMed);
    gfx.setTextColor(theme::kText);
    monoAt(gfx, static_cast<int16_t>(kLeft + shiftX), rowTop, label);
    gfx.setTextColor(theme::kAccent);
    monoAt(gfx, static_cast<int16_t>(kCodeX + shiftX), rowTop,
           weatherart::codeForCondition(wx.days[i].condition));

    char line[48];
    snprintf(line, sizeof(line), "%d\xF8 / %d\xF8   rain %d%%", roundC(wx.days[i].highC),
             roundC(wx.days[i].lowC), wx.days[i].precipitationChancePct);
    gfx.setTextSize(kSizeSmall);
    gfx.setTextColor(theme::kTextDim);
    monoAt(gfx, static_cast<int16_t>(kLeft + shiftX),
           static_cast<int16_t>(rowTop + kLine2Offset), line);
  }
  monoEnd(gfx);
}

// ---------------------------------------------------------------------------
// Details — the same snapshot spelled out, plus the visible Refresh button.
//
// Envelope, worst case:
//   location  x[20..344] y[78..93]
//   freshness x[20..296] y[104..119]
//   labels    x[20..104] y[148..275]   (7 cells at size 2, five rows, 28 apart)
//   values    x[116..308]              (16 cells at size 2: "2026-07-31 14:32")
//   button    x[12..356] y[306..362]
void WeatherApp::renderDetails(Arduino_GFX& gfx, const WeatherSnapshot& wx, int16_t shiftX,
                               int16_t shiftY) {
  constexpr int16_t kLocTop = 78;
  constexpr int16_t kFreshTop = 104;
  constexpr int16_t kRowTop = 148;
  constexpr int16_t kRowStride = 28;
  constexpr int16_t kValueX = kLeft + 6 * kSizeSmall * 8;  // 116
  constexpr int16_t kButtonTop = 306;
  constexpr size_t kValueMaxChars = 20;  // (368 - 116) / 12 = 21, held to 20

  char buf[64];

  monoBegin(gfx, kSizeSmall, theme::kAccent);
  fitMono(wx.location, buf, sizeof(buf), kSmallMaxChars);
  monoAt(gfx, static_cast<int16_t>(kLeft + shiftX), static_cast<int16_t>(kLocTop + shiftY),
         buf);

  // The relative age lives on its own full-width line: "cached (before
  // restart)" is 23 cells and would not fit beside a label. The "when" row
  // below carries the absolute stamp; between them the age is always stated.
  formatFreshness(wx, buf, sizeof(buf));
  gfx.setTextColor(wx.fetchedAtUptimeMs == 0 ? theme::kWarn : theme::kTextDim);
  monoAt(gfx, static_cast<int16_t>(kLeft + shiftX), static_cast<int16_t>(kFreshTop + shiftY),
         buf);

  struct Row {
    const char* label;  // 6 cells, padded by the format below
    char value[24];
  };
  Row rows[5];
  rows[0].label = "cond";
  fitMono(wx.condition, rows[0].value, sizeof(rows[0].value), kValueMaxChars);
  rows[1].label = "rain";
  snprintf(rows[1].value, sizeof(rows[1].value), "%d%%", wx.precipitationChancePct);
  rows[2].label = "hi/lo";
  snprintf(rows[2].value, sizeof(rows[2].value), "%d\xF8 / %d\xF8", roundC(wx.highC),
           roundC(wx.lowC));
  rows[3].label = "when";
  if (wx.fetchedAtEpoch != 0) {
    // Absolute local wall time. TimeService has already applied the POSIX TZ,
    // so localtime_r resolves in the user's zone.
    const time_t stamp = static_cast<time_t>(wx.fetchedAtEpoch);
    struct tm lt;
    if (localtime_r(&stamp, &lt) != nullptr) {
      strftime(rows[3].value, sizeof(rows[3].value), "%Y-%m-%d %H:%M", &lt);
    } else {
      snprintf(rows[3].value, sizeof(rows[3].value), "not synced");
    }
  } else {
    // No absolute stamp — the relative line above is the whole story here, and
    // saying so is better than inventing a timestamp.
    snprintf(rows[3].value, sizeof(rows[3].value), "not synced");
  }
  rows[4].label = "link";
  snprintf(rows[4].value, sizeof(rows[4].value), "%s",
           services_.state->internet ? "online" : "offline");

  for (int i = 0; i < 5; i++) {
    const int16_t y = static_cast<int16_t>(kRowTop + i * kRowStride + shiftY);
    char label[10];
    snprintf(label, sizeof(label), "%-6s:", rows[i].label);  // exactly 7 cells
    gfx.setTextColor(theme::kTextDim);
    monoAt(gfx, static_cast<int16_t>(kLeft + shiftX), y, label);
    gfx.setTextColor(theme::kText);
    monoAt(gfx, static_cast<int16_t>(kValueX + shiftX), y, rows[i].value);
  }
  monoEnd(gfx);

  // A visible control, so refresh is never gesture-only. Its rect is stored
  // shifted, which is what makes the hit test follow the drawn button.
  refreshRect_ = widgets::button(gfx, static_cast<int16_t>(theme::kPadding + shiftX),
                                 static_cast<int16_t>(kButtonTop + shiftY),
                                 DISPLAY_WIDTH - 2 * theme::kPadding, 56, "Refresh", true);
}

bool WeatherApp::handleInput(const InputEvent& event) {
  if (event.action == InputAction::Tap) {
    // The button wins over the view cycle, so the visible control is never
    // shadowed by the background gesture.
    if (refreshRect_.w > 0 && refreshRect_.contains(event.x, event.y)) {
      requestRefresh();
      return true;
    }
    switch (view_) {
      case View::Now: view_ = View::Forecast; break;
      case View::Forecast: view_ = View::Details; break;
      case View::Details: view_ = View::Now; break;
    }
    dirty_ = true;
    return true;
  }
  if (event.action == InputAction::LongPress) {
    requestRefresh();
    return true;
  }
  return false;  // Back/Home fall through to the router
}
