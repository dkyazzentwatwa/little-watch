#include "SettingsApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../services/ProvisioningService.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../hardware/audio/Es8311.h"
#include "../services/RecorderService.h"
#include "../services/SettingsService.h"
#include "../ui/widgets/Widgets.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"

namespace {
constexpr int16_t kTop = theme::kStatusBarHeight + 12;
constexpr uint32_t kLivePollMs = 1000;  // refresh cadence for live screens
}  // namespace

void SettingsApp::onOpen() {
  screen_ = Screen::Root;
  confirmRestart_ = false;
  dirty_ = true;
}

void SettingsApp::onClose() {
  leaveSetupMode();
  confirmRestart_ = false;
}

void SettingsApp::onPause() {
  // Backgrounded still means unattended: a paused SettingsApp used to sit on
  // the stack broadcasting a provisioning AP until reboot.
  leaveSetupMode();
  // An armed reboot confirm must never be waiting under the user's first tap
  // when they come back to this screen.
  confirmRestart_ = false;
}

// Leaving the setup screen — for good or just into the background — never
// leaves the AP running silently. Moving screen_ matters as much as stopping
// the radio: a paused app resumes on the screen it left, and SetupMode would
// then render a live-looking SSID and password for a hotspot that is gone.
void SettingsApp::leaveSetupMode() {
  if (screen_ != Screen::SetupMode) {
    return;
  }
  // Phase::Success means provisioning is already finishing its own teardown;
  // stopping the AP here would cut the portal off mid-handshake.
  if (services_.provisioning != nullptr &&
      services_.provisioning->phase() != ProvisioningService::Phase::Success) {
    services_.provisioning->stop();
  }
  screen_ = Screen::Wifi;
  dirty_ = true;
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
  statusBar_.render(gfx, state, services_.amoled->shiftX(),
                    services_.amoled->shiftY());

  switch (screen_) {
    case Screen::Root:
      renderRoot(gfx);
      if (confirmRestart_) {
        renderRestartConfirm(gfx);
      }
      break;
    case Screen::Wifi: renderWifi(gfx); break;
    case Screen::WifiMethod: renderWifiMethod(gfx); break;
    case Screen::SetupMode: renderSetupMode(gfx); break;
    case Screen::Display: renderDisplay(gfx); break;
    case Screen::Themes: renderThemes(gfx); break;
    case Screen::Sound: renderSound(gfx); break;
    case Screen::Assistant: renderAssistant(gfx); break;
    case Screen::Video: renderVideo(gfx); break;
    case Screen::About: renderAbout(gfx); break;
  }
  display->markDirty();
}

// A live preview grid: each row is painted in that theme's own bg/text/accent
// so you see the palette before committing. Tapping applies + persists it.
void SettingsApp::renderThemes(Arduino_GFX& gfx) {
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Themes");

  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  const int16_t rowH = 34;
  int16_t y = kTop + 34;
  const uint8_t active = services_.settings != nullptr ? services_.settings->themeIndex() : 0;
  for (uint8_t i = 0; i < theme::kThemeCount; i++) {
    const theme::ThemeDef& t = theme::kThemes[i];
    themeRects_[i] = {theme::kPadding, y, w, static_cast<int16_t>(rowH - 4)};
    // Paint the swatch in the theme's own colors — an instant preview.
    gfx.fillRoundRect(themeRects_[i].x, themeRects_[i].y, themeRects_[i].w, themeRects_[i].h, 6,
                      t.bg);
    if (i == active) {
      gfx.drawRoundRect(themeRects_[i].x, themeRects_[i].y, themeRects_[i].w, themeRects_[i].h, 6,
                        t.accent);
    }
    // A trio of accent/good/warn dots so the swatch reads as a palette.
    const int16_t cy = themeRects_[i].y + themeRects_[i].h / 2;
    gfx.fillCircle(themeRects_[i].x + 14, cy, 5, t.accent);
    gfx.fillCircle(themeRects_[i].x + 30, cy, 5, t.good);
    gfx.fillCircle(themeRects_[i].x + 46, cy, 5, t.warn);
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(t.text);
    gfx.setCursor(themeRects_[i].x + 62, cy - 8);
    gfx.print(t.name);
    if (i == active) {
      gfx.setTextColor(t.accent);
      gfx.setCursor(themeRects_[i].x + themeRects_[i].w - 24, cy - 8);
      gfx.print("*");
    }
    y += rowH;
  }
}

