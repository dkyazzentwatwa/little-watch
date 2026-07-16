#pragma once

#include <Arduino.h>

class WifiService;
class SettingsService;

// Phone-assisted Wi-Fi setup (spec §22): a temporary SoftAP
// "LittleCube-XXXX" protected by a device-specific password (shown on the
// cube screen), a captive DNS, and a one-page portal at 192.168.4.1 with
// network scan, SSID/password (show-hide), hidden-network, device name,
// timezone, and weather city. /save triggers a live connection test in
// AP+STA mode; on success credentials persist (NVS, via WifiService) and
// the AP tears itself down. Inactivity times the portal out. Passwords are
// never logged and never echoed back into the page.
class ProvisioningService {
 public:
  enum class Phase {
    Off,
    Portal,      // AP up, waiting for the form
    Testing,     // /save received, STA connect in flight
    Success,     // connected; AP shutting down after a grace period
    Failed,      // test failed; portal stays up with the error shown
  };

  void begin(WifiService* wifi, SettingsService* settings);
  void update(uint32_t deltaMs);

  bool start();
  void stop();
  bool active() const { return phase_ != Phase::Off; }
  Phase phase() const { return phase_; }

  String apName() const { return apName_; }
  String apPassword() const { return apPassword_; }
  String portalIp() const { return portalIp_; }
  const char* lastError() const { return lastError_; }

 private:
  void handleRoot();
  void handleSave();
  void handleStatus();
  void teardown();

  WifiService* wifi_ = nullptr;
  SettingsService* settings_ = nullptr;

  Phase phase_ = Phase::Off;
  String apName_;
  String apPassword_;
  String portalIp_;
  const char* lastError_ = "";

  String pendingSsid_;
  String pendingPassword_;
  bool pendingHidden_ = false;
  bool testStarted_ = false;

  uint32_t idleMs_ = 0;
  uint32_t successMs_ = 0;
};
