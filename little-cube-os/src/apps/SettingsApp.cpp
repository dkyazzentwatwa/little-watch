#include "SettingsApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../services/ProvisioningService.h"
#include "../services/SettingsService.h"
#include "../ui/Theme.h"

namespace {
constexpr int16_t kTop = theme::kStatusBarHeight + 12;
constexpr uint32_t kLivePollMs = 1000;  // refresh cadence for live screens
}  // namespace

void SettingsApp::onOpen() {
  screen_ = Screen::Root;
  dirty_ = true;
}

void SettingsApp::onClose() {
  // Leaving Settings never leaves the setup AP running silently.
  if (screen_ == Screen::SetupMode && services_.provisioning != nullptr &&
      services_.provisioning->phase() != ProvisioningService::Phase::Success) {
    services_.provisioning->stop();
  }
}

void SettingsApp::go(Screen screen) {
  screen_ = screen;
  dirty_ = true;
}

void SettingsApp::update(uint32_t deltaMs) {
  // Wi-Fi and SetupMode screens show live state — refresh them at 1 Hz.
  if (screen_ == Screen::Wifi || screen_ == Screen::SetupMode) {
    pollAccumMs_ += deltaMs;
    if (pollAccumMs_ >= kLivePollMs) {
      pollAccumMs_ = 0;
      dirty_ = true;
    }
  }
}

void SettingsApp::render() {
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

  switch (screen_) {
    case Screen::Root: renderRoot(gfx); break;
    case Screen::Wifi: renderWifi(gfx); break;
    case Screen::WifiMethod: renderWifiMethod(gfx); break;
    case Screen::SetupMode: renderSetupMode(gfx); break;
    case Screen::Display: renderDisplay(gfx); break;
    case Screen::About: renderAbout(gfx); break;
  }
  display->markDirty();
}

void SettingsApp::renderRoot(Arduino_GFX& gfx) {
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Settings");

  const char* labels[4] = {"Wi-Fi", "Display", "About", "Restart"};
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  int16_t y = kTop + 44;
  for (uint8_t i = 0; i < 4; i++) {
    rootRects_[i] = widgets::button(gfx, theme::kPadding, y, w, 58, labels[i], false);
    y += 70;
  }
}

void SettingsApp::renderWifi(Arduino_GFX& gfx) {
  const SystemState& state = *services_.state;
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Wi-Fi");

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, kTop + 36);
  gfx.print(wifiStateName(state.wifi));

  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  wifiScanRect_ = widgets::button(gfx, theme::kPadding, kTop + 62, (w - 8) / 2, 48, "Scan", false);
  wifiPhoneRect_ = widgets::button(gfx, theme::kPadding + (w + 8) / 2, kTop + 62, (w - 8) / 2, 48,
                                   "Phone setup", true);

  uint8_t count = 0;
  const WifiService::ScanResult* results =
      services_.wifi != nullptr ? services_.wifi->scanResults(count) : nullptr;
  int16_t y = kTop + 126;
  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t idx = wifiPage_ * 4 + i;
    if (results == nullptr || idx >= count) {
      wifiRowRects_[i] = widgets::Rect{};
      continue;
    }
    char secondary[48];
    snprintf(secondary, sizeof(secondary), "%d dBm · %s%s", (int)results[idx].rssi,
             results[idx].secure ? "locked" : "open", results[idx].saved ? " · saved" : "");
    wifiRowRects_[i] = widgets::listItem(gfx, theme::kPadding, y, w, results[idx].ssid,
                                         secondary, false);
    y += 62;
  }
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
  if (count == 0) {
    gfx.print(state.wifi == WifiState::Scanning ? "scanning..." : "tap Scan to find networks");
  } else {
    char pager[40];
    snprintf(pager, sizeof(pager), "%u networks · swipe up/down", (unsigned)count);
    gfx.print(pager);
  }
}

void SettingsApp::renderWifiMethod(Arduino_GFX& gfx) {
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Connect to");
  gfx.setTextColor(theme::kAccent);
  gfx.setCursor(theme::kPadding, kTop + 34);
  gfx.print(chosenSsid_);

  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  methodPhoneRect_ =
      widgets::button(gfx, theme::kPadding, kTop + 90, w, 64, "Phone setup", true);
  methodSerialRect_ =
      widgets::button(gfx, theme::kPadding, kTop + 170, w, 64, "USB serial", false);

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  widgets::textBlock(gfx, theme::kPadding, kTop + 250, w,
                     "Phone setup opens a temporary hotspot with a form. USB serial: plug in "
                     "and run  wifi connect \"<name>\"",
                     theme::kTextSizeSmall, theme::kTextDim);
}

