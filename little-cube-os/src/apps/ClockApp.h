#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../ui/ClockFaces.h"
#include "../ui/StatusBar.h"

// Clock (spec §12). The app owns selection, persistence and the animation
// clock; every pixel lives in ui/ClockFaces.cpp. Tapping cycles faces and
// writes the choice to NVS — which is why setClockFace() is called ONLY from
// handleInput(). A per-frame caller at 30 fps would be ~108,000 NVS writes an
// hour and would wear the partition out inside a fortnight.
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
  // to the face renderers. animating_ is whatever the last render() reported:
  // a static face leaves it false and the screen only repaints on the minute.
  uint32_t animMs_ = 0;
  bool animating_ = false;
  int lastMinute_ = -1;
};
