#pragma once

#include <Arduino.h>

class WifiService;
class SettingsService;

// Phone-assisted Wi-Fi setup (spec §22): temporary SoftAP "LittleCube-XXXX"
// with a captive setup portal at 192.168.4.1. Provisioning-only lifecycle —
// times out on inactivity and tears down after a successful save.
class ProvisioningService {
 public:
  void begin(WifiService* wifi, SettingsService* settings);
  void update(uint32_t deltaMs);

  bool start();
  void stop();
  bool active() const { return active_; }

  // AP name shown on the cube screen while setup mode is live.
  String apName() const { return apName_; }

 private:
  WifiService* wifi_ = nullptr;
  SettingsService* settings_ = nullptr;
  String apName_;
  bool active_ = false;
  uint32_t idleMs_ = 0;
};
