#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../services/WifiService.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

// Settings (spec §28) with the on-device Wi-Fi flow (spec §21): scan ->
// pick a network -> choose Phone Setup (portal) or USB Serial. Display
// section covers brightness / timeout / always-on; About shows identity.
class SettingsApp : public App {
 public:
  explicit SettingsApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override {}
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
    About,
  };

  void go(Screen screen);
  void renderRoot(Arduino_GFX& gfx);
  void renderWifi(Arduino_GFX& gfx);
  void renderWifiMethod(Arduino_GFX& gfx);
  void renderSetupMode(Arduino_GFX& gfx);
  void renderDisplay(Arduino_GFX& gfx);
  void renderAbout(Arduino_GFX& gfx);

  Services& services_;
  StatusBar statusBar_;
  Screen screen_ = Screen::Root;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;
  uint32_t pollAccumMs_ = 0;

  widgets::Rect rootRects_[4];
  widgets::Rect wifiScanRect_;
  widgets::Rect wifiPhoneRect_;
  widgets::Rect wifiRowRects_[4];
  uint8_t wifiPage_ = 0;
  char chosenSsid_[33] = "";
  widgets::Rect methodPhoneRect_;
  widgets::Rect methodSerialRect_;
  widgets::Rect displayRects_[6];
};