bool SettingsApp::handleThemes(const InputEvent& event) {
  if (event.action == InputAction::SwipeRight || event.action == InputAction::Back ||
      event.action == InputAction::Cancel) {
    go(Screen::Root);
    return true;
  }
  if (event.action == InputAction::Tap && services_.settings != nullptr) {
    for (uint8_t i = 0; i < theme::kThemeCount; i++) {
      if (themeRects_[i].contains(event.x, event.y)) {
        services_.settings->setThemeIndex(i);
        theme::applyTheme(i);  // live: the very next frame is in the new palette
        dirty_ = true;
        return true;
      }
    }
    return true;
  }
  return false;
}

void SettingsApp::renderRoot(Arduino_GFX& gfx) {
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Settings");

  // Eight settings fit above the rounded lower bezel with 42 px targets. This
  // keeps Assistant discoverable without hiding it behind a gesture or making
  // the root screen scroll.
  const char* labels[8] = {"Wi-Fi", "Display", "Themes", "Sound", "Assistant", "Video",
                           "About", "Restart"};
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  int16_t y = kTop + 30;
  for (uint8_t i = 0; i < 8; i++) {
    rootRects_[i] = widgets::button(gfx, theme::kPadding, y, w, 42, labels[i], false);
    y += 44;
  }
}

void SettingsApp::renderAssistant(Arduino_GFX& gfx) {
  SettingsService* settings = services_.settings;
  if (settings == nullptr) {
    return;
  }
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Assistant");

  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  const bool realtime = settings->assistantBackend() == AssistantBackend::Realtime;
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, kTop + 44);
  gfx.print("Voice backend");
  const int16_t half = (w - 8) / 2;
  assistantRects_[0] =
      widgets::button(gfx, theme::kPadding, kTop + 66, half, 54, "API", !realtime);
  assistantRects_[1] = widgets::button(gfx, theme::kPadding + half + 8, kTop + 66, half, 54,
                                        "Realtime", realtime);

  gfx.setTextColor(theme::kTextDim);
  if (realtime) {
    widgets::textBlock(gfx, theme::kPadding, kTop + 144, w,
                       "Speech-to-speech over a Realtime session. Push to talk remains on this cube.",
                       theme::kTextSizeSmall, theme::kTextDim);
  } else {
    widgets::textBlock(gfx, theme::kPadding, kTop + 144, w,
                       "Current pipeline: transcribe, answer, then synthesize speech.",
                       theme::kTextSizeSmall, theme::kTextDim);
  }
  gfx.setTextColor(theme::kPanelAlt);
  gfx.setCursor(theme::kPadding, kTop + 218);
  gfx.print("Changes are saved immediately.");
}

bool SettingsApp::handleAssistant(const InputEvent& event) {
  if (event.action == InputAction::SwipeRight || event.action == InputAction::Back ||
      event.action == InputAction::Cancel) {
    go(Screen::Root);
    return true;
  }
  if (event.action == InputAction::Tap && services_.settings != nullptr) {
    if (assistantRects_[0].contains(event.x, event.y)) {
      services_.settings->setAssistantBackend(AssistantBackend::Api);
      dirty_ = true;
    } else if (assistantRects_[1].contains(event.x, event.y)) {
      services_.settings->setAssistantBackend(AssistantBackend::Realtime);
      dirty_ = true;
    }
    return true;
  }
  return false;
}

// Orientation is the whole screen for now. It is its own row rather than a
// line on Display because Display is out of vertical room, and because this
// is about how video is laid out, not about the panel.
void SettingsApp::renderVideo(Arduino_GFX& gfx) {
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Video");

  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  int16_t y = kTop + 48;
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("Orientation");

  const bool upright = services_.settings == nullptr ||
                       services_.settings->videoOrientation() == VideoOrientation::Upright;
  const int16_t half = (w - 8) / 2;
  videoRects_[0] =
      widgets::button(gfx, theme::kPadding, y + 26, half, 50, "Upright", upright);
  videoRects_[1] = widgets::button(gfx, theme::kPadding + half + 8, y + 26, half, 50, "Rotated",
                                   !upright);

  y += 96;
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print(upright ? "Same way up as Home." : "Turn the cube sideways.");
  gfx.setCursor(theme::kPadding, y + 22);
  gfx.print(upright ? "For wearing on a wrist." : "Wider picture, in the hand.");
  gfx.setTextColor(theme::kPanelAlt);
  gfx.setCursor(theme::kPadding, y + 52);
  gfx.print("Takes effect on the next video.");
}

