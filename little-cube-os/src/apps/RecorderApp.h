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
  void onClose() override;
  void onPause() override;
  void onResume() override;

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  void refreshList();
  // Rows that actually fit between the list header and the footer. The
  // transport controls are taller mid-take, so the budget shrinks then.
  size_t rowsThatFit(bool recording) const;
  void showPage(uint8_t page);

  Services& services_;
  StatusBar statusBar_;
  bool dirty_ = true;
  bool confirmDelete_ = false;
  bool wasRecording_ = false;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;
  uint32_t tickMs_ = 0;

  static constexpr size_t kMaxListed = 4;
  // Keyset paging (see RecorderService::list): pageAnchors_[p] is the name of
  // the last row of page p-1. Without paging, every recording past the first
  // screenful is unplayable *and* undeletable on-device — delete only exists
  // on a rendered row.
  static constexpr uint8_t kMaxPages = 8;
  RecordingInfo recordings_[kMaxListed];
  size_t recordingCount_ = 0;
  size_t totalRecordings_ = 0;
  char pageAnchors_[kMaxPages][48] = {};
  // Rows per page is not constant (the transport is taller while recording),
  // so the footer's "X-Y of Z" needs the running index, not page*rows.
  size_t pageFirst_[kMaxPages] = {};
  uint8_t page_ = 0;
  size_t deleteIndex_ = 0;

  widgets::Rect recordRect_;
  widgets::Rect pauseRect_;
  widgets::Rect rowRects_[kMaxListed];
  widgets::Rect rowDeleteRects_[kMaxListed];
  widgets::Rect confirmRect_;
  widgets::Rect cancelRect_;
};
