#pragma once

#include <Arduino.h>

#include "WifiState.h"

class EventBus;
struct SystemState;

// Wi-Fi state machine (spec §20). Credentials live in NVS only, are never
// logged, never echoed, never written to SD. Scanning and connecting are
// asynchronous — update() is ticked by the kernel and must not block.
class WifiService {
 public:
  void begin(EventBus* events, SystemState* state);
  void update(uint32_t deltaMs);

  WifiState state() const { return state_; }

  void startScan();
  bool connectTo(const char* ssid, const char* password, bool hidden = false);
  void disconnect();
  void forget(const char* ssid);
  void setOfflineMode(bool offline);
  bool offlineMode() const { return offline_; }

 private:
  EventBus* events_ = nullptr;
  SystemState* systemState_ = nullptr;
  WifiState state_ = WifiState::Idle;
  bool offline_ = false;
};
