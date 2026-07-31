#include "ClockApp.h"

#include <Arduino_GFX_Library.h>

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
  nextFrameMs_ = clockfaces::kFaceStatic;
  lastMinute_ = -1;  // forces the first update() to treat the minute as new
  dirty_ = true;
}

clockfaces::FaceId ClockApp::face() const {
  // A missing settings service (boot degraded — everything degrades) is not an
  // error here: the default face is a perfectly good clock.
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

  // Count down toward the frame the face asked for at its last render.
  // Saturating, never wrapping: a large deltaMs (a long blocking service tick,
  // or the first frame after the screen wakes) would otherwise underflow
  // straight past zero to ~49 days and freeze an animated face solid.
  if (nextFrameMs_ != clockfaces::kFaceStatic) {
    nextFrameMs_ = (deltaMs >= nextFrameMs_) ? 0 : nextFrameMs_ - deltaMs;
    if (nextFrameMs_ == 0) {
      dirty_ = true;  // a face returning 0 lands here every tick, by design
    }
  }

  // Minute roll, checked for every face — an animated face still has to show
  // the right time. lastMinute_ is written HERE and not in render() on purpose:
  // kernelLoop() skips render() while the screen is off or a flush is pending,
  // so a render-side write would never advance and this would latch dirty_ on
  // every tick forever.
  //
  // now() && valid(), not now() alone: TimeService::now() succeeds whenever an
  // RTC is present even when it has never been set ("Even an unset RTC ticks",
  // TimeService.cpp:161). Without valid(), a never-set device would repaint the
  // whole canvas once a minute to redraw an unchanging "--:--".
  struct tm t;
  if (services_.time != nullptr && services_.time->now(t) && services_.time->valid()) {
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
  // Reload the countdown from whatever the face just asked for. update() turns
  // it into per-frame repaints mid-animation, a timed wake for a face that is
  // idle between beats, and nothing at all for a static face.
  //
  // AmoledProtection::keepAwake() is deliberately NOT called anywhere in this
  // app: an animated face that held the panel awake is precisely the burn-in
  // case spec §37 exists to prevent, and the user chose that clock faces
  // respect the screen timeout.
  nextFrameMs_ = clockfaces::render(gfx, id, ctx);

  // Face name goes left, action hint goes right — footer()'s documented policy
  // is that `right` wins the space and `left` ellipsizes, and truncating the
  // hint would be the wrong failure. Measured: hint 106px, widest name
  // ("Mood Cube") 91px, in a 328px band. Neither truncates.
  widgets::footer(gfx, clockfaces::name(id), "tap: next face", shiftX, shiftY);

  display->markDirty();
}

bool ClockApp::handleInput(const InputEvent& event) {
  if (event.action == InputAction::Tap) {
    // The only place in this app that setClockFace() may be called: it writes
    // NVS. See the class comment.
    if (services_.settings != nullptr) {
      const uint8_t next =
          static_cast<uint8_t>((services_.settings->clockFace() + 1) % clockfaces::kFaceCount);
      services_.settings->setClockFace(next);
    }
    animMs_ = 0;
    nextFrameMs_ = clockfaces::kFaceStatic;  // the new face sets its own pace
    dirty_ = true;
    return true;
  }
  return false;  // Back/Home fall through to the router
}
