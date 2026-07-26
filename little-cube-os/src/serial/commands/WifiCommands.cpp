#include "WifiCommands.h"

#include <Arduino.h>
#include <WiFi.h>

#include "../../core/Services.h"
#include "../../services/WifiService.h"
#include "../CmdArgs.h"

void printWifiHelp() {
  Serial.println("wifi scan                     scan nearby networks");
  Serial.println("wifi connect \"<ssid>\"         connect (password prompted on the next line)");
  Serial.println("wifi connect \"<ssid>\" open    connect to an open network");
  Serial.println("wifi connect \"<ssid>\" hidden  hidden SSID (password prompted)");
  Serial.println("wifi status                   current state");
  Serial.println("wifi disconnect               drop the connection");
  Serial.println("wifi forget \"<ssid>\"          remove a saved network");
  Serial.println("wifi offline on|off           radio kill-switch");
}

bool handleWifiCommand(Services& services, PasswordPrompt& prompt, const char* verb,
                       char* args) {
  WifiService* wifi = services.wifi;
  if (wifi == nullptr) {
    return false;
  }

  if (strcmp(verb, "scan") == 0) {
    Serial.println("scanning...");
    wifi->startScan(true /*print when done*/);
    return true;
  }

  if (strcmp(verb, "status") == 0) {
    Serial.printf("state: %s\n", wifiStateName(wifi->state()));
    const String ssid = wifi->currentSsid();
    if (ssid.length() > 0) {
      Serial.printf("network: %s (%d dBm)\n", ssid.c_str(), (int)WiFi.RSSI());
      Serial.printf("ip: %s\n", WiFi.localIP().toString().c_str());
    }
    Serial.printf("saved networks: %u\n", (unsigned)wifi->savedCount());
    Serial.printf("offline mode: %s\n", wifi->offlineMode() ? "on" : "off");
    return true;
  }

  if (strcmp(verb, "connect") == 0) {
    char* cursor = args;
    const char* ssid = cmdargs::nextToken(cursor);
    const char* flag = cmdargs::nextToken(cursor);
    if (ssid == nullptr || strlen(ssid) > 32) {
      Serial.println("usage: wifi connect \"<ssid>\" [open|hidden]");
      return true;
    }
    if (flag != nullptr && strcmp(flag, "open") == 0) {
      Serial.printf("Connecting to %s...\n", ssid);
      wifi->connectTo(ssid, "", false);
      return true;
    }
    prompt.active = true;
    prompt.hidden = flag != nullptr && strcmp(flag, "hidden") == 0;
    strncpy(prompt.ssid, ssid, sizeof(prompt.ssid) - 1);
    prompt.ssid[sizeof(prompt.ssid) - 1] = '\0';
    // Honest wording: this build has no flash encryption, so NVS is plaintext
    // to anyone who can run `esptool read_flash` on the cube. The prompt goes
    // last so it is the line the cursor sits under.
    Serial.println("note: most terminals still show what you type. The cube never echoes or"
                   " logs the password and never writes it to the SD card, but it is stored");
    Serial.println("      in NVS flash, which is NOT encrypted on this board — anyone with"
                   " physical access can read it back.");
    Serial.println("Password:");
    return true;
  }

  if (strcmp(verb, "disconnect") == 0) {
    wifi->disconnect();
    Serial.println("disconnected");
    return true;
  }

  if (strcmp(verb, "forget") == 0) {
    char* cursor = args;
    const char* ssid = cmdargs::nextToken(cursor);
    if (ssid == nullptr) {
      Serial.println("usage: wifi forget \"<ssid>\"");
      return true;
    }
    Serial.println(wifi->forget(ssid) ? "forgotten" : "error: not a saved network");
    return true;
  }

  if (strcmp(verb, "offline") == 0) {
    char* cursor = args;
    const char* state = cmdargs::nextToken(cursor);
    if (state == nullptr || (strcmp(state, "on") != 0 && strcmp(state, "off") != 0)) {
      Serial.println("usage: wifi offline on|off");
      return true;
    }
    wifi->setOfflineMode(strcmp(state, "on") == 0);
    Serial.printf("offline mode %s\n", state);
    return true;
  }

  return false;
}
