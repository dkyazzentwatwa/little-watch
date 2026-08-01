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
// widgets::header() returns theme::kStatusBarHeight + 10 + cap(Title) + 10 + 12
// with shiftY folded in. cap(FreeSansBold18pt, "H") is 25, so the unshifted
// value is 85. render() lays out from the real return value; this constant only
// feeds rowsThatFit(), which needs a BUDGET (a difference), so the burn-in
// shift cancels and a pixel of drift here costs nothing.
constexpr int16_t kContentTop = 85;

// Offsets from the content top. The transport is taller mid-take, so the list
// starts lower then.
constexpr int16_t kRecordDy = 30;   // record/stop button top
constexpr int16_t kRecordH = 64;
constexpr int16_t kPauseDy = 102;   // pause/resume button top (recording only)
constexpr int16_t kPauseH = 52;
constexpr int16_t kHintDyIdle = 102;
constexpr int16_t kHintDyRec = 164;
constexpr int16_t kListDyIdle = 126;
constexpr int16_t kListDyRec = 186;

constexpr int16_t kRowHeight = 60;
// widgets::footer() puts its rule at DISPLAY_HEIGHT - 44 and its caption below
// that, so the reserved band is 44 plus a little clearance — the old 30 was
// sized for the -28 footers and would have let the last row draw over the rule.
constexpr int16_t kFooterHeight = 48;

// The delete button hangs off the SAFE area, not the raw panel edge: at
// kPadding it sat 12 px from the glass and crowded the name beside it.
constexpr int16_t kDeleteW = 44;
constexpr int16_t kDeleteH = 40;
constexpr int16_t kDeleteX = DISPLAY_WIDTH - theme::kSafeInset - kDeleteW;  // 304
constexpr int16_t kRowGap = 8;
constexpr int16_t kRowW = kDeleteX - kRowGap - theme::kPadding;  // 284
// widgets::listItem() insets its text by 10 on each side and does NOT clip.
constexpr int16_t kRowTextW = kRowW - 20;

// Red dot + gap that precedes "REC". Centring the text inside a box inset by
// the cluster width centres the WHOLE composite (see render()).
constexpr int16_t kDotR = 12;
constexpr int16_t kDotGap = 12;
constexpr int16_t kDotCluster = 2 * kDotR + kDotGap;

// Where the recordings list starts, below the transport controls.
int16_t listTop(bool recording) {
  return kContentTop + (recording ? kListDyRec : kListDyIdle);
}

// "~16274 min left on card" both overran the line and was unreadable as a
// quantity. Largest sensible unit instead.
void formatRemaining(uint32_t seconds, char* out, size_t outLen) {
  const uint32_t minutes = seconds / 60;
  if (minutes >= 48 * 60) {
    snprintf(out, outLen, "~%lu days left", (unsigned long)(minutes / (24 * 60)));
  } else if (minutes >= 90) {
    snprintf(out, outLen, "~%lu h left", (unsigned long)(minutes / 60));
  } else {
    snprintf(out, outLen, "~%lu min left", (unsigned long)minutes);
  }
}

