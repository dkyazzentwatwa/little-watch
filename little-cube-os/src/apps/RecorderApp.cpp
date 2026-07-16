#include "RecorderApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/SdCardAdapter.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../ui/Theme.h"

namespace {
constexpr int16_t kTop = theme::kStatusBarHeight + 12;
}

void RecorderApp::onOpen() {
  refreshList();
  confirmDelete_ = false;
  dirty_ = true;
}

void RecorderApp::refreshList() {
  recordingCount_ =
      services_.recorder != nullptr ? services_.recorder->list(recordings_, kMaxListed) : 0;
}

void RecorderApp::update(uint32_t deltaMs) {
  // Elapsed-time readout while recording; 2 Hz is plenty.
  if (services_.recorder != nullptr && services_.recorder->recording()) {
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
  statusBar_.render(gfx, state, 0, 0);

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

  // Latest recordings.
  int16_t y = kTop + (recording ? 218 : 160);
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print(recordingCount_ > 0 ? "latest (tap = play)" : "no recordings yet");
  y += 28;
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
    y += 60;
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

  if (event.action == InputAction::Tap) {
    if (recordRect_.contains(event.x, event.y)) {
      if (rec->recording()) {
        rec->stop();
        refreshList();
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
            services_.audio->playWavFile(recordings_[i].path);
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
