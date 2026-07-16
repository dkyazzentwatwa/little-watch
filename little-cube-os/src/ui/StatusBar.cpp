#include "StatusBar.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "Theme.h"

namespace {

void drawWifi(Arduino_GFX& gfx, int16_t x, int16_t y, const SystemState& state) {
  const bool connected =
      state.wifi == WifiState::Connected || state.wifi == WifiState::ConnectedNoInternet;
  const uint16_t color = connected ? (state.internet ? theme::kAccent : theme::kWarn)
                                   : theme::kPanelAlt;
  // Three ascending bars.
  gfx.fillRect(x, y + 10, 4, 6, color);
  gfx.fillRect(x + 6, y + 6, 4, 10, color);
  gfx.fillRect(x + 12, y + 2, 4, 14, color);
  if (state.wifi == WifiState::Disabled) {
    gfx.drawLine(x - 1, y + 16, x + 16, y, theme::kBad);  // slash = radio off
  }
}

void drawSd(Arduino_GFX& gfx, int16_t x, int16_t y, const SystemState& state) {
  uint16_t color = theme::kPanelAlt;  // absent = barely visible
  if (state.sd == SdCardState::Mounted) {
    color = theme::kTextDim;
  } else if (state.sd == SdCardState::ReadOnly || state.sd == SdCardState::Full) {
    color = theme::kWarn;
  } else if (state.sd == SdCardState::Corrupted || state.sd == SdCardState::Error ||
             state.sd == SdCardState::RemovedUnexpectedly) {
    color = theme::kBad;
  }
  // microSD silhouette with a notched corner.
  gfx.fillRect(x, y + 4, 12, 14, color);
  gfx.fillTriangle(x + 8, y, x + 12, y, x + 12, y + 4, color);
  gfx.fillTriangle(x + 8, y, x + 8, y + 4, x + 12, y + 4, color);
}

void drawRecording(Arduino_GFX& gfx, int16_t x, int16_t y, const SystemState& state) {
  if (!state.recording) {
    return;
  }
  gfx.fillCircle(x + 6, y + 9, 6, theme::kBad);
}

void drawAlarm(Arduino_GFX& gfx, int16_t x, int16_t y, const SystemState& state) {
  if (!state.alarmArmed) {
    return;
  }
  // Minimal bell: dome + clapper.
  gfx.fillCircle(x + 7, y + 8, 6, theme::kTextDim);
  gfx.fillRect(x + 1, y + 8, 13, 5, theme::kTextDim);
  gfx.fillCircle(x + 7, y + 16, 2, theme::kTextDim);
}

void drawBattery(Arduino_GFX& gfx, int16_t x, int16_t y, const SystemState& state) {
  if (!state.batteryPresent || state.batteryPercent < 0) {
    return;
  }
  const int16_t w = 24;
  const int16_t h = 12;
  uint16_t color = theme::kGood;
  if (state.charging) {
    color = theme::kAccent;
  } else if (state.batteryPercent < 15) {
    color = theme::kBad;
  } else if (state.batteryPercent < 40) {
    color = theme::kWarn;
  }
  gfx.drawRoundRect(x, y + 3, w, h, 2, theme::kTextDim);
  gfx.fillRect(x + w, y + 6, 2, 6, theme::kTextDim);  // tip
  const int16_t fill = static_cast<int16_t>((w - 4) * state.batteryPercent / 100);
  if (fill > 0) {
    gfx.fillRect(x + 2, y + 5, fill, h - 4, color);
  }
}

}  // namespace

void StatusBar::render(Arduino_GFX& gfx, const SystemState& state, int16_t shiftX,
                       int16_t shiftY) {
  const int16_t y = shiftY;

  // Clock, left-aligned. Dim rather than bright: this element is persistent
  // (AMOLED protection also drifts it via shiftX/shiftY).
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(state.timeValid ? theme::kText : theme::kTextDim);
  gfx.setCursor(theme::kPadding + shiftX, y + 6);
  gfx.print(state.clockHhMm);

  // Right-aligned glyph row, packed right to left.
  int16_t x = DISPLAY_WIDTH - theme::kPadding + shiftX;
  x -= 26;
  drawBattery(gfx, x, y + 3, state);
  x -= 22;
  drawWifi(gfx, x, y + 4, state);
  x -= 20;
  drawSd(gfx, x, y + 3, state);
  if (state.alarmArmed) {
    x -= 22;
    drawAlarm(gfx, x, y + 2, state);
  }
  if (state.recording) {
    x -= 20;
    drawRecording(gfx, x, y + 4, state);
  }
}
