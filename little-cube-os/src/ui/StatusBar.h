#pragma once

#include <Arduino.h>

class Arduino_GFX;
struct SystemState;

// Shared status area (spec §8): time, Wi-Fi, SD, recording, alarm, battery.
// Drawn into the frame canvas each render; honors the global pixel-shift
// offsets so persistent glyphs never burn in.
class StatusBar {
 public:
  void render(Arduino_GFX& gfx, const SystemState& state, int16_t shiftX, int16_t shiftY);
};