bool SettingsApp::handleVideo(const InputEvent& event) {
  if (event.action == InputAction::SwipeRight || event.action == InputAction::Back ||
      event.action == InputAction::Cancel) {
    go(Screen::Root);
    return true;
  }
  if (event.action == InputAction::Tap && services_.settings != nullptr) {
    if (videoRects_[0].contains(event.x, event.y)) {
      services_.settings->setVideoOrientation(VideoOrientation::Upright);
      dirty_ = true;
    } else if (videoRects_[1].contains(event.x, event.y)) {
      services_.settings->setVideoOrientation(VideoOrientation::Rotated);
      dirty_ = true;
    }
    return true;
  }
  return false;
}

// The whole audio path in one place: speaker on top, microphone below. The
// mic controls were serial-only until now (`recordings gain|normalize|gate`),
// and every control here writes through SettingsService so it survives a
// reboot and cannot disagree with the serial family.
void SettingsApp::renderSound(Arduino_GFX& gfx) {
  SettingsService* settings = services_.settings;
  if (settings == nullptr) {
    return;
  }
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  int16_t y = widgets::header(gfx, "Sound", services_.amoled->shiftX(),
                              services_.amoled->shiftY());

  // ---- Speaker ----
  const uint8_t vol = settings->volumePercent();
  char line[48];
  snprintf(line, sizeof(line), "Volume  %u%%", static_cast<unsigned>(vol));
  widgets::text(gfx, theme::kPadding, y, line, widgets::TextStyle::Body, theme::kText);

  // A meter with the unity mark drawn on it: past that tick the extra gain is
  // digital and clips hot material, so the boost region is shown, not hidden.
  const int16_t barY = y + 30;
  gfx.fillRoundRect(theme::kPadding, barY, w, 8, 4, theme::kPanelAlt);
  gfx.fillRoundRect(theme::kPadding, barY, w * vol / 100, 8,
                    4, vol > Es8311::kUnityVolumePercent ? theme::kWarn : theme::kAccent);
  const int16_t unityX = theme::kPadding + w * Es8311::kUnityVolumePercent / 100;
  gfx.fillRect(unityX, barY - 4, 2, 16, theme::kTextDim);
  widgets::text(gfx, theme::kPadding, barY + 16,
                vol > Es8311::kUnityVolumePercent ? "boost — may distort loud audio"
                                                  : "clean up to the mark",
                widgets::TextStyle::Caption, theme::kTextDim);

  const int16_t bw = (w - 16) / 3;
  const int16_t rowY = barY + 44;
  soundVolDownRect_ = widgets::button(gfx, theme::kPadding, rowY, bw, 50, "-", false);
  soundVolUpRect_ = widgets::button(gfx, theme::kPadding + bw + 8, rowY, bw, 50, "+", false);
  soundToneRect_ =
      widgets::button(gfx, theme::kPadding + 2 * (bw + 8), rowY, bw, 50, "Test", true);

  // ---- Microphone ----
  y = rowY + 66;
  gfx.drawFastHLine(theme::kPadding, y, w, theme::kPanelAlt);
  y += 12;
  snprintf(line, sizeof(line), "Mic gain  %u dB",
           static_cast<unsigned>(settings->micGain()) * 6u);
  widgets::text(gfx, theme::kPadding, y, line, widgets::TextStyle::Body, theme::kText);
  soundGainDownRect_ = widgets::button(gfx, theme::kPadding + w - 2 * (bw / 2) - 8, y - 6,
                                       bw / 2, 44, "-", false);
  soundGainUpRect_ =
      widgets::button(gfx, theme::kPadding + w - bw / 2, y - 6, bw / 2, 44, "+", false);

  y += 54;
  const int16_t toggleW = (w - 8) / 2;
  soundNormalizeRect_ = widgets::button(gfx, theme::kPadding, y, toggleW, 50,
                                        settings->recordNormalize() ? "Level: on" : "Level: off",
                                        settings->recordNormalize());
  soundGateRect_ = widgets::button(gfx, theme::kPadding + toggleW + 8, y, toggleW, 50,
                                   settings->recordGate() ? "Gate: on" : "Gate: off",
                                   settings->recordGate());
  widgets::text(gfx, theme::kPadding, y + 58,
                "Level lifts quiet takes; gate hushes the pauses.",
                widgets::TextStyle::Caption, theme::kTextDim);
}

