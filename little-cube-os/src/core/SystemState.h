#pragma once

#include <Arduino.h>

#include "../hardware/SdCardState.h"
#include "../services/WifiState.h"

// Shared, cheap-to-read snapshot of device status for the status bar,
// Today card, and serial `status`. Services keep this current; readers
// never call into hardware.
struct SystemState {
  // Bumped whenever any field below changes; cheap dirty check for UI
  // (apps re-render when the version moves).
  uint32_t version = 0;

  char clockHhMm[6] = "--:--";
  bool timeValid = false;

  WifiState wifi = WifiState::Idle;
  bool internet = false;

  SdCardState sd = SdCardState::NotPresent;

  bool recording = false;
  bool alarmArmed = false;
  bool playingAudio = false;

  bool batteryPresent = false;
  int batteryPercent = -1;
  bool charging = false;
};
