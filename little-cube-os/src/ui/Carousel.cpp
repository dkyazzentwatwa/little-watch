#include "Carousel.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/Services.h"
#include "../core/SystemState.h"
#include "../feature_flags.h"
#include "../hardware/SdCardAdapter.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../services/AssistantService.h"
#include "../services/NewsService.h"
#include "../services/SettingsService.h"
#include "../services/WeatherService.h"
#include "Icons.h"
#include "Theme.h"
#include "widgets/Widgets.h"

namespace {

constexpr Carousel::Card kCards[] = {
    {AppId::Today, "Today", "at a glance", icons::IconId::Today, false},
    {AppId::Clock, "Clock", "time & alarms", icons::IconId::Clock, false},
    {AppId::Weather, "Weather", "forecast", icons::IconId::Weather, false},
    {AppId::News, "News", "headlines", icons::IconId::News, false},
    {AppId::Notes, "Notes", "read & capture", icons::IconId::Notes, false},
    {AppId::Reader, "Reader", "read books", icons::IconId::Reader, false},
    {AppId::Recorder, "Recorder", "voice notes", icons::IconId::Recorder, false},
    {AppId::Assistant, "Assistant", "voice AI", icons::IconId::Assistant, false},
    {AppId::Audio, "Audio", "sound", icons::IconId::Audio, false},
#if FEATURE_VIDEO
    {AppId::Video, "Video", "tv & anime", icons::IconId::Video, false},
#endif
    {AppId::Calendar, "Calendar", "agenda", icons::IconId::Calendar, false},
    {AppId::Settings, "Settings", "device", icons::IconId::Settings, false},
    {AppId::Files, "Tools", "files & more", icons::IconId::Tools, true},
};
constexpr uint8_t kCardCount = sizeof(kCards) / sizeof(kCards[0]);

// Slide easing: proportional decay, ~120 ms to settle.
constexpr uint32_t kEaseDivisorMs = 120;

// One live fact per card, replacing the static hint when the answer is known.
//
// EVERY branch must be O(1) against already-cached state: this runs for the
// focused card (and its neighbour mid-slide) on every rendered frame. No SD
// walk, no network, no I2C. RecorderService::list() is deliberately absent for
// exactly that reason — counting takes means walking the card's directory.
//
// Returns nullptr when nothing live is known, and the card falls back to hint.
const char* glanceFor(AppId id, Services& services, char* buf, size_t cap) {
  const SystemState* state = services.state;
  switch (id) {
    case AppId::Clock:
      if (state != nullptr && state->timeValid) {
        snprintf(buf, cap, "%s now", state->clockHhMm);
        return buf;
      }
      return "clock not set";

    case AppId::Today:
      if (state != nullptr && state->batteryPresent && state->batteryPercent >= 0) {
        snprintf(buf, cap, "battery %d%%%s", state->batteryPercent,
                 state->charging ? " · charging" : "");
        return buf;
      }
      return nullptr;

    case AppId::Weather: {
      if (services.weather == nullptr) {
        return nullptr;
      }
      const WeatherSnapshot& w = services.weather->snapshot();
      if (!w.valid) {
        return state != nullptr && state->internet ? "fetching..." : "offline";
      }
      snprintf(buf, cap, "%d°C · %s", roundC(w.temperatureC), w.condition);
      return buf;
    }

    case AppId::News:
      if (services.news == nullptr || services.news->count() == 0) {
        return nullptr;
      }
      snprintf(buf, cap, "%u headlines", static_cast<unsigned>(services.news->count()));
      return buf;

    case AppId::Assistant:
      if (services.settings != nullptr && !services.settings->hasOpenaiKey()) {
        return "needs an API key";
      }
      if (state != nullptr && !state->internet) {
        return "offline";
      }
      if (services.assistant != nullptr && services.assistant->busy()) {
        return services.assistant->stateName();
      }
      return "ready — tap to talk";

    case AppId::Audio:
      if (services.audio != nullptr && services.audio->isPlaying()) {
        const char* path = services.audio->playingPath();
        const char* name = strrchr(path, '/');
        // Bounded precision: a full path is far wider than the card anyway,
        // and an unbounded %s here is a truncation warning under -Wall.
        snprintf(buf, cap, "playing %.28s", name != nullptr ? name + 1 : path);
        return buf;
      }
      return nullptr;

    case AppId::Files:
      if (services.sdCard != nullptr && services.sdCard->mounted()) {
        const uint64_t freeMb = services.sdCard->freeBytes() / (1024 * 1024);
        if (freeMb >= 1024) {
          snprintf(buf, cap, "%u.%u GB free", static_cast<unsigned>(freeMb / 1024),
                   static_cast<unsigned>((freeMb % 1024) * 10 / 1024));
        } else {
          snprintf(buf, cap, "%u MB free", static_cast<unsigned>(freeMb));
        }
        return buf;
      }
      return "no SD card";

    default:
      return nullptr;
  }
}

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
    // animating() goes false in this same tick, so without this flag the last
    // frame ever drawn is the one a few px short and the cards visibly rest
    // off-centre.
    settled_ = true;
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

  // Icon sits in a tinted well so it reads as an object rather than as loose
  // strokes floating on the panel.
  constexpr int16_t kIconSize = 84;
  const int16_t wellSize = kIconSize + 28;
  const int16_t wellX = x + (w - wellSize) / 2;
  const int16_t wellY = y + h / 2 - 128;
  gfx.fillRoundRect(wellX, wellY, wellSize, wellSize, theme::kCardRadius + 4,
                    theme::kPanelAlt);
  icons::draw(gfx, c.icon, wellX + 14, wellY + 14, kIconSize, theme::kAccent,
              theme::kPanelAlt);

  widgets::textCentered(gfx, x, wellY + wellSize + 26, w, c.title, widgets::TextStyle::Title,
                        theme::kText);

  char buf[48];
  const char* line = c.hint;
  if (services_ != nullptr) {
    const char* live = glanceFor(c.id, *services_, buf, sizeof(buf));
    if (live != nullptr) {
      line = live;
    }
  }
  widgets::textCentered(gfx, x, wellY + wellSize + 74, w, line, widgets::TextStyle::Caption,
                        theme::kTextDim);
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

  // Progress pill: twelve dots at this width are a smear, so the position
  // reads as a filled fraction of a single track instead.
  const int16_t trackW = DISPLAY_WIDTH - 2 * theme::kSafeInset;
  const int16_t trackX = theme::kSafeInset;
  const int16_t trackY = DISPLAY_HEIGHT - 26;
  const int16_t segW = trackW / kCardCount;
  gfx.fillRoundRect(trackX, trackY, trackW, 4, 2, theme::kPanelAlt);
  gfx.fillRoundRect(trackX + segW * index_, trackY, segW, 4, 2, theme::kAccent);
#endif
}
