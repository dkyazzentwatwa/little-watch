#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../ui/StatusBar.h"

// Today (spec §11): the default practical dashboard — time, date, weather,
// and device status at a glance. Calendar/alarm/timer slots show a quiet
// dash until those features land.
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
  Services& services_;
  StatusBar statusBar_;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;
};
