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
  Reader,
  News,
  Assistant,
};

constexpr uint8_t kAppCount = 15;

const char* appName(AppId id);

// Base class every app implements (spec §10). Only one app renders at a
// time; apps keep lightweight state across close/reopen. All methods must
// be non-blocking — long work belongs in services.
//
// Lifecycle contract — the short version is that onResume() is onOpen()
// minus the navigation reset:
//
//   onOpen()    fresh entry. Reset to the root screen and load what you show.
//   onPause()   backgrounded but still on the back stack, to be revived later.
//               Navigation is preserved. Release anything that would be a lie
//               or a hazard while unattended (a live hotspot, an armed delete
//               confirm) — but never anything the user asked to keep running,
//               such as a recording or playback.
//   onResume()  revived from the stack. Navigation is preserved, but the data
//               behind it may have moved on while you were away, so refresh it.
//   onClose()   leaving for good. Drop heavy buffers; the next visit is onOpen().
//
// Every app is constructed once at boot and never destroyed, so state that
// survives onClose() survives forever — release it deliberately.
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
