#include "HomeApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/AppRouter.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../services/SettingsService.h"
#include "../services/WifiService.h"
#include "../ui/Theme.h"

namespace {
constexpr AppId kToolsApps[3] = {AppId::Files, AppId::Contacts, AppId::Calculator};
}

void HomeApp::onOpen() {
  if (!began_) {
    carousel_.begin(&services_);
    began_ = true;
  }
  mode_ = Mode::Cards;
  forceRedraw_ = true;
}

void HomeApp::setMode(Mode mode) {
  mode_ = mode;
  forceRedraw_ = true;
}

void HomeApp::update(uint32_t deltaMs) {
  carousel_.update(deltaMs);
}

void HomeApp::render() {
  const SystemState& state = *services_.state;
  const bool stateChanged = state.version != lastStateVersion_;
  if (!forceRedraw_ && !stateChanged && !carousel_.animating()) {
    return;
  }
  lastStateVersion_ = state.version;
  forceRedraw_ = false;

  DisplayAdapter* display = services_.display;
  if (display == nullptr || !display->ready()) {
    return;
  }
  Arduino_GFX& gfx = *display->canvas();
  gfx.fillScreen(theme::kBg);
  statusBar_.render(gfx, state, 0, 0);

  switch (mode_) {
    case Mode::Cards:
      carousel_.render(gfx, theme::kStatusBarHeight);
      break;
    case Mode::Tools:
      renderTools(gfx);
      break;
    case Mode::Status:
      renderStatus(gfx);
      break;
    case Mode::QuickActions:
      renderQuickActions(gfx);
      break;
  }
  display->markDirty();
}

void HomeApp::renderTools(Arduino_GFX& gfx) {
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 16);
  gfx.print("Tools");

  const char* labels[3] = {"Files", "Contacts", "Calculator"};
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  int16_t y = theme::kStatusBarHeight + 56;
  for (uint8_t i = 0; i < 3; i++) {
    toolsRects_[i] = widgets::button(gfx, theme::kPadding, y, w, 64, labels[i], false);
    y += 76;
  }

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 32);
  gfx.print("swipe right / BOOT = back");
}

void HomeApp::renderStatus(Arduino_GFX& gfx) {
  const SystemState& state = *services_.state;

  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 16);
  gfx.print("Status");

  char line[64];
  int16_t y = theme::kStatusBarHeight + 60;
  const int16_t step = 34;
  gfx.setTextSize(theme::kTextSizeSmall);

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  snprintf(line, sizeof(line), "%s %s", FIRMWARE_NAME, FIRMWARE_VERSION);
  gfx.print(line);
  y += step;

  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, y);
  snprintf(line, sizeof(line), "Wi-Fi: %s", wifiStateName(state.wifi));
  gfx.print(line);
  y += step;

  gfx.setCursor(theme::kPadding, y);
  snprintf(line, sizeof(line), "SD card: %s", sdCardStateName(state.sd));
  gfx.print(line);
  y += step;

  if (state.batteryPresent) {
    gfx.setCursor(theme::kPadding, y);
    snprintf(line, sizeof(line), "Battery: %d%%%s", state.batteryPercent,
             state.charging ? " (charging)" : "");
    gfx.print(line);
    y += step;
  }

  gfx.setCursor(theme::kPadding, y);
  const uint32_t upSec = millis() / 1000;
  snprintf(line, sizeof(line), "Uptime: %lum %lus", (unsigned long)(upSec / 60),
           (unsigned long)(upSec % 60));
  gfx.print(line);

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 32);
  gfx.print("swipe up / BOOT = back");
}