// Restart is one row away from Display and About; a misplaced tap used to
// reboot the cube outright. Anything that can end an in-progress recording
// gets a confirm — and says so.
void SettingsApp::renderRestartConfirm(Arduino_GFX& gfx) {
  const bool recording = services_.recorder != nullptr && services_.recorder->recording();
  restartConfirmRect_ = widgets::modalConfirm(
      gfx, "Restart cube?",
      recording ? "A recording is running. Restarting now loses it."
                : "The cube reboots. Wi-Fi reconnects on its own.",
      restartCancelRect_);
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
  wifiNetworkCount_ = count;
  // A rescan can shrink the list out from under the current page.
  if (count > 0 && wifiPage_ * 4 >= count) {
    wifiPage_ = 0;
  }
  int16_t y = kTop + 126;
  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t idx = wifiPage_ * 4 + i;
    if (results == nullptr || idx >= count) {
      wifiRowRects_[i] = widgets::Rect{};
      wifiRowSsids_[i][0] = '\0';
      continue;
    }
    // Remember what this row says; the tap handler must connect to the SSID
    // the user saw, not to whatever occupies this slot by then.
    strncpy(wifiRowSsids_[i], results[idx].ssid, sizeof(wifiRowSsids_[i]) - 1);
    wifiRowSsids_[i][sizeof(wifiRowSsids_[i]) - 1] = '\0';
    char secondary[48];
    snprintf(secondary, sizeof(secondary), "%d dBm - %s%s", (int)results[idx].rssi,
             results[idx].secure ? "locked" : "open", results[idx].saved ? " - saved" : "");
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
    snprintf(pager, sizeof(pager), "%u networks - swipe up/down", (unsigned)count);
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
  y += 72;

  // Only the on/off switch fits here; the window and its brightness stay at
  // their defaults, shown below so the toggle is never a mystery.
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("Bedtime dim");
  displayRects_[5] = widgets::button(gfx, theme::kPadding + w - 110, y - 8, 110, 46,
                                     settings->bedtimeEnabled() ? "on" : "off",
                                     settings->bedtimeEnabled());

  char window[32];
  snprintf(window, sizeof(window), "%02u:%02u-%02u:%02u at %u",
           (unsigned)(settings->bedtimeStartMin() / 60),
           (unsigned)(settings->bedtimeStartMin() % 60),
           (unsigned)(settings->bedtimeEndMin() / 60),
           (unsigned)(settings->bedtimeEndMin() % 60),
           (unsigned)settings->bedtimeBrightness());
  gfx.setTextColor(theme::kPanelAlt);
  gfx.setCursor(theme::kPadding, y + 54);
  gfx.print(window);
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
      if (confirmRestart_) {
        if (event.action == InputAction::Tap) {
          if (restartConfirmRect_.contains(event.x, event.y)) {
            Serial.println("restarting from Settings...");
            Serial.flush();
            delay(100);
            ESP.restart();
          }
          confirmRestart_ = false;  // anything else, including Cancel, backs out
          dirty_ = true;
          return true;
        }
        if (back) {
          confirmRestart_ = false;
          dirty_ = true;
        }
        return true;  // absorb everything else while the modal is up
      }
      if (event.action == InputAction::Tap) {
        if (rootRects_[0].contains(event.x, event.y)) {
          go(Screen::Wifi);
        } else if (rootRects_[1].contains(event.x, event.y)) {
          go(Screen::Display);
        } else if (rootRects_[2].contains(event.x, event.y)) {
          go(Screen::Themes);
        } else if (rootRects_[3].contains(event.x, event.y)) {
          go(Screen::Sound);
        } else if (rootRects_[4].contains(event.x, event.y)) {
          go(Screen::Assistant);
        } else if (rootRects_[5].contains(event.x, event.y)) {
          go(Screen::Video);
        } else if (rootRects_[6].contains(event.x, event.y)) {
          go(Screen::About);
        } else if (rootRects_[7].contains(event.x, event.y)) {
          confirmRestart_ = true;
          dirty_ = true;
        }
        return true;
      }
      return false;  // Back falls through to the router (home)

    case Screen::Sound: {
      if (back || event.action == InputAction::SwipeRight) {
        go(Screen::Root);
        return true;
      }
      if (event.action != InputAction::Tap || services_.settings == nullptr) {
        return true;
      }
      SettingsService* settings = services_.settings;
      AudioAdapter* audio = services_.audio;
      if (soundVolDownRect_.contains(event.x, event.y) ||
          soundVolUpRect_.contains(event.x, event.y)) {
        const int step = soundVolUpRect_.contains(event.x, event.y) ? 5 : -5;
        int next = static_cast<int>(settings->volumePercent()) + step;
        next = next < 0 ? 0 : (next > 100 ? 100 : next);
        settings->setVolumePercent(static_cast<uint8_t>(next));
        if (audio != nullptr) {
          audio->setVolumePercent(static_cast<uint8_t>(next));
        }
      } else if (soundToneRect_.contains(event.x, event.y)) {
        // playTone() refuses while a capture or playback owns the codec, so a
        // stray tap during a recording cannot cut the take short.
        if (audio != nullptr) {
          audio->playTone(880, 400);
        }
      } else if (soundGainDownRect_.contains(event.x, event.y) ||
                 soundGainUpRect_.contains(event.x, event.y)) {
        const int step = soundGainUpRect_.contains(event.x, event.y) ? 1 : -1;
        int next = static_cast<int>(settings->micGain()) + step;
        next = next < 0 ? 0 : (next > 7 ? 7 : next);
        settings->setMicGain(static_cast<uint8_t>(next));
        if (audio != nullptr && !audio->isRecording()) {
          audio->setMicGain(static_cast<uint8_t>(next));  // codec is live: never mid-take
        }
      } else if (soundNormalizeRect_.contains(event.x, event.y)) {
        const bool on = !settings->recordNormalize();
        settings->setRecordNormalize(on);
        if (audio != nullptr) {
          audio->setRecordNormalize(on);
        }
      } else if (soundGateRect_.contains(event.x, event.y)) {
        const bool on = !settings->recordGate();
        settings->setRecordGate(on);
        if (audio != nullptr) {
          audio->setRecordGate(on);
        }
      }
      dirty_ = true;
      return true;
    }

    case Screen::Assistant:
      return handleAssistant(event);

    case Screen::Wifi:
      if (back || event.action == InputAction::SwipeRight) {
        go(Screen::Root);
        return true;
      }
      if (event.action == InputAction::SwipeUp || event.action == InputAction::SwipeDown) {
        // Page against what was rendered, not against a live re-read: paging
        // past the end of the visible list is the same staleness bug as
        // tapping the wrong row.
        const uint8_t pages = wifiNetworkCount_ == 0 ? 1 : (wifiNetworkCount_ + 3) / 4;
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
        for (uint8_t i = 0; i < 4; i++) {
          if (wifiRowSsids_[i][0] != '\0' && wifiRowRects_[i].contains(event.x, event.y)) {
            strncpy(chosenSsid_, wifiRowSsids_[i], sizeof(chosenSsid_) - 1);
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
          if (next < MIN_BRIGHTNESS) next = MIN_BRIGHTNESS;
          if (next > MAX_BRIGHTNESS) next = MAX_BRIGHTNESS;
          // Setting only — AmoledProtection owns the panel and follows within
          // a frame. Two writers would fight over dim and blank.
          settings->setBrightness(static_cast<uint8_t>(next));
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
        } else if (displayRects_[5].contains(event.x, event.y)) {
          settings->setBedtimeEnabled(!settings->bedtimeEnabled());
          dirty_ = true;
        }
        return true;
      }
      return false;

    case Screen::Themes:
      return handleThemes(event);

    case Screen::Video:
      return handleVideo(event);

    case Screen::About:
      if (back || event.action == InputAction::SwipeRight || event.action == InputAction::Tap) {
        go(Screen::Root);
        return true;
      }
      return false;
  }
  return false;
}
