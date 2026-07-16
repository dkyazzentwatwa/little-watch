#pragma once

#include <Arduino.h>

#include "InputAction.h"

// Application identifiers (spec §10).
enum class AppId : uint8_t {
  Home,
  Today,
  Clock,
  Weather,
  Calendar,
  Notes,
  Recorder,
  Audio,
  Files,
  Contacts,
  Calculator,
  Settings,
};

constexpr uint8_t kAppCount = 12;

const char* appName(AppId id);

// Base class every app implements (spec §10). Only one app renders at a
// time; apps keep lightweight state across close/reopen. All methods must
// be non-blocking — long work belongs in services.
class App {
 public:
  virtual ~App() = default;

  virtual void onOpen() = 0;
  virtual void onClose() = 0;
  virtual void onPause() = 0;
  virtual void onResume() = 0;

  virtual void update(uint32_t deltaMs) = 0;
  virtual void render() = 0;

  // Return true when the event was consumed; unconsumed Back/Home fall
  // through to the router.
  virtual bool handleInput(const InputEvent& event) = 0;
};
