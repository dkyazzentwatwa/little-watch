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

// "Nothing moves until the minute rolls over" — see render() below.
constexpr uint32_t kFaceStatic = 0xFFFFFFFFu;

// Draws the face. Returns how many milliseconds until it wants its next frame:
//   0            — redraw as soon as possible (mid-animation)
//   kFaceStatic  — nothing moves until the minute rolls over
//   anything else— idle for that long, then redraw
//
// A bool could not express "idle now, wake me in 3.2 s", which is precisely
// what a blinking face is: ~140 ms of motion every few seconds. Saying
// "static" between blinks would starve it of the frames the blink needs to
// start — a static face gets only the ~2 frames/minute the minute roll and the
// 60 s pixel shift produce. Saying "redraw always" would repaint the whole
// 322 KB PSRAM canvas and re-flush it over QSPI at 30 fps, ~97% of it
// redrawing an unchanged image, on the screen users leave open longest.
//
// This return value is load-bearing: ClockApp must honour it rather than
// redrawing unconditionally.
uint32_t render(Arduino_GFX& gfx, FaceId id, const FaceContext& ctx);

}  // namespace clockfaces
