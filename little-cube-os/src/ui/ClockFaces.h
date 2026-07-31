#pragma once

#include <Arduino.h>

// Clock faces. The enum and count live in ui/ rather than apps/ so
// SettingsService can persist a face index without depending on an app
// header — exactly how ui/Theme.h's kThemeCount is shared today. The
// renderers land here in Task 5.
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

}  // namespace clockfaces
