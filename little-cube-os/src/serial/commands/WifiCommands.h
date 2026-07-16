#pragma once

struct Services;

// Password capture handshake between the dispatcher and the wifi family:
// `wifi connect "<ssid>"` arms this, and the NEXT serial line is treated as
// the password — dispatched straight to WifiService, never echoed, never
// logged, and the line buffer is wiped afterwards.
struct PasswordPrompt {
  bool active = false;
  bool hidden = false;
  char ssid[33] = "";
};

bool handleWifiCommand(Services& services, PasswordPrompt& prompt, const char* verb, char* args);
void printWifiHelp();
