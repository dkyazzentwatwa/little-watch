#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../ui/StatusBar.h"

// Weather (spec §13): current conditions + 3-day outlook, always labelled
// with freshness. Tap refreshes when online; offline shows the cached
// snapshot with its age, never pretending it is current.
class WeatherApp : public App {
 public:
  explicit WeatherApp(Services& services) : services_(services) {}

  void onOpen() override { dirty_ = true; }
  void onClose() override {}
  void onPause() override {}
  void onResume() override { dirty_ = true; }

  void update(uint32_t deltaMs) override { (void)deltaMs; }
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  Services& services_;
  StatusBar statusBar_;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;
};
