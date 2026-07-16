#include "Carousel.h"

// TODO(task-5): card order per spec §6 (Today, Clock, Weather, Notes,
// Recorder, Audio, Calendar, Settings + Tools), canvas slide animation,
// grid variant behind the compile-time flag.

void Carousel::begin(Services* services) {
  services_ = services;
}

void Carousel::update(uint32_t deltaMs) {
  (void)deltaMs;
}

void Carousel::render(Arduino_GFX& gfx) {
  (void)gfx;
}

AppId Carousel::focusedApp() const {
  return AppId::Today;
}

void Carousel::next() {}

void Carousel::previous() {}
