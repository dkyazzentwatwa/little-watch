#pragma once

#include <Arduino.h>

#include "../core/App.h"

class Arduino_GFX;
struct Services;

// One-card-at-a-time Home carousel (spec §7): swipe left/right animates
// between app cards, tap opens. A compile-time 2x2 grid variant exists
// behind LITTLECUBE_HOME_LAYOUT (feature_flags.h).
class Carousel {
 public:
  void begin(Services* services);

  void update(uint32_t deltaMs);
  void render(Arduino_GFX& gfx);

  // Returns the AppId to open when the user taps the focused card.
  AppId focusedApp() const;
  void next();
  void previous();

 private:
  Services* services_ = nullptr;
  uint8_t index_ = 0;
};
