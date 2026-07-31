#pragma once

#include <Arduino.h>
#include <time.h>

class Arduino_GFX;
struct SystemState;

// Clock faces. The enum and count live in ui/ rather than apps/ so
// SettingsService can persist a face index without depending on an app
// header — exactly how ui/Theme.h's kThemeCount is shared today. The
// renderers live here too, so ClockApp owns only selection, persistence
// and the animation clock; every pixel is drawn below.
namespace clockfaces {

enum class FaceId : uint8_t {
  BigDigital,  // default
  Stacked,
  Word,
  Blinky,
  BigEyes,
  MoodCube,
};
constexpr uint8_t kFaceCount = 6;

// Display label, e.g. for the footer and `settings get clockface`.
const char* name(FaceId id);

struct FaceContext {
  const struct tm* time = nullptr;      // nullptr when the clock is not set
  const SystemState* state = nullptr;   // battery/charging, for MoodCube
  uint32_t animMs = 0;                  // monotonic ms since the app opened
  int16_t shiftX = 0;
  int16_t shiftY = 0;
};

// Draws the face. Returns true when an animation is in flight and the face
// wants another frame soon; false when it is static until the minute rolls.
//
// This return value is load-bearing: it is what keeps an animated face from
// pinning the frame loop and the panel at full tilt. ClockApp must honour it
// rather than redrawing unconditionally.
bool render(Arduino_GFX& gfx, FaceId id, const FaceContext& ctx);

}  // namespace clockfaces
