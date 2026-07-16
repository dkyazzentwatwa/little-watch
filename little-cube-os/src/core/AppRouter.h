#pragma once

#include <Arduino.h>

#include "App.h"

// Owns which app is in the foreground (spec §10): open/close, back stack,
// pause/resume, and Home restore. Apps are registered once at boot and
// never destroyed — closing preserves their lightweight state.
class AppRouter {
 public:
  void registerApp(AppId id, App* app);
  void begin(AppId initial);

  void open(AppId id);
  void back();
  void home();

  AppId currentId() const { return currentId_; }
  App* current() { return find(currentId_); }

  void update(uint32_t deltaMs);
  void render();

  // Feed one semantic input event through the foreground app; unconsumed
  // Back/Home actions are handled here.
  void handleInput(const InputEvent& event);

 private:
  App* find(AppId id);

  struct Entry {
    AppId id = AppId::Home;
    App* app = nullptr;
  };
  Entry entries_[kAppCount];
  uint8_t count_ = 0;

  static constexpr uint8_t kMaxStack = 8;
  AppId stack_[kMaxStack];
  uint8_t depth_ = 0;

  AppId currentId_ = AppId::Home;
  bool started_ = false;
};
