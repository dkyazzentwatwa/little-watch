#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../services/RecorderService.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

// Recorder (spec §23). The stop control is a visible button — recording
// never ends by gesture alone. Latest recordings play back with a tap;
// delete sits behind a confirm modal.
class RecorderApp : public App {
 public:
  explicit RecorderApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override {}
  void onPause() override {}
  void onResume() override { dirty_ = true; }

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  void refreshList();

  Services& services_;
  StatusBar statusBar_;
  bool dirty_ = true;
  bool confirmDelete_ = false;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;
  uint32_t tickMs_ = 0;

  static constexpr size_t kMaxListed = 4;
  RecordingInfo recordings_[kMaxListed];
  size_t recordingCount_ = 0;
  size_t deleteIndex_ = 0;

  widgets::Rect recordRect_;
  widgets::Rect pauseRect_;
  widgets::Rect rowRects_[kMaxListed];
  widgets::Rect rowDeleteRects_[kMaxListed];
  widgets::Rect confirmRect_;
  widgets::Rect cancelRect_;
};
