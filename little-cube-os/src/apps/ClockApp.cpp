#include "ClockApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../services/TimeService.h"
#include "../ui/Theme.h"

// Functional-lite clock (spec §12): big digital time + date. Alarms,
// timers, stopwatch, and focus sessions are follow-up work; time comes
// from the RTC/NTP via TimeService, manual set via serial `time set`.

namespace {
const char* kWeekdays[7] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                            "Thursday", "Friday", "Saturday"};
const char* kMonths[12] = {"January", "February", "March",     "April",   "May",      "June",
                           "July",    "August",   "September", "October", "November", "December"};
}  // namespace

void ClockApp::render() {
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
  const bool haveTime = services_.time != nullptr && services_.time->now(t);
  const bool valid = services_.time != nullptr && services_.time->valid();

  char big[8];
  if (haveTime) {
    snprintf(big, sizeof(big), "%02d:%02d", t.tm_hour, t.tm_min);
  } else {
    snprintf(big, sizeof(big), "--:--");
  }

  gfx.setTextSize(theme::kTextSizeClock);
  gfx.setTextColor(valid ? theme::kText : theme::kTextDim);
  const int16_t tw = static_cast<int16_t>(strlen(big)) * 6 * theme::kTextSizeClock;
  gfx.setCursor((DISPLAY_WIDTH - tw) / 2, DISPLAY_HEIGHT / 2 - 60);
  gfx.print(big);

  if (haveTime && valid) {
    char date[48];
    snprintf(date, sizeof(date), "%s, %s %d", kWeekdays[t.tm_wday > 6 ? 0 : t.tm_wday],
             kMonths[t.tm_mon > 11 ? 0 : t.tm_mon], t.tm_mday);
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kTextDim);
    const int16_t dw = static_cast<int16_t>(strlen(date)) * 6 * theme::kTextSizeSmall;
    gfx.setCursor((DISPLAY_WIDTH - dw) / 2, DISPLAY_HEIGHT / 2 + 24);
    gfx.print(date);
  } else {
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kWarn);
    const char* hint = "time not set - connect Wi-Fi or `time set`";
    const int16_t hw = static_cast<int16_t>(strlen(hint)) * 6 * theme::kTextSizeSmall;
    gfx.setCursor((DISPLAY_WIDTH - hw) / 2, DISPLAY_HEIGHT / 2 + 24);
    gfx.print(hint);
  }

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kPanelAlt);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
  gfx.print("alarms & timers: coming soon");

  display->markDirty();
}

bool ClockApp::handleInput(const InputEvent& event) {
  (void)event;
  return false;  // Back/Home fall through to the router
}
