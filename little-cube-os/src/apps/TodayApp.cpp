#include "TodayApp.h"

#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/SdCardState.h"
#include "../services/SettingsService.h"
#include "../services/TimeService.h"
#include "../services/WeatherService.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"
#include "../ui/widgets/Widgets.h"

// Today (spec §11), as a neofetch-style system readout.
//
// WHAT THIS SCREEN ACTUALLY IS: a status dashboard. It was card-shaped and set
// in the proportional Free* faces; it is now an ASCII logo over `key value`
// rows in the built-in 6x8 bitmap font, matching the voice WeatherApp and the
// ASCII clock faces established. Same font, same monospace grid, same
// restraint — one device, one dialect.
//
// THE GRID. With the built-in font setCursor() takes the TOP-LEFT (not the
// baseline, as it does under a GFXfont); a cell at setTextSize(n) is 6n across
// by 8n down, and the glyph inks the leftmost 5n of it. So n characters from
// cursor x occupy [x, x + 6*n*size) horizontally and [y, y + 8*size)
// vertically. That arithmetic is the whole layout system below.
//
// GEOMETRY IS MEASURED, NOT EYEBALLED. Every extent quoted was computed from
// the formula above over all five burn-in shift quadrants ((0,0) and (±2,±2))
// with worst-case content: a 32-character device name, "unsupported
// filesystem" (the longest sdCardStateName()), "Thunderstorm" at -100°, a
// three-digit battery percentage, and a multi-day uptime. Widest ink lands at
// x = 346 of 368; lowest at y = 386 against the footer rule at 404.
//
// NO SD, NO NETWORK, NO SERVICE CALLS THAT TOUCH EITHER. Everything here is a
// RAM read — SystemState, the cached weather snapshot, TimeService's cached
// tm, millis(), the settings device name — so render() draws and does nothing
// else, and the app needs no onOpen() load. Deliberately NOT consulted:
// CalendarService, NotesService, RecorderService. All three read the card.

