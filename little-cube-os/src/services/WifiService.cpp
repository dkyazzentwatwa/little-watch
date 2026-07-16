#include "WifiService.h"

const char* wifiStateName(WifiState state) {
  switch (state) {
    case WifiState::Disabled: return "disabled";
    case WifiState::Idle: return "idle";
    case WifiState::Scanning: return "scanning";
    case WifiState::NetworksFound: return "networks found";
    case WifiState::Connecting: return "connecting";
    case WifiState::Connected: return "connected";
    case WifiState::ConnectedNoInternet: return "connected, no internet";
    case WifiState::AuthenticationFailed: return "authentication failed";
    case WifiState::NetworkNotFound: return "network not found";
    case WifiState::CaptivePortalSuspected: return "captive portal suspected";
    case WifiState::Disconnected: return "disconnected";
    case WifiState::Error: return "error";
  }
  return "?";
}

// TODO(task-12): async scan, saved networks in NVS, auto-reconnect,
// connectivity probe (204), captive-portal detection, disconnect-reason
// mapping to AuthenticationFailed vs NetworkNotFound.

void WifiService::begin(EventBus* events, SystemState* state) {
  events_ = events;
  systemState_ = state;
  state_ = WifiState::Idle;
}

void WifiService::update(uint32_t deltaMs) {
  (void)deltaMs;
}

void WifiService::startScan() {}

bool WifiService::connectTo(const char* ssid, const char* password, bool hidden) {
  (void)ssid;
  (void)password;
  (void)hidden;
  return false;
}

void WifiService::disconnect() {}

void WifiService::forget(const char* ssid) {
  (void)ssid;
}

void WifiService::setOfflineMode(bool offline) {
  offline_ = offline;
}
