#include "RecorderApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/SdCardAdapter.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"

namespace {
constexpr int16_t kTop = theme::kStatusBarHeight + 12;
constexpr int16_t kRowHeight = 60;
constexpr int16_t kFooterHeight = 30;

// Where the recordings list starts, below the transport controls.
int16_t listTop(bool recording) { return kTop + (recording ? 218 : 160) + 28; }
}  // namespace

void RecorderApp::onOpen() {
  page_ = 0;
  refreshList();
  confirmDelete_ = false;
  dirty_ = true;
}

// Leaving this app — paused or closed — deliberately does NOT stop an
// in-progress recording. RecorderService owns the session and the status bar
// reports it from every app, so glancing at the clock mid-interview must not
// silently truncate the audio. Only the UI's own transient state is dropped.
void RecorderApp::onPause() {
  confirmDelete_ = false;
}

void RecorderApp::onClose() {
  confirmDelete_ = false;
}

void RecorderApp::onResume() {
  // `rec delete` over serial or a card swap can change the list underneath.
  refreshList();
  dirty_ = true;
}

size_t RecorderApp::rowsThatFit(bool recording) const {
  const int16_t budget = DISPLAY_HEIGHT - kFooterHeight - listTop(recording);
  const int16_t rows = budget / kRowHeight;
  if (rows <= 0) {
    return 0;
  }
  return static_cast<size_t>(rows) > kMaxListed ? kMaxListed : static_cast<size_t>(rows);
}

void RecorderApp::refreshList() {
  recordingCount_ = 0;
  totalRecordings_ = 0;
  RecorderService* rec = services_.recorder;
  if (rec == nullptr) {
    return;
  }
  const size_t rows = rowsThatFit(rec->recording());
  if (rows == 0) {
    return;
  }
  recordingCount_ =
      rec->list(recordings_, rows, &totalRecordings_, page_ > 0 ? pageAnchors_[page_] : nullptr);
  // The page can empty out underneath us — a serial `rec delete`, a card
  // swap, or simply fewer rows fitting now that recording has started.
  if (recordingCount_ == 0 && page_ > 0) {
    page_ = 0;
    recordingCount_ = rec->list(recordings_, rows, &totalRecordings_, nullptr);
  }
  if (page_ == 0) {
    pageFirst_[0] = 0;
  }
}

void RecorderApp::showPage(uint8_t page) {
  page_ = page;
  refreshList();
  confirmDelete_ = false;  // the armed row is not on screen any more
  dirty_ = true;
}

void RecorderApp::update(uint32_t deltaMs) {
  const bool rec = services_.recorder != nullptr && services_.recorder->recording();

  // Stopping is asynchronous, so the new .wav only exists once the recorder
  // has gone idle. Re-list on that edge rather than at the STOP tap — and go
  // back to page 0, which is where the newest recording now sorts.
  if (wasRecording_ && !rec) {
    page_ = 0;
    refreshList();
    dirty_ = true;
  } else if (!wasRecording_ && rec) {
    // Do not touch SD while the analog mic is live. Keep the cached list and
    // clamp it to the smaller recording layout; refresh after the take ends.
    const size_t visibleRows = rowsThatFit(true);
    if (recordingCount_ > visibleRows) {
      recordingCount_ = visibleRows;
    }
    dirty_ = true;
  }
  wasRecording_ = rec;

  // Elapsed-time readout while recording; 2 Hz is plenty.
  if (rec) {
    tickMs_ += deltaMs;
    if (tickMs_ >= 500) {
      tickMs_ = 0;
      dirty_ = true;
    }
  }
}