namespace {

// --- grid ------------------------------------------------------------------

constexpr int16_t kLeft = theme::kSafeInset;  // 20; matches WeatherApp's body origin
constexpr uint8_t kSize = 2;                  // 12x16 cell, the readout's only text size

// Label field is four cells plus one blank: `os`, `up`, `time`, `wx`, `batt`,
// `sd`, `net`, `rec`. FOUR AND NOT SEVEN ("Battery", "Storage", ...) because
// the value column has to hold "unsupported filesystem" — 22 characters — and
// 20 + 12*(label+1) + 12*22 must stay inside the right inset. Seven-cell
// labels leave 19 cells and would ellipsize SD states, which is the exact
// failure the forecast rows already hit once ("Overcas" on the panel). The
// terse lowercase keys are the same voice as WeatherApp's details view.
constexpr int16_t kLabelCells = 5;
constexpr int16_t kValueX = kLeft + 6 * kSize * kLabelCells;  // 80
// (368 - 20) - 80 = 268 px = 22 cells; ink of the 22nd ends at 342, +2 shift = 344.
constexpr size_t kValueMaxChars = 22;
// (368 - 20) - 20 = 328 px = 27 cells from kLeft.
constexpr size_t kHeaderMaxChars = 27;

constexpr int16_t kHeaderTop = 40;  // 40..55
constexpr int16_t kRuleY = 66;
constexpr int16_t kLogoTop = 76;    // 76..171 at size 2
constexpr int16_t kRowsTop = 186;
constexpr int16_t kRowStride = 26;  // 16 ink + 10 gap
constexpr int kMaxRows = 8;         // os, up, time, wx, batt, sd, net, (+rec)

// The last row must clear widgets::footer()'s rule, which sits at
// DISPLAY_HEIGHT - 44. Both shift together with the burn-in offset, so the
// clearance this asserts is the one the panel gets in every quadrant.
static_assert(kRowsTop + (kMaxRows - 1) * kRowStride + 8 * kSize <= DISPLAY_HEIGHT - 44,
              "Today's readout rows must stay above the footer rule");
static_assert(kValueX + 6 * kSize * static_cast<int>(kValueMaxChars) <=
                  DISPLAY_WIDTH - theme::kSafeInset,
              "Today's value column must stay inside the right safe inset");

// --- the logo --------------------------------------------------------------
//
// CHARACTER SET, verified by parsing GFX_Library_for_Arduino's
// src/font/glcdfont.h and dumping each candidate as a 5x8 grid, not by
// trusting an editor:
//   '+' 0x2B  cross, stem in column 2, bar in row 3
//   '-' 0x2D  bar in row 3
//   '/' 0x2F  diagonal, row 1 col 4 down to row 5 col 0
//   0xB3      the ONLY full-height solid column in the table (column 3,
//             rows 0-7), so it is the only glyph that joins across stacked
//             rows. '|' (0x7C) is a BROKEN bar — rows 3 and 7 are blank — and
//             renders the verticals as dashes.
//
// WHY NOT THE CP437 BOX DRAWING (0xDA/0xBF/0xC0/0xD9 + 0xC4): those corners
// ink only half a cell each, so the rectangle breaks open at every corner and
// reads as display corruption. It was rendered from the font table and
// rejected on sight. '+' anchors each corner with a full cross instead.
//
// The horizontals are unavoidably dashed: drawChar inks 5 of the 6 cell
// columns, so any run of '-' or 0xC4 shows a one-unit seam per cell. That is
// the same seam the shipped weather art carries in its '_' runs, and in the
// classic ASCII cube idiom it reads as intended rather than as damage. The
// 0xB3 verticals sit one unit right of the '+' stem; at size 2 that is a 2 px
// jog inside a cross 10 px wide, and it is not visible at arm's length.
constexpr uint8_t kLogoCols = 7;
constexpr uint8_t kLogoRows = 6;
constexpr int16_t kLogoX = (DISPLAY_WIDTH - 6 * kSize * kLogoCols) / 2;  // 142
const char* const kLogo[kLogoRows] = {
    "  +---+",
    " /   /\xB3",
    "+---+ \xB3",
    "\xB3   \xB3 +",
    "\xB3   \xB3/ ",
    "+---+  ",
};

// --- built-in-font primitives (same contract as WeatherApp's) --------------

void monoBegin(Arduino_GFX& gfx, uint8_t size, uint16_t color) {
  gfx.setFont(nullptr);
  gfx.setTextSize(size);
  gfx.setTextColor(color);
  // A wrapped row would silently fold onto the line below instead of clipping.
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

// Copies src into out, clipped to maxChars cells with a "..." tail when it does
// not fit. A character count, not a pixel measure — which is exactly why the
// mono grid is easier to keep inside the canvas than the proportional one.
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

const char* kWeekdaysShort[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* kMonthsShort[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Uptime from millis(), which WRAPS AT ~49.7 DAYS. Nothing here pretends
// otherwise: after the wrap this reads as a fresh boot, the same thing every
// millis()-derived uptime on this chip does. Longest output is "49d 17h 02m",
// 11 cells.
void formatUptime(uint32_t ms, char* out, size_t outLen) {
  const uint32_t totalMin = ms / 60000UL;
  const unsigned long days = totalMin / 1440UL;
  const unsigned long hours = (totalMin % 1440UL) / 60UL;
  const unsigned long mins = totalMin % 60UL;
  if (days > 0) {
    snprintf(out, outLen, "%lud %luh %02lum", days, hours, mins);
  } else if (hours > 0) {
    snprintf(out, outLen, "%luh %02lum", hours, mins);
  } else {
    snprintf(out, outLen, "%lum", mins);
  }
}

// Wi-Fi state in at most 17 cells.
//
// NOT wifiStateName(): its longest strings run to 24 characters ("captive
// portal suspected"), which no label+value row on a 368 px panel can hold, and
// ellipsizing a diagnostic state is worse than shortening it deliberately.
// Every distinction wifiStateName() draws survives here — in particular
// "wrong password" and "network not found" stay separate, which spec §20 makes
// a product requirement. Connected-without-internet collapses into one
// phrasing because the two states mean the same thing to a reader.
struct NetLine {
  const char* text;
  bool warn;
};

NetLine networkLine(const SystemState& state) {
  switch (state.wifi) {
    case WifiState::Disabled: return {"wifi off", false};
    case WifiState::Idle: return {"idle", false};
    case WifiState::Scanning: return {"scanning", false};
    case WifiState::NetworksFound: return {"not joined", false};
    case WifiState::Connecting: return {"connecting", false};
    case WifiState::Connected:
    case WifiState::ConnectedNoInternet:
      return state.internet ? NetLine{"online", false} : NetLine{"wifi, no internet", true};
    case WifiState::AuthenticationFailed: return {"wrong password", true};
    case WifiState::NetworkNotFound: return {"network not found", true};
    case WifiState::CaptivePortalSuspected: return {"captive portal", true};
    case WifiState::Disconnected: return {"disconnected", false};
    case WifiState::Error: return {"wifi error", true};
  }
  return {"unknown", true};
}

}  // namespace

// One `label value` line. Returns nothing: rows are a fixed stride apart, so
// the caller already knows where the next one goes.
void TodayApp::drawRow(Arduino_GFX& gfx, int row, const char* label, const char* value,
                       uint16_t valueColor, int16_t shiftX, int16_t shiftY) const {
  const int16_t y = static_cast<int16_t>(kRowsTop + row * kRowStride + shiftY);
  char key[8];
  snprintf(key, sizeof(key), "%-4s", label);  // exactly 4 cells, blank cell after
  gfx.setTextColor(theme::kTextDim);
  monoAt(gfx, static_cast<int16_t>(kLeft + shiftX), y, key);
  char fitted[kValueMaxChars + 4];
  fitMono(value, fitted, sizeof(fitted), kValueMaxChars);
  gfx.setTextColor(valueColor);
  monoAt(gfx, static_cast<int16_t>(kValueX + shiftX), y, fitted);
}

// The battery row draws itself: a filled/shaded gauge cannot go through
// drawRow()'s single-colour value.
//
// 0xDB (full block) and 0xB0 (light shade) are the marks, the pair the Binary
// clock face already uses. They share one cell footprint, so the gauge never
// changes width as it drains; '#' and '.' would not, and the bar would jitter.
void TodayApp::drawBatteryRow(Arduino_GFX& gfx, int row, const SystemState& state,
                              int16_t shiftX, int16_t shiftY) const {
  const int16_t y = static_cast<int16_t>(kRowsTop + row * kRowStride + shiftY);
  gfx.setTextColor(theme::kTextDim);
  monoAt(gfx, static_cast<int16_t>(kLeft + shiftX), y, "batt");

  if (!state.batteryPresent) {
    gfx.setTextColor(theme::kTextDim);
    monoAt(gfx, static_cast<int16_t>(kValueX + shiftX), y, "not present");
    return;
  }
  // A negative percentage means the gauge has not produced a reading yet.
  // Say "unknown" — never a number, and never a low-battery warning, which is
  // what a 0 would look like.
  if (state.batteryPercent < 0) {
    gfx.setTextColor(theme::kTextDim);
    monoAt(gfx, static_cast<int16_t>(kValueX + shiftX), y, "unknown");
    return;
  }

  constexpr int kBarCells = 10;
  const int pct = state.batteryPercent > 100 ? 100 : state.batteryPercent;
  int filled = (pct * kBarCells + 50) / 100;
  if (filled > kBarCells) {
    filled = kBarCells;
  }
  const bool low = pct <= 15 && !state.charging;

  char bar[kBarCells + 1];
  for (int i = 0; i < kBarCells; i++) {
    bar[i] = static_cast<char>(i < filled ? 0xDB : 0xB0);
  }
  bar[kBarCells] = '\0';
  // Two passes so the drained cells stay dim: print the whole gauge as shade
  // in kTextDim, then overprint the filled run. The overprint is opaque
  // BECAUSE 0xDB's ink is a strict superset of 0xB0's — setTextColor's
  // one-argument form draws only the set pixels, so a full block completely
  // covers a shade cell underneath it. kTextDim, never kPanelAlt — kPanelAlt
  // is a fill colour and is illegible as ink.
  gfx.setTextColor(theme::kTextDim);
  monoAt(gfx, static_cast<int16_t>(kValueX + shiftX), y, bar);
  if (filled > 0) {
    char head[kBarCells + 1];
    memcpy(head, bar, static_cast<size_t>(filled));
    head[filled] = '\0';
    gfx.setTextColor(state.charging ? theme::kGood : (low ? theme::kWarn : theme::kAccent));
    monoAt(gfx, static_cast<int16_t>(kValueX + shiftX), y, head);
  }

  // " 100% chg" is 9 cells; 10 + 9 = 19 of the 22 available.
  char tail[16];
  snprintf(tail, sizeof(tail), " %d%%%s", pct, state.charging ? " chg" : "");
  gfx.setTextColor(low ? theme::kWarn : theme::kText);
  monoAt(gfx, static_cast<int16_t>(kValueX + 6 * kSize * kBarCells + shiftX), y, tail);
}

void TodayApp::render() {
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

  monoBegin(gfx, kSize, theme::kAccent);

  // Header: the machine's own name, neofetch's `user@host`. "cube" is the same
  // identity WeatherApp's `cube:~$` prompt speaks as. The device name is
  // capped at 32 characters by SettingsService, so the pair is fitted to the
  // 27 cells between the insets rather than assumed to fit.
  char header[48];
  snprintf(header, sizeof(header), "cube@%s",
           services_.settings != nullptr ? services_.settings->deviceName().c_str() : "cube");
  char headerFitted[kHeaderMaxChars + 4];
  fitMono(header, headerFitted, sizeof(headerFitted), kHeaderMaxChars);
  monoAt(gfx, static_cast<int16_t>(kLeft + shiftX), static_cast<int16_t>(kHeaderTop + shiftY),
         headerFitted);
  // A real hairline, not a run of 0xC4: drawChar leaves the sixth cell column
  // blank, so a character rule is visibly dashed. This matches the rule
  // widgets::footer() draws at the other end of the screen.
  gfx.drawFastHLine(static_cast<int16_t>(theme::kPadding + shiftX),
                    static_cast<int16_t>(kRuleY + shiftY), DISPLAY_WIDTH - 2 * theme::kPadding,
                    theme::kPanelAlt);

  for (uint8_t r = 0; r < kLogoRows; r++) {
    monoAt(gfx, static_cast<int16_t>(kLogoX + shiftX),
           static_cast<int16_t>(kLogoTop + r * 8 * kSize + shiftY), kLogo[r]);
  }

  int row = 0;

  char buf[64];
  snprintf(buf, sizeof(buf), "%s %s", FIRMWARE_NAME, FIRMWARE_VERSION);  // 20 cells
  drawRow(gfx, row++, "os", buf, theme::kText, shiftX, shiftY);

  formatUptime(millis(), buf, sizeof(buf));
  drawRow(gfx, row++, "up", buf, theme::kText, shiftX, shiftY);

  // Clock and calendar date on one line. An unset RTC gets the placeholder and
  // a named reason, never a garbage read dressed up as a date.
  struct tm t;
  const bool timeOk =
      services_.time != nullptr && services_.time->valid() && services_.time->now(t);
  if (timeOk) {
    snprintf(buf, sizeof(buf), "%02d:%02d %s %02d %s", t.tm_hour, t.tm_min,
             kWeekdaysShort[(t.tm_wday >= 0 && t.tm_wday <= 6) ? t.tm_wday : 0], t.tm_mday,
             kMonthsShort[(t.tm_mon >= 0 && t.tm_mon <= 11) ? t.tm_mon : 0]);
  } else {
    snprintf(buf, sizeof(buf), "--:-- not synced");
  }
  drawRow(gfx, row++, "time", buf, timeOk ? theme::kText : theme::kTextDim, shiftX, shiftY);

  // Weather: temperature and the CONDITION TEXT, not weatherart's 3-letter
  // code and not an icons:: glyph. The code exists for rows too tight to hold
  // the words (its header says so) and would print "CLR" beside "Clear" here,
  // where 22 cells is room enough for "-100° Thunderstorm" at 18. A vector
  // icon would be the only ink on the screen that is not on the cell grid the
  // rest of the readout aligns to.
  const WeatherSnapshot* wx =
      services_.weather != nullptr ? &services_.weather->snapshot() : nullptr;
  uint16_t wxColor = theme::kTextDim;
  if (wx != nullptr && wx->valid) {
    // Never present stale data as current (spec §11). A snapshot restored from
    // cache after a reboot has no uptime stamp to measure against, so its age
    // is genuinely unknown — that is a warning, not a quiet fact.
    const bool noStamp = wx->fetchedAtUptimeMs == 0;
    const uint32_t ageHours = noStamp ? 0 : (millis() - wx->fetchedAtUptimeMs) / 3600000UL;
    const bool stale = noStamp || ageHours >= 1;
    snprintf(buf, sizeof(buf), "%d\xF8 %s", roundC(wx->temperatureC), wx->condition);
    // The age suffix only goes on when the line has room for it; the colour
    // carries the warning either way, and truncating the condition to make
    // space would be the worse trade.
    if (stale && !noStamp && strlen(buf) + 6 <= kValueMaxChars) {
      char age[12];
      snprintf(age, sizeof(age), " (%luh)", static_cast<unsigned long>(ageHours));
      strcat(buf, age);
    }
    wxColor = stale ? theme::kWarn : theme::kText;
  } else {
    // Distinct remedies, so distinct words: no radio versus nothing fetched.
    snprintf(buf, sizeof(buf), "%s",
             state.internet ? "not fetched yet" : "offline, no cache");
  }
  drawRow(gfx, row++, "wx", buf, wxColor, shiftX, shiftY);

  drawBatteryRow(gfx, row++, state, shiftX, shiftY);

  // The specific SD state, never a generic error (spec §31). Longest is
  // "unsupported filesystem" at 22 cells, which is what set kValueMaxChars.
  drawRow(gfx, row++, "sd", sdCardStateName(state.sd),
          state.sd == SdCardState::Mounted ? theme::kText : theme::kWarn, shiftX, shiftY);

  const NetLine net = networkLine(state);
  drawRow(gfx, row++, "net", net.text,
          net.warn ? theme::kWarn : (state.internet ? theme::kText : theme::kTextDim), shiftX,
          shiftY);

  // The recording row EXISTS ONLY WHILE RECORDING. An absent row beats a
  // permanent dash.
  //
  // TWO ROWS WERE DELETED HERE AND SHOULD NOT COME BACK AS DASHES. `Next  -
  // nothing scheduled` was a hardcoded string: CalendarService holds real
  // events, but reading them needs an onOpen() SD load this screen
  // deliberately does not have, so the row asserted "nothing scheduled"
  // without ever having looked. `Alarm  -` was worse — nothing in the firmware
  // ever writes SystemState::alarmArmed, so it could not have said anything
  // else. Restore either one only together with the service call that makes it
  // true.
  if (state.recording) {
    drawRow(gfx, row++, "rec", "recording", theme::kBad, shiftX, shiftY);
  }

  monoEnd(gfx);

  // No action hint on the right: this screen has no gestures to advertise, and
  // inventing one would be a lie. The rule frames the readout.
  widgets::footer(gfx, "system readout", nullptr, shiftX, shiftY);

  display->markDirty();
}

bool TodayApp::handleInput(const InputEvent& event) {
  (void)event;
  return false;
}
