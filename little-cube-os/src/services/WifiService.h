#pragma once

#include <Arduino.h>

#include "WifiState.h"

class EventBus;
struct SystemState;

// Wi-Fi state machine (spec §20). Credentials live in NVS only ("lc-wifi"),
// are never logged, never echoed, never written to SD; the RAM copies used
// during a connect are wiped afterwards. Scanning and connecting are
// asynchronous; the connectivity probe (HTTP 204) runs on a short-lived
// FreeRTOS task so the UI loop never blocks. Distinct outcomes per spec:
// wrong password -> AuthenticationFailed, missing SSID -> NetworkNotFound,
// probe redirect -> CaptivePortalSuspected, probe failure ->
// ConnectedNoInternet.
class WifiService {
 public:
  struct ScanResult {
    char ssid[33] = "";
    int32_t rssi = 0;
    bool secure = false;
    bool saved = false;
  };
  static constexpr uint8_t kMaxScanResults = 20;
  static constexpr uint8_t kMaxSaved = 5;

  void begin(EventBus* events, SystemState* state);
  void update(uint32_t deltaMs);

  WifiState state() const { return state_; }
  bool internet() const { return internet_; }
  String currentSsid() const;

  void startScan(bool printWhenDone = false);
  const ScanResult* scanResults(uint8_t& count) const {
    count = scanCount_;
    return results_;
  }

  bool connectTo(const char* ssid, const char* password, bool hidden = false);
  void disconnect();
  bool forget(const char* ssid);
  void setOfflineMode(bool offline);
  bool offlineMode() const { return offline_; }

  uint8_t savedCount() const { return savedCount_; }
  bool isSaved(const char* ssid) const;

  // Called from the WiFi event callback only.
  void noteDisconnectReason(uint8_t reason) { lastDisconnectReason_ = reason; }

 private:
  friend void wifiProbeTask(void* arg);

  void setState(WifiState next);
  void loadSaved();
  void persistSaved();
  void rememberNetwork(const char* ssid, const char* password);
  bool tryNextSaved();
  void onConnected();
  void startProbe();
  void wipePending();

  EventBus* events_ = nullptr;
  SystemState* systemState_ = nullptr;
  WifiState state_ = WifiState::Idle;
  bool offline_ = false;
  bool internet_ = false;

  ScanResult results_[kMaxScanResults];
  uint8_t scanCount_ = 0;
  bool scanPending_ = false;
  bool scanPrint_ = false;

  struct SavedNet {
    String ssid;
    String pass;
  };
  SavedNet saved_[kMaxSaved];
  uint8_t savedCount_ = 0;
  int8_t autoIndex_ = -1;  // next saved network to try at boot / reconnect

  String pendingSsid_;
  String pendingPass_;
  bool pendingHidden_ = false;
  bool pendingSave_ = false;
  uint32_t connectStartMs_ = 0;

  volatile int probeResult_ = 0;  // 0 idle/pending, 1 internet, 2 none, 3 captive
  bool probeRunning_ = false;

  volatile uint8_t lastDisconnectReason_ = 0;
  uint32_t reconnectAccumMs_ = 0;
};
