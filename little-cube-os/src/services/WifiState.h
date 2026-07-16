#pragma once

// Wi-Fi service states (spec §20). Distinct failure states are part of the
// product: "wrong password" and "network gone" must never collapse into a
// generic error.
enum class WifiState {
  Disabled,
  Idle,
  Scanning,
  NetworksFound,
  Connecting,
  Connected,
  ConnectedNoInternet,
  AuthenticationFailed,
  NetworkNotFound,
  CaptivePortalSuspected,
  Disconnected,
  Error,
};

const char* wifiStateName(WifiState state);
