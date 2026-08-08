#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../services/WifiService.h"
#include "../ui/StatusBar.h"
#include "../ui/Theme.h"
#include "../ui/widgets/Widgets.h"

// Settings (spec §28) with the on-device Wi-Fi flow (spec §21): scan ->
// pick a network -> choose Phone Setup (portal) or USB Serial. Display
// section covers brightness / timeout / always-on; About shows identity.
class SettingsApp : public App {
 public:
  explicit SettingsApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override;
  void onResume() override { dirty_ = true; }

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  enum class Screen {
    Root,
    Wifi,
    WifiMethod,  // per-network chooser: phone setup / serial hint
    SetupMode,   // portal live: AP name + password + address
    Display,
    Themes,
    Sound,
    Assistant,
    Video,
    About,
  };

  void go(Screen screen);
  void leaveSetupMode();
  void renderRoot(Arduino_GFX& gfx);
  void renderRestartConfirm(Arduino_GFX& gfx);
  void renderWifi(Arduino_GFX& gfx);
  void renderWifiMethod(Arduino_GFX& gfx);
  void renderSetupMode(Arduino_GFX& gfx);
  void renderDisplay(Arduino_GFX& gfx);
  void renderThemes(Arduino_GFX& gfx);
  void renderSound(Arduino_GFX& gfx);
  void renderAssistant(Arduino_GFX& gfx);
  bool handleAssistant(const InputEvent& event);
  bool handleThemes(const InputEvent& event);
  void renderVideo(Arduino_GFX& gfx);
  bool handleVideo(const InputEvent& event);
  void renderAbout(Arduino_GFX& gfx);

  Services& services_;
  StatusBar statusBar_;
  Screen screen_ = Screen::Root;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;
  uint32_t pollAccumMs_ = 0;

  widgets::Rect rootRects_[8];
  widgets::Rect assistantRects_[2];
  // Video screen: the two orientation choices.
  widgets::Rect videoRects_[2];
  // Sound screen: volume -/+, test tone, mic gain -/+, normalize, gate.
  widgets::Rect soundVolDownRect_;
  widgets::Rect soundVolUpRect_;
  widgets::Rect soundToneRect_;
  widgets::Rect soundGainDownRect_;
  widgets::Rect soundGainUpRect_;
  widgets::Rect soundNormalizeRect_;
  widgets::Rect soundGateRect_;
  widgets::Rect themeRects_[theme::kThemeCount];
  bool confirmRestart_ = false;
  widgets::Rect restartConfirmRect_;
  widgets::Rect restartCancelRect_;

  widgets::Rect wifiScanRect_;
  widgets::Rect wifiPhoneRect_;
  widgets::Rect wifiRowRects_[4];
  // The SSIDs actually drawn in those rows. A scan finishing between the
  // render and the tap reshuffles scanResults(), so reading it at tap time
  // could hand the user a different network than the one they touched.
  char wifiRowSsids_[4][33] = {};
  uint8_t wifiNetworkCount_ = 0;  // networks as of the last render
  uint8_t wifiPage_ = 0;
  char chosenSsid_[33] = "";
  widgets::Rect methodPhoneRect_;
  widgets::Rect methodSerialRect_;
  widgets::Rect displayRects_[6];
};
