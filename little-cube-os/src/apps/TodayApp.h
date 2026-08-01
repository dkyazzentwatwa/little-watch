#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../ui/StatusBar.h"

class Arduino_GFX;

// Today (spec §11): the default glance screen, drawn as a neofetch-style
// system readout — an ASCII cube over `key value` rows in the built-in 6x8
// bitmap font. Every fact on it is a RAM read (SystemState, the cached
// weather snapshot, TimeService's cached tm, millis(), the settings device
// name), so the screen needs no onOpen() load and render() never touches the
// SD card or the network. See TodayApp.cpp for the grid, the measured
// envelopes, and why the calendar and alarm rows were removed rather than
// left showing a dash.
class TodayApp : public App {
 public:
  explicit TodayApp(Services& services) : services_(services) {}

  void onOpen() override { dirty_ = true; }
  void onClose() override {}
  void onPause() override {}
  void onResume() override { dirty_ = true; }

  void update(uint32_t deltaMs) override { (void)deltaMs; }
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  // Both assume the caller has already put the canvas in the built-in font at
  // the readout's text size; neither restores it.
  void drawRow(Arduino_GFX& gfx, int row, const char* label, const char* value,
               uint16_t valueColor, int16_t shiftX, int16_t shiftY) const;
  void drawBatteryRow(Arduino_GFX& gfx, int row, const SystemState& state, int16_t shiftX,
                      int16_t shiftY) const;

  Services& services_;
  StatusBar statusBar_;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;
};
