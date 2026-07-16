#include "Carousel.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "Theme.h"
#include "widgets/Widgets.h"

namespace {

constexpr Carousel::Card kCards[] = {
    {AppId::Today, "Today", "at a glance", false},
    {AppId::Clock, "Clock", "time & alarms", false},
    {AppId::Weather, "Weather", "forecast", false},
    {AppId::Notes, "Notes", "read & capture", false},
    {AppId::Recorder, "Recorder", "voice notes", false},
    {AppId::Audio, "Audio", "sound", false},
    {AppId::Calendar, "Calendar", "agenda", false},
    {AppId::Settings, "Settings", "device", false},
    {AppId::Files, "Tools", "files & more", true},
};
constexpr uint8_t kCardCount = sizeof(kCards) / sizeof(kCards[0]);

// Slide easing: proportional decay, ~120 ms to settle.
constexpr uint32_t kEaseDivisorMs = 120;

}  // namespace

void Carousel::begin(Services* services) {
  services_ = services;
}

uint8_t Carousel::count() const {
  return kCardCount;
}

const Carousel::Card& Carousel::focused() const {
  return kCards[index_];
}

void Carousel::next() {
  index_ = static_cast<uint8_t>((index_ + 1) % kCardCount);
  offsetPx_ = DISPLAY_WIDTH;  // new card slides in from the right
}

void Carousel::previous() {
  index_ = static_cast<uint8_t>((index_ + kCardCount - 1) % kCardCount);
  offsetPx_ = -DISPLAY_WIDTH;  // new card slides in from the left
}

void Carousel::update(uint32_t deltaMs) {
  if (offsetPx_ == 0) {
    return;
  }
  const int32_t step = static_cast<int32_t>(offsetPx_) * static_cast<int32_t>(deltaMs) /
                       static_cast<int32_t>(kEaseDivisorMs);
  int32_t nextOffset = offsetPx_ - (step != 0 ? step : (offsetPx_ > 0 ? 1 : -1));
  if ((offsetPx_ > 0 && nextOffset <= 2) || (offsetPx_ < 0 && nextOffset >= -2)) {
    nextOffset = 0;
  }
  offsetPx_ = static_cast<int16_t>(nextOffset);
}

void Carousel::renderCard(Arduino_GFX& gfx, uint8_t cardIndex, int16_t xOffset, int16_t topY) {
  const Card& c = kCards[cardIndex];
  const int16_t x = theme::kPadding + xOffset;
  const int16_t y = topY + theme::kPadding;
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  const int16_t h = DISPLAY_HEIGHT - y - 48;

  widgets::card(gfx, x, y, w, h);

  gfx.setTextSize(theme::kTextSizeTitle);
  gfx.setTextColor(theme::kText);
  const int16_t tw = static_cast<int16_t>(strlen(c.title)) * 6 * theme::kTextSizeTitle;
  gfx.setCursor(x + (w - tw) / 2, y + h / 2 - 40);
  gfx.print(c.title);

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  const int16_t hw = static_cast<int16_t>(strlen(c.hint)) * 6 * theme::kTextSizeSmall;
  gfx.setCursor(x + (w - hw) / 2, y + h / 2 + 8);
  gfx.print(c.hint);

  gfx.fillRect(x + (w - 60) / 2, y + h / 2 - 8, 60, 3, theme::kAccent);
}

void Carousel::render(Arduino_GFX& gfx, int16_t topY) {
#if defined(LITTLECUBE_HOME_LAYOUT_GRID)
  // 2x2 grid variant (compile-time): four cards per page, no slide.
  const uint8_t page = index_ / 4;
  const int16_t cw = (DISPLAY_WIDTH - 3 * theme::kPadding) / 2;
  const int16_t ch = (DISPLAY_HEIGHT - topY - 3 * theme::kPadding - 48) / 2;
  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t cardIndex = page * 4 + i;
    if (cardIndex >= kCardCount) {
      break;
    }
    const int16_t gx = theme::kPadding + (i % 2) * (cw + theme::kPadding);
    const int16_t gy = topY + theme::kPadding + (i / 2) * (ch + theme::kPadding);
    gfx.fillRoundRect(gx, gy, cw, ch, theme::kCardRadius, theme::kPanel);
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(cardIndex == index_ ? theme::kAccent : theme::kText);
    gfx.setCursor(gx + 10, gy + ch / 2 - 8);
    gfx.print(kCards[cardIndex].title);
  }
#else
  renderCard(gfx, index_, offsetPx_, topY);
  if (offsetPx_ > 0) {
    // Sliding left: the previous card exits to the left.
    const uint8_t prev = static_cast<uint8_t>((index_ + kCardCount - 1) % kCardCount);
    renderCard(gfx, prev, offsetPx_ - DISPLAY_WIDTH, topY);
  } else if (offsetPx_ < 0) {
    const uint8_t nextIdx = static_cast<uint8_t>((index_ + 1) % kCardCount);
    renderCard(gfx, nextIdx, offsetPx_ + DISPLAY_WIDTH, topY);
  }

  // Position dots.
  const int16_t dotsW = kCardCount * 12;
  int16_t dx = (DISPLAY_WIDTH - dotsW) / 2;
  const int16_t dy = DISPLAY_HEIGHT - 24;
  for (uint8_t i = 0; i < kCardCount; i++) {
    if (i == index_) {
      gfx.fillCircle(dx + 4, dy, 4, theme::kAccent);
    } else {
      gfx.fillCircle(dx + 4, dy, 2, theme::kPanelAlt);
    }
    dx += 12;
  }
#endif
}