void SettingsApp::renderSetupMode(Arduino_GFX& gfx) {
  ProvisioningService* prov = services_.provisioning;
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kAccent);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("SETUP MODE");

  if (prov == nullptr || !prov->active()) {
    const bool done = prov != nullptr &&
                      prov->phase() == ProvisioningService::Phase::Off &&
                      services_.state->wifi == WifiState::Connected;
    widgets::textBlock(gfx, theme::kPadding, kTop + 50, DISPLAY_WIDTH - 2 * theme::kPadding,
                       done ? "Setup complete - connected." : "Setup mode is off.",
                       theme::kTextSizeSmall, theme::kText);
    return;
  }

  int16_t y = kTop + 50;
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("Connect your phone to:");
  y += 30;
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, y);
  gfx.print(prov->apName());
  y += 40;
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("password:");
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding + 120, y);
  gfx.print(prov->apPassword());
  y += 40;
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("then open:");
  y += 30;
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kAccent);
  gfx.setCursor(theme::kPadding, y);
  gfx.print(prov->portalIp());
  y += 48;

  gfx.setTextSize(theme::kTextSizeSmall);
  switch (prov->phase()) {
    case ProvisioningService::Phase::Testing:
      gfx.setTextColor(theme::kWarn);
      gfx.setCursor(theme::kPadding, y);
      gfx.print("testing connection...");
      break;
    case ProvisioningService::Phase::Success:
      gfx.setTextColor(theme::kGood);
      gfx.setCursor(theme::kPadding, y);
      gfx.print("connected! finishing up");
      break;
    case ProvisioningService::Phase::Failed:
      gfx.setTextColor(theme::kBad);
      gfx.setCursor(theme::kPadding, y);
      gfx.print(prov->lastError());
      break;
    default:
      break;
  }

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
  gfx.print("BOOT / swipe right = cancel setup");
}

void SettingsApp::renderDisplay(Arduino_GFX& gfx) {
  SettingsService* settings = services_.settings;
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Display");

  char value[24];
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  int16_t y = kTop + 48;

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("Brightness");
  snprintf(value, sizeof(value), "%u", (unsigned)settings->brightness());
  gfx.setTextColor(theme::kText);
  gfx.setCursor(DISPLAY_WIDTH / 2 - 18, y + 40);
  gfx.print(value);
  displayRects_[0] = widgets::button(gfx, theme::kPadding, y + 26, 60, 50, "-", false);
  displayRects_[1] = widgets::button(gfx, theme::kPadding + w - 60, y + 26, 60, 50, "+", false);
  y += 100;

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("Screen timeout");
  if (settings->screenTimeoutSec() == 0) {
    snprintf(value, sizeof(value), "never");
  } else {
    snprintf(value, sizeof(value), "%us", (unsigned)settings->screenTimeoutSec());
  }
  gfx.setTextColor(theme::kText);
  gfx.setCursor(DISPLAY_WIDTH / 2 - 30, y + 40);
  gfx.print(value);
  displayRects_[2] = widgets::button(gfx, theme::kPadding, y + 26, 60, 50, "<", false);
  displayRects_[3] = widgets::button(gfx, theme::kPadding + w - 60, y + 26, 60, 50, ">", false);
  y += 100;

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("Always-on");
  displayRects_[4] = widgets::button(gfx, theme::kPadding + w - 110, y - 8, 110, 46,
                                     settings->alwaysOn() ? "on" : "off",
                                     settings->alwaysOn());
}

void SettingsApp::renderAbout(Arduino_GFX& gfx) {
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("About");

  char line[64];
  int16_t y = kTop + 48;
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kText);

  gfx.setCursor(theme::kPadding, y);
  snprintf(line, sizeof(line), "%s %s", FIRMWARE_NAME, FIRMWARE_VERSION);
  gfx.print(line);
  y += 34;

  gfx.setCursor(theme::kPadding, y);
  snprintf(line, sizeof(line), "Device: %s", services_.settings->deviceName().c_str());
  gfx.print(line);
  y += 34;

  gfx.setCursor(theme::kPadding, y);
  snprintf(line, sizeof(line), "Board: Waveshare AMOLED 1.8");
  gfx.print(line);
  y += 34;

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("USB serial: 115200, type 'help'");
}

