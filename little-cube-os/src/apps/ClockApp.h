#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../ui/ClockFaces.h"
#include "../ui/StatusBar.h"

// Clock (spec §12). The app owns selection, persistence and the animation
// clock; every pixel lives in ui/ClockFaces.cpp. Tapping cycles faces and
// writes the choice to NVS.
//
// Within this app, setClockFace() is called ONLY from handleInput() — never
// from update() or render(). (SettingsCommands.cpp calls it too, legitimately,
// once per `settings set clockface`.) A per-frame caller at 30 fps would be
// ~108,000 NVS writes an hour and would wear the partition out inside a
// fortnight, with no symptom until the flash dies.
class ClockApp : public App {
 public:
  explicit ClockApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override {}
  void onPause() override {}
  void onResume() override { dirty_ = true; }

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  clockfaces::FaceId face() const;

  Services& services_;
  StatusBar statusBar_;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  // Monotonic ms since the app opened (or since the face last changed), handed
  // to the face renderers.
  uint32_t animMs_ = 0;
  // Countdown, in ms, to the frame the last render()'s face asked for.
  // clockfaces::kFaceStatic means "do not count down at all" — that face only
  // repaints when the minute rolls.
  uint32_t nextFrameMs_ = clockfaces::kFaceStatic;
  int lastMinute_ = -1;
};