void RecorderApp::render() {
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
  statusBar_.render(gfx, state, services_.amoled->shiftX(),
                    services_.amoled->shiftY());

  RecorderService* rec = services_.recorder;
  const bool recording = rec != nullptr && rec->recording();

  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Recorder");

  // Elapsed / remaining line.
  char line[64];
  gfx.setTextSize(theme::kTextSizeSmall);
  if (recording) {
    const uint32_t sec = rec->elapsedMs() / 1000;
    snprintf(line, sizeof(line), "%s %02lu:%02lu", rec->paused() ? "paused" : "recording",
             (unsigned long)(sec / 60), (unsigned long)(sec % 60));
    gfx.setTextColor(rec->paused() ? theme::kWarn : theme::kBad);
  } else if (services_.sdCard != nullptr && services_.sdCard->writable()) {
    const uint32_t remain = rec != nullptr ? rec->estimatedRemainingSec() : 0;
    snprintf(line, sizeof(line), "ready · ~%lu min left on card",
             (unsigned long)(remain / 60));
    gfx.setTextColor(theme::kTextDim);
  } else {
    snprintf(line, sizeof(line), "SD card %s",
             services_.sdCard != nullptr ? sdCardStateName(services_.sdCard->state()) : "?");
    gfx.setTextColor(theme::kWarn);
  }
  gfx.setCursor(theme::kPadding, kTop + 36);
  gfx.print(line);

  // Big record / stop control (always a visible button, never gesture-only).
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  recordRect_ = widgets::Rect{theme::kPadding, kTop + 66, w, 72};
  if (recording) {
    gfx.fillRoundRect(recordRect_.x, recordRect_.y, recordRect_.w, recordRect_.h, 12,
                      theme::kBad);
    gfx.setTextSize(theme::kTextSizeBody);
    gfx.setTextColor(theme::kBg);
    gfx.setCursor(recordRect_.x + w / 2 - 36, recordRect_.y + 26);
    gfx.print("STOP");
    pauseRect_ = widgets::button(gfx, theme::kPadding, kTop + 150, w, 52,
                                 rec->paused() ? "resume" : "pause", false);
  } else {
    gfx.fillRoundRect(recordRect_.x, recordRect_.y, recordRect_.w, recordRect_.h, 12,
                      theme::kPanel);
    gfx.fillCircle(recordRect_.x + w / 2 - 52, recordRect_.y + 36, 12, theme::kBad);
    gfx.setTextSize(theme::kTextSizeBody);
    gfx.setTextColor(theme::kText);
    gfx.setCursor(recordRect_.x + w / 2 - 28, recordRect_.y + 26);
    gfx.print("REC");
    pauseRect_ = widgets::Rect{};
  }

  // Recordings, newest first.
  int16_t y = kTop + (recording ? 218 : 160);
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print(totalRecordings_ > 0 ? "newest first (tap = play)" : "no recordings yet");
  y = listTop(recording);
  for (size_t i = 0; i < kMaxListed; i++) {
    if (i >= recordingCount_) {
      rowRects_[i] = widgets::Rect{};
      rowDeleteRects_[i] = widgets::Rect{};
      continue;
    }
    char secondary[32];
    snprintf(secondary, sizeof(secondary), "%u KB", (unsigned)(recordings_[i].sizeBytes / 1024));
    rowRects_[i] = widgets::listItem(gfx, theme::kPadding, y, w - 56, recordings_[i].name,
                                     secondary, false);
    rowDeleteRects_[i] = widgets::Rect{static_cast<int16_t>(DISPLAY_WIDTH - theme::kPadding - 44),
                                       static_cast<int16_t>(y + 6), 44, 40};
    gfx.drawRoundRect(rowDeleteRects_[i].x, rowDeleteRects_[i].y, rowDeleteRects_[i].w,
                      rowDeleteRects_[i].h, 6, theme::kBad);
    gfx.setTextColor(theme::kBad);
    gfx.setCursor(rowDeleteRects_[i].x + 12, rowDeleteRects_[i].y + 12);
    gfx.print("x");
    y += kRowHeight;
  }

  // Never claim the list is complete when it is not: the footer carries the
  // true total and how to reach the rest.
  if (totalRecordings_ > 0) {
    const size_t first = pageFirst_[page_] + 1;
    char footer[48];
    if (recordingCount_ > 0 && totalRecordings_ > recordingCount_) {
      snprintf(footer, sizeof(footer), "%u-%u of %u · swipe up/down", (unsigned)first,
               (unsigned)(first + recordingCount_ - 1), (unsigned)totalRecordings_);
    } else {
      snprintf(footer, sizeof(footer), "%u recording%s", (unsigned)totalRecordings_,
               totalRecordings_ == 1 ? "" : "s");
    }
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kTextDim);
    gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 24);
    gfx.print(footer);
  }

  if (confirmDelete_ && deleteIndex_ < recordingCount_) {
    confirmRect_ =
        widgets::modalConfirm(gfx, "Delete recording?", recordings_[deleteIndex_].name,
                              cancelRect_);
  }

  display->markDirty();
}

bool RecorderApp::handleInput(const InputEvent& event) {
  RecorderService* rec = services_.recorder;
  if (rec == nullptr) {
    return false;
  }

  if (confirmDelete_) {
    if (event.action == InputAction::Tap) {
      if (confirmRect_.contains(event.x, event.y)) {
        rec->remove(recordings_[deleteIndex_].path);
        refreshList();
      }
      confirmDelete_ = false;
      dirty_ = true;
      return true;
    }
    if (event.action == InputAction::Back || event.action == InputAction::Cancel) {
      confirmDelete_ = false;
      dirty_ = true;
      return true;
    }
    return true;
  }

  // Paging: the whole point is that a recording past the first screenful can
  // still be played and deleted.
  if (event.action == InputAction::SwipeUp) {
    if (recordingCount_ > 0 && pageFirst_[page_] + recordingCount_ < totalRecordings_ &&
        page_ + 1 < kMaxPages) {
      const uint8_t next = page_ + 1;
      strncpy(pageAnchors_[next], recordings_[recordingCount_ - 1].name,
              sizeof(pageAnchors_[next]) - 1);
      pageAnchors_[next][sizeof(pageAnchors_[next]) - 1] = '\0';
      pageFirst_[next] = pageFirst_[page_] + recordingCount_;
      showPage(next);
    }
    return true;
  }
  if (event.action == InputAction::SwipeDown) {
    if (page_ > 0) {
      showPage(static_cast<uint8_t>(page_ - 1));
    }
    return true;
  }

  if (event.action == InputAction::Tap) {
    if (recordRect_.contains(event.x, event.y)) {
      if (rec->recording()) {
        // Asynchronous: the file is not renamed yet, so the list is refreshed
        // by update() on the recording -> idle edge instead of here.
        rec->stop();
      } else {
        rec->start();
      }
      dirty_ = true;
      return true;
    }
    if (rec->recording() && pauseRect_.contains(event.x, event.y)) {
      rec->setPaused(!rec->paused());
      dirty_ = true;
      return true;
    }
    for (size_t i = 0; i < recordingCount_; i++) {
      if (rowDeleteRects_[i].contains(event.x, event.y)) {
        deleteIndex_ = i;
        confirmDelete_ = true;
        dirty_ = true;
        return true;
      }
      if (rowRects_[i].contains(event.x, event.y)) {
        if (!rec->recording() && services_.audio != nullptr) {
          if (services_.audio->isPlaying()) {
            services_.audio->stopPlayback();
          } else {
            services_.audio->requestPlayWavFile(recordings_[i].path);
          }
        }
        dirty_ = true;
        return true;
      }
    }
    return true;
  }

  // While recording, Back/Home must not silently abandon the session.
  if ((event.action == InputAction::Back || event.action == InputAction::Home) &&
      rec->recording()) {
    Serial.println("[recorder] stop recording first (STOP button)");
    return true;
  }
  return false;
}