bool SettingsApp::handleInput(const InputEvent& event) {
  const bool back = event.action == InputAction::Back || event.action == InputAction::Cancel;

  switch (screen_) {
    case Screen::Root:
      if (event.action == InputAction::Tap) {
        if (rootRects_[0].contains(event.x, event.y)) {
          go(Screen::Wifi);
        } else if (rootRects_[1].contains(event.x, event.y)) {
          go(Screen::Display);
        } else if (rootRects_[2].contains(event.x, event.y)) {
          go(Screen::About);
        } else if (rootRects_[3].contains(event.x, event.y)) {
          Serial.println("restarting from Settings...");
          Serial.flush();
          delay(100);
          ESP.restart();
        }
        return true;
      }
      return false;  // Back falls through to the router (home)

    case Screen::Wifi:
      if (back || event.action == InputAction::SwipeRight) {
        go(Screen::Root);
        return true;
      }
      if (event.action == InputAction::SwipeUp || event.action == InputAction::SwipeDown) {
        uint8_t count = 0;
        services_.wifi->scanResults(count);
        const uint8_t pages = count == 0 ? 1 : (count + 3) / 4;
        if (event.action == InputAction::SwipeUp && wifiPage_ + 1 < pages) {
          wifiPage_++;
        } else if (event.action == InputAction::SwipeDown && wifiPage_ > 0) {
          wifiPage_--;
        }
        dirty_ = true;
        return true;
      }
      if (event.action == InputAction::Tap) {
        if (wifiScanRect_.contains(event.x, event.y) && services_.wifi != nullptr) {
          services_.wifi->startScan(false);
          wifiPage_ = 0;
          dirty_ = true;
          return true;
        }
        if (wifiPhoneRect_.contains(event.x, event.y) && services_.provisioning != nullptr) {
          services_.provisioning->start();
          go(Screen::SetupMode);
          return true;
        }
        uint8_t count = 0;
        const WifiService::ScanResult* results = services_.wifi->scanResults(count);
        for (uint8_t i = 0; i < 4; i++) {
          const uint8_t idx = wifiPage_ * 4 + i;
          if (idx < count && wifiRowRects_[i].contains(event.x, event.y)) {
            strncpy(chosenSsid_, results[idx].ssid, sizeof(chosenSsid_) - 1);
            chosenSsid_[sizeof(chosenSsid_) - 1] = '\0';
            go(Screen::WifiMethod);
            return true;
          }
        }
        return true;
      }
      return false;

    case Screen::WifiMethod:
      if (back || event.action == InputAction::SwipeRight) {
        go(Screen::Wifi);
        return true;
      }
      if (event.action == InputAction::Tap) {
        if (methodPhoneRect_.contains(event.x, event.y) && services_.provisioning != nullptr) {
          services_.provisioning->start();
          go(Screen::SetupMode);
        } else if (methodSerialRect_.contains(event.x, event.y)) {
          Serial.printf("hint: wifi connect \"%s\"\n", chosenSsid_);
          go(Screen::Wifi);
        }
        return true;
      }
      return false;

    case Screen::SetupMode:
      if (back || event.action == InputAction::SwipeRight) {
        if (services_.provisioning != nullptr &&
            services_.provisioning->phase() != ProvisioningService::Phase::Success) {
          services_.provisioning->stop();
        }
        go(Screen::Wifi);
        return true;
      }
      return true;  // absorb taps; this is a passive info screen

    case Screen::Display:
      if (back || event.action == InputAction::SwipeRight) {
        go(Screen::Root);
        return true;
      }
      if (event.action == InputAction::Tap) {
        SettingsService* settings = services_.settings;
        if (displayRects_[0].contains(event.x, event.y) ||
            displayRects_[1].contains(event.x, event.y)) {
          const int step = displayRects_[1].contains(event.x, event.y) ? 16 : -16;
          int next = static_cast<int>(settings->brightness()) + step;
          if (next < 16) next = 16;
          if (next > 255) next = 255;
          settings->setBrightness(static_cast<uint8_t>(next));
          services_.display->setBrightness(static_cast<uint8_t>(next));
          dirty_ = true;
        } else if (displayRects_[2].contains(event.x, event.y) ||
                   displayRects_[3].contains(event.x, event.y)) {
          static const uint32_t kSteps[] = {15, 30, 60, 120, 300, 0};  // 0 = never
          const uint32_t current = settings->screenTimeoutSec();
          int idx = 0;
          for (int i = 0; i < 6; i++) {
            if (kSteps[i] == current) {
              idx = i;
              break;
            }
          }
          idx += displayRects_[3].contains(event.x, event.y) ? 1 : -1;
          if (idx < 0) idx = 5;
          if (idx > 5) idx = 0;
          settings->setScreenTimeoutSec(kSteps[idx]);
          dirty_ = true;
        } else if (displayRects_[4].contains(event.x, event.y)) {
          settings->setAlwaysOn(!settings->alwaysOn());
          dirty_ = true;
        }
        return true;
      }
      return false;

    case Screen::About:
      if (back || event.action == InputAction::SwipeRight || event.action == InputAction::Tap) {
        go(Screen::Root);
        return true;
      }
      return false;
  }
  return false;
}
