#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../ui/StatusBar.h"

// Voice assistant UI (docs/superpowers/specs/2026-07-25-voice-assistant-design.md):
// one big talk button (tap = listen, tap again = send), live pipeline status,
// and the last exchange's transcript + answer. All work happens in
// AssistantService; this app only reflects its state and forwards taps.
class AssistantApp : public App {
 public:
  explicit AssistantApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override {}
  void onResume() override { dirty_ = true; }

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  struct Rect {
    int16_t x = 0, y = 0, w = 0, h = 0;
    bool contains(int16_t px, int16_t py) const {
      return px >= x && px < x + w && py >= y && py < y + h;
    }
  };

  Services& services_;
  StatusBar statusBar_;
  Rect talkRect_;
  uint8_t lastState_ = 255;
  uint32_t lastStateVersion_ = 0;
  uint32_t tickMs_ = 0;
  bool dirty_ = true;
};
