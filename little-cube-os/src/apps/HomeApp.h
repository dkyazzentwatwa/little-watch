#pragma once

#include "../core/App.h"
#include "../core/Services.h"

class HomeApp : public App {
 public:
  explicit HomeApp(Services& services) : services_(services) {}

  void onOpen() override {}
  void onClose() override {}
  void onPause() override {}
  void onResume() override {}

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  Services& services_;
};
