#include "TodayApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../services/TimeService.h"
#include "../services/WeatherService.h"
#include "../ui/Theme.h"

namespace {
const char* kWeekdaysShort[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* kMonthsShort[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
}  // namespace

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
  statusBar_.render(gfx, state, 0, 0);

  struct tm t;
  const bool valid = services_.time != nullptr && services_.time->valid() &&
                     services_.time->now(t);

  // Big time.
  char big[8];
  if (valid) {
    snprintf(big, sizeof(big), "%02d:%02d", t.tm_hour, t.tm_min);
  } else {
    snprintf(big, sizeof(big), "--:--");
  }
  gfx.setTextSize(6);
  gfx.setTextColor(valid ? theme::kText : theme::kTextDim);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 24);
  gfx.print(big);

  int16_t y = theme::kStatusBarHeight + 88;
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  if (valid) {
    char date[32];
    snprintf(date, sizeof(date), "%s, %s %d", kWeekdaysShort[t.tm_wday > 6 ? 0 : t.tm_wday],
             kMonthsShort[t.tm_mon > 11 ? 0 : t.tm_mon], t.tm_mday);
    gfx.print(date);
  } else {
    gfx.print("date pending time sync");
  }
  y += 44;

  // Weather line.
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, y);
  const WeatherSnapshot* wx =
      services_.weather != nullptr ? &services_.weather->snapshot() : nullptr;
  if (wx != nullptr && wx->valid) {
    char line[64];
    snprintf(line, sizeof(line), "%d\xF8 · %s", (int)(wx->temperatureC + 0.5f), wx->condition);
    gfx.print(line);
    // Never present stale data as current (spec §11).
    if (wx->fetchedAtUptimeMs == 0) {
      gfx.setTextColor(theme::kWarn);
      gfx.setCursor(theme::kPadding + 210, y);
      gfx.print("(cached)");
    } else {
      const uint32_t ageMin = (millis() - wx->fetchedAtUptimeMs) / 60000UL;
      if (ageMin > 60) {
        gfx.setTextColor(theme::kWarn);
        gfx.setCursor(theme::kPadding + 210, y);
        char age[24];
        snprintf(age, sizeof(age), "(%luh old)", (unsigned long)(ageMin / 60));
        gfx.print(age);
      }
    }
  } else {
    gfx.setTextColor(theme::kTextDim);
    gfx.print("weather: not fetched yet");
  }
  y += 44;

  // Next event / alarm / latest voice note (placeholders until those land).
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("Next");
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding + 90, y);
  gfx.print("- nothing scheduled");
  y += 36;

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("Alarm");
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding + 90, y);
  gfx.print(state.alarmArmed ? "armed" : "-");
  y += 36;

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("Rec");
  gfx.setTextColor(state.recording ? theme::kBad : theme::kText);
  gfx.setCursor(theme::kPadding + 90, y);
  gfx.print(state.recording ? "recording now" : "-");

  display->markDirty();
}

bool TodayApp::handleInput(const InputEvent& event) {
  (void)event;
  return false;
}
