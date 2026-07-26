#include "WeatherApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../services/TimeService.h"
#include "../services/WeatherService.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"

namespace {
const char* kWeekdaysShort[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
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
  statusBar_.render(gfx, state, services_.amoled->shiftX(),
                    services_.amoled->shiftY());

  const WeatherSnapshot* wx =
      services_.weather != nullptr ? &services_.weather->snapshot() : nullptr;

  if (wx == nullptr || !wx->valid) {
    gfx.setTextSize(theme::kTextSizeBody);
    gfx.setTextColor(theme::kText);
    gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 16);
    gfx.print("Weather");
    const bool online = state.internet;
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kTextDim);
    gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 70);
    if (!online) {
      gfx.print("offline - no cached weather yet");
    } else {
      gfx.print("no location set - use phone setup");
    }
    gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 100);
    gfx.print("(tap to retry)");
    display->markDirty();
    return;
  }

  // Location + freshness.
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kAccent);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 10);
  gfx.print(wx->location);

  char freshness[40];
  if (wx->fetchedAtUptimeMs == 0) {
    snprintf(freshness, sizeof(freshness), "cached (before restart)");
  } else {
    const uint32_t ageMin = (millis() - wx->fetchedAtUptimeMs) / 60000UL;
    if (ageMin < 1) {
      snprintf(freshness, sizeof(freshness), "updated just now");
    } else if (ageMin < 60) {
      snprintf(freshness, sizeof(freshness), "updated %lum ago", (unsigned long)ageMin);
    } else {
      snprintf(freshness, sizeof(freshness), "updated %luh ago", (unsigned long)(ageMin / 60));
    }
  }
  gfx.setTextColor(wx->fetchedAtUptimeMs == 0 ? theme::kWarn : theme::kTextDim);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 34);
  gfx.print(freshness);

  // Current conditions.
  char big[12];
  snprintf(big, sizeof(big), "%d\xF8", (int)(wx->temperatureC + 0.5f));
  gfx.setTextSize(7);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 66);
  gfx.print(big);

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 130);
  gfx.print(wx->condition);

  char range[48];
  snprintf(range, sizeof(range), "H %d\xF8  L %d\xF8  rain %d%%", (int)(wx->highC + 0.5f),
           (int)(wx->lowC + 0.5f), wx->precipitationChancePct);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 158);
  gfx.print(range);

  // Three-day outlook.
  struct tm t;
  const bool haveDate = services_.time != nullptr && services_.time->valid() &&
                        services_.time->now(t);
  int16_t y = theme::kStatusBarHeight + 200;
  for (int i = 0; i < 3; i++) {
    const char* label = i == 0 ? "Today" : (i == 1 ? "Tmrw" : nullptr);
    char dayName[8];
    if (label == nullptr) {
      if (haveDate) {
        snprintf(dayName, sizeof(dayName), "%s", kWeekdaysShort[(t.tm_wday + i) % 7]);
      } else {
        snprintf(dayName, sizeof(dayName), "+%dd", i);
      }
      label = dayName;
    }
    char row[64];
    snprintf(row, sizeof(row), "%-6s %3d\xF8/%3d\xF8  %2d%%  %s", label,
             (int)(wx->days[i].highC + 0.5f), (int)(wx->days[i].lowC + 0.5f),
             wx->days[i].precipitationChancePct, wx->days[i].condition);
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kText);
    gfx.setCursor(theme::kPadding, y);
    gfx.print(row);
    y += 34;
  }

  gfx.setTextColor(theme::kPanelAlt);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
  gfx.print("tap to refresh");

  display->markDirty();
}

bool WeatherApp::handleInput(const InputEvent& event) {
  if (event.action == InputAction::Tap && services_.weather != nullptr) {
    if (services_.weather->refresh()) {
      Serial.println("[weather] manual refresh requested");
    }
    dirty_ = true;
    return true;
  }
  return false;
}