// Row label: drop the uniform ".wav" (23 chars of REC_YYYYMMDD_HHMMSS.wav
// measure 303 px at Body against 264 px of row, and listItem does not clip),
// then ellipsize anything still too wide. Pure measurement — no I/O — so this
// is safe on the render path.
void rowLabel(Arduino_GFX& gfx, const char* name, int16_t maxW, char* out, size_t outLen) {
  size_t len = strlen(name);
  if (len > 4 && name[len - 4] == '.' && tolower(name[len - 3]) == 'w' &&
      tolower(name[len - 2]) == 'a' && tolower(name[len - 1]) == 'v') {
    len -= 4;
  }
  if (len > outLen - 1) {
    len = outLen - 1;
  }
  memcpy(out, name, len);
  out[len] = '\0';
  if (widgets::textWidth(gfx, out, widgets::TextStyle::Body) <= maxW) {
    return;
  }
  while (len > 0) {
    len--;
    out[len] = '\0';
    if (len + 4 <= outLen) {
      strcat(out, "...");
      if (widgets::textWidth(gfx, out, widgets::TextStyle::Body) <= maxW) {
        return;
      }
      out[len] = '\0';
    }
  }
}
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
  const int16_t sx = services_.amoled->shiftX();
  const int16_t sy = services_.amoled->shiftY();
  gfx.fillScreen(theme::kBg);
  statusBar_.render(gfx, state, sx, sy);

  RecorderService* rec = services_.recorder;
  const bool recording = rec != nullptr && rec->recording();

  // Hacker framing (rule + footer) around proportional body type: filenames and
  // durations are read and acted on, so they stay in the TextStyle faces rather
  // than the monospace ASCII the glance screens use.
  const int16_t contentTop = widgets::header(gfx, "Recorder", sx, sy);
  const int16_t bodyCap = widgets::ascent(gfx, widgets::TextStyle::Body);

  // Elapsed / remaining line. No middot: the Free* faces are ASCII 0x20-0x7E,
  // so the old "ready · ..." separator rendered as a stray glyph on the panel.
  char line[64];
  uint16_t lineInk;
  if (recording) {
    const uint32_t sec = rec->elapsedMs() / 1000;
    snprintf(line, sizeof(line), "%s %02lu:%02lu", rec->paused() ? "paused" : "recording",
             (unsigned long)(sec / 60), (unsigned long)(sec % 60));
    lineInk = rec->paused() ? theme::kWarn : theme::kBad;
  } else if (services_.sdCard != nullptr && services_.sdCard->writable()) {
    char remain[32];
    formatRemaining(rec != nullptr ? rec->estimatedRemainingSec() : 0, remain, sizeof(remain));
    snprintf(line, sizeof(line), "ready - %s", remain);
    lineInk = theme::kTextDim;
  } else {
    // Always the SPECIFIC state, never a generic error — and this branch is
    // also the no-card-at-all render.
    snprintf(line, sizeof(line), "SD card %s",
             services_.sdCard != nullptr ? sdCardStateName(services_.sdCard->state()) : "?");
    lineInk = theme::kWarn;
  }
  widgets::text(gfx, theme::kPadding + sx, contentTop, line, widgets::TextStyle::Body, lineInk);

  // Big record / stop control (always a visible button, never gesture-only).
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  const int16_t labelTop = contentTop + kRecordDy + (kRecordH - bodyCap) / 2;
  recordRect_ = widgets::Rect{static_cast<int16_t>(theme::kPadding + sx),
                              static_cast<int16_t>(contentTop + kRecordDy), w, kRecordH};
  if (recording) {
    gfx.fillRoundRect(recordRect_.x, recordRect_.y, recordRect_.w, recordRect_.h, 12,
                      theme::kBad);
    widgets::textCentered(gfx, recordRect_.x, labelTop, w, "STOP", widgets::TextStyle::Body,
                          theme::kBg);
    pauseRect_ = widgets::button(gfx, theme::kPadding + sx, contentTop + kPauseDy, w, kPauseH,
                                 rec->paused() ? "resume" : "pause", false);
  } else {
    gfx.fillRoundRect(recordRect_.x, recordRect_.y, recordRect_.w, recordRect_.h, 12,
                      theme::kPanel);
    // The red dot is the only colour cue separating "start" from "stop", so it
    // stays. Centring "REC" inside a box inset by the dot cluster puts the
    // composite (dot + gap + ink) dead centre — no hardcoded pixel offset.
    const int16_t labelW = widgets::textWidth(gfx, "REC", widgets::TextStyle::Body);
    const int16_t inkX = recordRect_.x + kDotCluster + (w - kDotCluster - labelW) / 2;
    gfx.fillCircle(inkX - kDotGap - kDotR, recordRect_.y + kRecordH / 2, kDotR, theme::kBad);
    widgets::textCentered(gfx, recordRect_.x + kDotCluster, labelTop, w - kDotCluster, "REC",
                          widgets::TextStyle::Body, theme::kText);
    pauseRect_ = widgets::Rect{};
  }

  // Recordings, newest first.
  widgets::text(gfx, theme::kPadding + sx,
                contentTop + (recording ? kHintDyRec : kHintDyIdle),
                totalRecordings_ > 0 ? "newest first (tap = play)" : "no recordings yet",
                widgets::TextStyle::Caption, theme::kTextDim);
  int16_t y = contentTop + (recording ? kListDyRec : kListDyIdle);
  for (size_t i = 0; i < kMaxListed; i++) {
    if (i >= recordingCount_) {
      rowRects_[i] = widgets::Rect{};
      rowDeleteRects_[i] = widgets::Rect{};
      continue;
    }
    char secondary[32];
    snprintf(secondary, sizeof(secondary), "%u KB", (unsigned)(recordings_[i].sizeBytes / 1024));
    char label[sizeof(recordings_[i].name)];
    rowLabel(gfx, recordings_[i].name, kRowTextW, label, sizeof(label));
    rowRects_[i] = widgets::listItem(gfx, theme::kPadding + sx, y, kRowW, label, secondary,
                                     false);
    rowDeleteRects_[i] = widgets::Rect{static_cast<int16_t>(kDeleteX + sx),
                                       static_cast<int16_t>(y + 6), kDeleteW, kDeleteH};
    gfx.drawRoundRect(rowDeleteRects_[i].x, rowDeleteRects_[i].y, rowDeleteRects_[i].w,
                      rowDeleteRects_[i].h, 6, theme::kBad);
    widgets::textCentered(gfx, rowDeleteRects_[i].x,
                          static_cast<int16_t>(rowDeleteRects_[i].y + (kDeleteH - bodyCap) / 2),
                          kDeleteW, "x", widgets::TextStyle::Body, theme::kBad);
    y += kRowHeight;
  }

  // Never claim the list is complete when it is not: the footer carries the
  // true total and how to reach the rest. Right wins the space and holds the
  // action hint; left is the count and ellipsizes. kTextDim, not kPanelAlt —
  // kPanelAlt is a fill colour (1.24-1.40:1 against kBg), never ink.
  char count[40];
  const char* pageHint = nullptr;
  if (totalRecordings_ > 0) {
    const size_t first = pageFirst_[page_] + 1;
    if (recordingCount_ > 0) {
      snprintf(count, sizeof(count), "%u-%u of %u", (unsigned)first,
               (unsigned)(first + recordingCount_ - 1), (unsigned)totalRecordings_);
    } else {
      snprintf(count, sizeof(count), "0 of %u", (unsigned)totalRecordings_);
    }
    if (page_ > 0 || totalRecordings_ > recordingCount_) {
      pageHint = "swipe up/down";
    }
  } else {
    snprintf(count, sizeof(count), "no recordings");
  }
  widgets::footer(gfx, count, pageHint, sx, sy);

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
