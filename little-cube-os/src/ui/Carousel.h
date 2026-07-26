#pragma once

#include <Arduino.h>

#include "../core/App.h"
#include "Icons.h"

class Arduino_GFX;
struct Services;

// One-card-at-a-time Home carousel (spec §7): swipe left/right slides
// between app cards, tap opens the focused card. Card order per spec §6,
// with a final Tools card grouping Files/Contacts/Calculator. A compile
// time 2x2 grid variant exists behind LITTLECUBE_HOME_LAYOUT.
class Carousel {
 public:
  struct Card {
    AppId id;
    const char* title;
    const char* hint;  // shown when the app has no live fact to report
    icons::IconId icon;
    bool tools;  // pseudo-card: opens the Tools screen, not an app
  };

  void begin(Services* services);

  void update(uint32_t deltaMs);
  void render(Arduino_GFX& gfx, int16_t topY);

  bool animating() const { return offsetPx_ != 0; }
  // True once, on the tick the slide reaches its final position, so the
  // caller can draw the settled frame.
  bool consumeSettled() {
    const bool s = settled_;
    settled_ = false;
    return s;
  }
  const Card& focused() const;
  uint8_t index() const { return index_; }
  uint8_t count() const;

  void next();
  void previous();

 private:
  void renderCard(Arduino_GFX& gfx, uint8_t cardIndex, int16_t xOffset, int16_t topY);

  Services* services_ = nullptr;
  bool settled_ = false;
  uint8_t index_ = 0;
  // Pixel offset of the focused card from center; eases back to 0.
  int16_t offsetPx_ = 0;
};