void HomeApp::renderQuickActions(Arduino_GFX& gfx) {
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 16);
  gfx.print("Quick actions");

  char value[16];
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  int16_t y = theme::kStatusBarHeight + 64;

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("Brightness");
  snprintf(value, sizeof(value), "%d", services_.settings->brightness());
  gfx.setTextColor(theme::kText);
  gfx.setCursor(DISPLAY_WIDTH / 2 - 20, y + 44);
  gfx.print(value);
  brightnessDown_ = widgets::button(gfx, theme::kPadding, y + 28, 64, 56, "-", false);
  brightnessUp_ = widgets::button(gfx, theme::kPadding + w - 64, y + 28, 64, 56, "+", false);
  y += 116;

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("Volume");
  snprintf(value, sizeof(value), "%d%%", services_.settings->volumePercent());
  gfx.setTextColor(theme::kText);
  gfx.setCursor(DISPLAY_WIDTH / 2 - 24, y + 44);
  gfx.print(value);
  volumeDown_ = widgets::button(gfx, theme::kPadding, y + 28, 64, 56, "-", false);
  volumeUp_ = widgets::button(gfx, theme::kPadding + w - 64, y + 28, 64, 56, "+", false);

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 32);
  gfx.print("swipe down / BOOT = back");
}

bool HomeApp::handleCards(const InputEvent& event) {
  switch (event.action) {
    case InputAction::SwipeLeft:
      carousel_.next();
      forceRedraw_ = true;
      return true;
    case InputAction::SwipeRight:
      carousel_.previous();
      forceRedraw_ = true;
      return true;
    case InputAction::SwipeDown:
      setMode(Mode::Status);
      return true;
    case InputAction::SwipeUp:
      setMode(Mode::QuickActions);
      return true;
    case InputAction::Tap:
    case InputAction::Confirm: {
      const Carousel::Card& card = carousel_.focused();
      if (card.tools) {
        setMode(Mode::Tools);
      } else if (services_.router != nullptr) {
        services_.router->open(card.id);
      }
      return true;
    }
    default:
      return false;
  }
}

bool HomeApp::handleTools(const InputEvent& event) {
  if (event.action == InputAction::Tap) {
    for (uint8_t i = 0; i < 3; i++) {
      if (toolsRects_[i].contains(event.x, event.y)) {
        setMode(Mode::Cards);
        if (services_.router != nullptr) {
          services_.router->open(kToolsApps[i]);
        }
        return true;
      }
    }
    return true;  // tap outside the buttons stays on the Tools screen
  }
  if (event.action == InputAction::SwipeRight || event.action == InputAction::Back ||
      event.action == InputAction::Cancel) {
    setMode(Mode::Cards);
    return true;
  }
  return false;
}

bool HomeApp::handleQuickActions(const InputEvent& event) {
  SettingsService* settings = services_.settings;
  if (event.action == InputAction::Tap && settings != nullptr) {
    if (brightnessDown_.contains(event.x, event.y) || brightnessUp_.contains(event.x, event.y)) {
      const int step = brightnessUp_.contains(event.x, event.y) ? 16 : -16;
      int next = static_cast<int>(settings->brightness()) + step;
      if (next < 16) next = 16;  // never fully dark from the quick panel
      if (next > 255) next = 255;
      settings->setBrightness(static_cast<uint8_t>(next));
      if (services_.display != nullptr) {
        services_.display->setBrightness(static_cast<uint8_t>(next));
      }
      forceRedraw_ = true;
      return true;
    }
    if (volumeDown_.contains(event.x, event.y) || volumeUp_.contains(event.x, event.y)) {
      const int step = volumeUp_.contains(event.x, event.y) ? 10 : -10;
      int next = static_cast<int>(settings->volumePercent()) + step;
      if (next < 0) next = 0;
      if (next > 100) next = 100;
      settings->setVolumePercent(static_cast<uint8_t>(next));
      forceRedraw_ = true;
      return true;
    }
    return true;
  }
  if (event.action == InputAction::SwipeDown || event.action == InputAction::Back ||
      event.action == InputAction::Cancel) {
    setMode(Mode::Cards);
    return true;
  }
  return false;
}

bool HomeApp::handleInput(const InputEvent& event) {
  switch (mode_) {
    case Mode::Cards:
      return handleCards(event);
    case Mode::Tools:
      return handleTools(event);
    case Mode::Status:
      if (event.action == InputAction::SwipeUp || event.action == InputAction::Back ||
          event.action == InputAction::Tap || event.action == InputAction::Cancel) {
        setMode(Mode::Cards);
        return true;
      }
      return false;
    case Mode::QuickActions:
      return handleQuickActions(event);
  }
  return false;
}
