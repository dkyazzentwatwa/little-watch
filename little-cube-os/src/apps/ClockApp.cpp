#include "ClockApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../services/SettingsService.h"
#include "../services/TimeService.h"
#include "../ui/AmoledProtection.h"
#include "../ui/ClockFaces.h"
#include "../ui/Theme.h"
#include "../ui/widgets/Widgets.h"

// Functional-lite clock (spec §12): a selectable face plus the date. Alarms,
// timers, stopwatch and focus sessions are follow-up work; time comes from the
// RTC/NTP via TimeService, manual set via serial `time set`.

void ClockApp::onOpen() {
  animMs_ = 0;
  animating_ = false;
  lastMinute_ = -1;  // forces the first update() to treat the minute as new
  dirty_ = true;
}

clockfaces::FaceId ClockApp::face() const {
  // A missing settings service (boot degraded, spec: everything degrades) is
  // not an error here — the default face is a perfectly good clock.
  if (services_.settings == nullptr) {
    return clockfaces::FaceId::BigDigital;
  }
  const uint8_t index = services_.settings->clockFace();
  if (index >= clockfaces::kFaceCount) {
    return clockfaces::FaceId::BigDigital;
  }
  return static_cast<clockfaces::FaceId>(index);
}

void ClockApp::update(uint32_t deltaMs) {
  animMs_ += deltaMs;
  if (animating_) {
    // The face asked for another frame. AmoledProtection::keepAwake() is
    // deliberately NOT called: an animated face that held the panel awake is
    // precisely the burn-in case spec §37 exists to prevent, and the user
    // chose that clock faces respect the screen timeout.
    dirty_ = true;
    return;
  }

  // Static face: repaint only when the displayed minute actually changes.
  // SystemState::version already ticks on the clock minute and on the 60 s
  // pixel shift, so this is belt-and-braces rather than the sole trigger.
  struct tm t;
  if (services_.time != nullptr && services_.time->now(t)) {
    if (t.tm_min != lastMinute_) {
      lastMinute_ = t.tm_min;
      dirty_ = true;
    }
  } else if (lastMinute_ != -1) {
    lastMinute_ = -1;
    dirty_ = true;
  }
}

void ClockApp::render() {
  const SystemState& state = *services_.state;
  if (!dirty_ && state.version == lastStateVersion_) {
    return;
  }
  lastStateVersion_ = state.version;
  dirty_ = false;

  DisplayAdapter* display = services_.display;
  if (display == nullptr || !display->ready()) {
    return;
  }
  Arduino_GFX& gfx = *display->canvas();
  gfx.fillScreen(theme::kBg);

  // The face is the most persistent, largest-area element the device ever
  // shows, so it drifts with the same offsets as the status bar (spec §37).
  const int16_t shiftX = services_.amoled->shiftX();
  const int16_t shiftY = services_.amoled->shiftY();
  statusBar_.render(gfx, state, shiftX, shiftY);

  struct tm t;
  const bool haveTime = services_.time != nullptr && services_.time->now(t) &&
                        services_.time->valid();

  clockfaces::FaceContext ctx;
  ctx.time = haveTime ? &t : nullptr;
  ctx.state = &state;
  ctx.animMs = animMs_;
  ctx.shiftX = shiftX;
  ctx.shiftY = shiftY;

  const clockfaces::FaceId id = face();
  // Store the request: update() turns this into per-frame repaints for an
  // animated face and leaves a static one alone until the minute rolls.
  animating_ = clockfaces::render(gfx, id, ctx);

  widgets::footer(gfx, "tap: next face", clockfaces::name(id), shiftX, shiftY);

  display->markDirty();
}

bool ClockApp::handleInput(const InputEvent& event) {
  if (event.action == InputAction::Tap) {
    // The ONLY place setClockFace() may be called: it writes NVS.
    if (services_.settings != nullptr) {
      const uint8_t next =
          static_cast<uint8_t>((services_.settings->clockFace() + 1) % clockfaces::kFaceCount);
      services_.settings->setClockFace(next);
    }
    animMs_ = 0;
    dirty_ = true;
    return true;
  }
  return false;  // Back/Home fall through to the router
}
