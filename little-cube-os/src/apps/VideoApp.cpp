#include "VideoApp.h"

#if FEATURE_VIDEO

#include <Arduino_GFX_Library.h>

#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/SdCardState.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../services/SettingsService.h"
#include "../storage/StoragePaths.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"
#include "../video/VideoPlayer.h"

void VideoApp::onOpen() {
  screen_ = Screen::Library;
  strncpy(dir_, paths::kVideo, sizeof(dir_) - 1);
  dir_[sizeof(dir_) - 1] = '\0';
  page_ = 0;
  pageAnchors_[0][0] = '\0';
  refreshList();
  syncChromeBuffer();
  dirty_ = true;

  adoptExternalPlayback();
}

// The Rotated path needs a 448x58 scratch canvas to draw landscape text into
// before transposing it onto the vertical strip; Upright draws straight into
// the frame canvas and needs nothing. Allocating ~52 KB is real work, so it
// happens here — on open, on resume and at playback start — and NEVER in
// render(), which may draw and nothing else.
void VideoApp::syncChromeBuffer() {
  const bool wantsBuffer = services_.settings == nullptr ||
                           services_.settings->videoOrientation() == VideoOrientation::Rotated;
  if (wantsBuffer && chrome_ == nullptr) {
    chrome_ = new Arduino_Canvas(kChromeW, kChromeH, nullptr);
    if (chrome_ != nullptr && !chrome_->begin(GFX_SKIP_OUTPUT_BEGIN)) {
      delete chrome_;
      chrome_ = nullptr;  // out of memory; renderChrome() degrades to no chrome
    }
  } else if (!wantsBuffer && chrome_ != nullptr) {
    delete chrome_;
    chrome_ = nullptr;
  }
}

bool VideoApp::uprightMode() const {
  const VideoPlayer* player = services_.videoPlayer;
  return player != nullptr && player->activeOrientation() == VideoOrientation::Upright;
}

void VideoApp::stepVolume(bool up) {
  const uint8_t cur = services_.audio->volumePercent();
  services_.audio->setVolumePercent(up ? (cur >= 90 ? 100 : cur + 10)
                                       : (cur <= 10 ? 0 : cur - 10));
  volShownMs_ = kVolShowMs;
  chromeDirty_ = true;
}

// Rotated asks the user to turn the device CCW, so an in-hand horizontal
// swipe arrives as portrait Up/Down and an in-hand vertical one as Left/Right.
// Upright is worn and never turned, so the panel axes are the user's axes.
VideoApp::Gesture VideoApp::gestureFor(InputAction action) const {
  const bool upright = uprightMode();
  switch (action) {
    case InputAction::SwipeUp:
      return upright ? Gesture::VolumeUp : Gesture::SeekBack;
    case InputAction::SwipeDown:
      return upright ? Gesture::VolumeDown : Gesture::SeekForward;
    case InputAction::SwipeLeft:
      return upright ? Gesture::SeekBack : Gesture::VolumeDown;
    case InputAction::SwipeRight:
      return upright ? Gesture::SeekForward : Gesture::VolumeUp;
    default:
      return Gesture::None;
  }
}

void VideoApp::onClose() {
  stopAndSavePosition();
  delete chrome_;  // heavy buffer; the next onOpen re-allocates
  chrome_ = nullptr;
}

void VideoApp::onPause() { stopAndSavePosition(); }

void VideoApp::onResume() {
  screen_ = Screen::Library;
  refreshList();
  // The user may have just come back from the Settings screen that owns the
  // orientation — this is the only moment it can change while this app lives,
  // because onPause() stops playback whenever the app is backgrounded.
  syncChromeBuffer();
  dirty_ = true;
}

void VideoApp::stopAndSavePosition() {
  nextQueued_ = false;  // an exit abandons any queued prev/next switch
  VideoPlayer* player = services_.videoPlayer;
  if (player != nullptr && player->playing()) {
    services_.video->savePosition(player->path(), player->positionMs(),
                                  player->durationMs());
    player->setUiActive(false);
    player->requestStop();
  }
}

// A playback this app did not start (the serial family's `video play`) is
// adopted by landing directly on the player screen. Called from onOpen and
// from update()'s Library idle path — the latter covers `video play` while
// this app is already foreground, where router->open() no-ops.
//
// The gate is stateless — player state, not an app-side flag — so it
// survives this app being backgrounded (onPause stops playback but doesn't
// tick update(), which is exactly how an app-side "expect idle" flag used
// to go stale and permanently block later adoption). playing() && !stopping()
// is the live condition on every call, always fresh: Playing means adopt,
// Stopping means a dying playback that must never flash onto the screen.
void VideoApp::adoptExternalPlayback() {
  VideoPlayer* player = services_.videoPlayer;
  if (player == nullptr || !player->playing() || player->stopping()) {
    return;  // nothing to adopt, or the playback is already condemned
  }
  // pendingPath_ must track the live file so prev/next and auto-advance
  // work from here.
  strncpy(pendingPath_, player->path(), sizeof(pendingPath_) - 1);
  pendingPath_[sizeof(pendingPath_) - 1] = '\0';
  player->setUiActive(true);
  syncChromeBuffer();  // the adopted playback picked its own orientation
  screen_ = Screen::Player;
  chromeVisible_ = true;
  chromeDirty_ = true;
  chromeMs_ = 0;
  saveMs_ = 0;
  wasPlaying_ = true;
  // The decoder only writes the fitted picture rect; blank the margins once.
  services_.display->canvas()->fillScreen(RGB565_BLACK);
  services_.display->markDirty();
}

void VideoApp::refreshList() {
  const char* after = page_ > 0 ? pageAnchors_[page_] : nullptr;
  itemCount_ = services_.video->list(dir_, items_, kMaxListed, &totalItems_, after);
  dirty_ = true;
}

void VideoApp::showPage(uint8_t page) {
  if (page >= kMaxPages) {
    return;
  }
  if (page > page_ && itemCount_ > 0) {
    // Anchor = sort key of the last visible row (dirs prefix '0', files '1').
    const VideoInfo& last = items_[itemCount_ - 1];
    pageAnchors_[page][0] = last.isDir ? '0' : '1';
    strncpy(pageAnchors_[page] + 1, last.name, sizeof(pageAnchors_[page]) - 2);
    pageAnchors_[page][sizeof(pageAnchors_[page]) - 1] = '\0';
  }
  page_ = page;
  refreshList();
}

void VideoApp::formatMs(uint32_t ms, char* out, size_t len) const {
  const uint32_t s = ms / 1000;
  if (s >= 3600) {
    snprintf(out, len, "%lu:%02lu:%02lu", static_cast<unsigned long>(s / 3600),
             static_cast<unsigned long>((s / 60) % 60), static_cast<unsigned long>(s % 60));
  } else {
    snprintf(out, len, "%lu:%02lu", static_cast<unsigned long>(s / 60),
             static_cast<unsigned long>(s % 60));
  }
}

void VideoApp::openItem(size_t index) {
  if (index >= itemCount_) {
    return;
  }
  const VideoInfo& item = items_[index];
  if (item.isDir) {
    strncpy(dir_, item.path, sizeof(dir_) - 1);
    dir_[sizeof(dir_) - 1] = '\0';
    page_ = 0;
    refreshList();
    return;
  }
  strncpy(pendingPath_, item.path, sizeof(pendingPath_) - 1);
  pendingPath_[sizeof(pendingPath_) - 1] = '\0';
  pendingResumeMs_ = services_.video->resumeMs(pendingPath_);
  const SystemState* st = services_.state;
  const bool lowBattery = st != nullptr && st->batteryPresent && st->batteryPercent >= 0 &&
                          st->batteryPercent < 10;
  batteryWarned_ = false;
  if (lowBattery || pendingResumeMs_ > 0) {
    batteryWarned_ = lowBattery;
    screen_ = Screen::ConfirmStart;
    dirty_ = true;
  } else {
    beginPlayback(0);
  }
}

void VideoApp::beginPlayback(uint32_t startMs) {
  nextQueued_ = false;
  char why[48] = "player unavailable";
  VideoPlayer* player = services_.videoPlayer;
  if (player == nullptr || !player->play(pendingPath_, startMs, why, sizeof(why))) {
    // A one-shot widgets::toast() draws once and is gone the next frame the
    // library redraws for any other reason — unreadable in practice. Hold
    // the reason in state instead and let renderLibrary() draw it every
    // frame until it expires (FilesApp's toast_/toastMs_ pattern).
    strncpy(toast_, why, sizeof(toast_) - 1);
    toast_[sizeof(toast_) - 1] = '\0';
    toastMs_ = kToastMs;
    screen_ = Screen::Library;
    dirty_ = true;
    return;
  }
  player->setUiActive(true);
  // play() has just resolved the picture layout from the setting, so this is
  // the moment the chrome buffer's need is settled for this playback.
  syncChromeBuffer();
  screen_ = Screen::Player;
  chromeVisible_ = true;
  chromeDirty_ = true;
  chromeMs_ = 0;
  saveMs_ = 0;
  wasPlaying_ = true;
  // Full black once; the decoder owns the image area from here on. It only
  // ever writes the fitted, centered picture rect, so this is the only place
  // the letterbox margins get blanked — never per frame.
  services_.display->canvas()->fillScreen(RGB565_BLACK);
  services_.display->markDirty();
}

void VideoApp::update(uint32_t deltaMs) {
  if (toastMs_ > 0) {
    toastMs_ = deltaMs >= toastMs_ ? 0 : toastMs_ - deltaMs;
    if (toastMs_ == 0) {
      toast_[0] = '\0';
      dirty_ = true;
    }
  }
  VideoPlayer* player = services_.videoPlayer;
  if (screen_ != Screen::Player) {
    // Covers Library AND ConfirmStart: a serial `video play` landing while
    // this app is sitting on the resume/battery modal for a different file
    // adopts that new playback. pendingPath_/pendingResumeMs_ get overwritten
    // by adoptExternalPlayback() — the modal the user was looking at is just
    // gone, same as if they had answered it.
    adoptExternalPlayback();  // serial play while we were already open
  }
  if (screen_ != Screen::Player || player == nullptr) {
    return;
  }
  // Periodic position save — this is what makes resume survive battery death.
  if (player->playing() && !player->paused()) {
    saveMs_ += deltaMs;
    if (saveMs_ >= kSaveEveryMs) {
      saveMs_ = 0;
      services_.video->savePosition(player->path(), player->positionMs(),
                                    player->durationMs());
    }
    // spec §37 exemption: full-motion video is burn-in-safe, so hold the
    // panel awake for as long as it is actually moving. A paused frozen
    // frame is static content and must NOT do this — it dims/blanks like
    // anything else.
    services_.amoled->keepAwake();
  }
  // Chrome auto-hide: static chrome exposure is bounded to ~4 s in every
  // state, playing or paused — a paused player's chrome comes back with a
  // tap like everything else. It used to stay pinned open while paused,
  // which is exactly the unbounded static content spec §37 forbids; the
  // paused frozen frame underneath it is protected the ordinary way, by
  // dim/blank, since keepAwake() is only called while actively playing.
  if (chromeVisible_) {
    chromeMs_ += deltaMs;
    if (chromeMs_ >= kChromeHideMs) {
      chromeVisible_ = false;
      chromeDirty_ = true;
    }
  }
  if (volShownMs_ > 0) {
    volShownMs_ = deltaMs >= volShownMs_ ? 0 : volShownMs_ - deltaMs;
    if (volShownMs_ == 0) {
      chromeDirty_ = true;
    }
  }
  // Redraw the timeline once per second while visible.
  if (chromeVisible_ && player->playing()) {
    const uint32_t s = player->positionMs() / 1000;
    if (s != posShownS_) {
      posShownS_ = s;
      chromeDirty_ = true;
    }
  }
  // Natural end -> auto-advance; user stop -> library. (Queue rule: only a
  // completed episode advances, mirroring lastPlayCompleted() semantics.)
  const bool playingNow = player->playing();
  if (playingNow) {
    lastKnownPosMs_ = player->positionMs();
  }
  if (wasPlaying_ && !playingNow) {
    if (nextQueued_) {
      // Task 10's prev/next chrome buttons stop the current file and set
      // this flag; this is the handshake that starts the queued sibling
      // once the stop has actually landed (state_ == Idle).
      nextQueued_ = false;
      beginPlayback(0);
      wasPlaying_ = player->playing();
      return;
    }
    if (player->completed()) {
      services_.video->clearPosition(pendingPath_);
      char next[160];
      if (services_.video->nextInFolder(pendingPath_, next, sizeof(next))) {
        strncpy(pendingPath_, next, sizeof(pendingPath_) - 1);
        pendingPath_[sizeof(pendingPath_) - 1] = '\0';
        beginPlayback(0);
        wasPlaying_ = player->playing();
        return;
      }
    } else {
      // A stop that didn't come through this app's own stopAndSavePosition()
      // (serial `video stop`, a card yank, kMaxConsecutiveBad) — nothing else
      // saved on the way out. player->positionMs() already reads 0 here
      // (state_ is Idle by this tick), so save the last position observed
      // while still playing instead.
      services_.video->savePosition(pendingPath_, lastKnownPosMs_, player->durationMs());
      if (player->lastError()[0] != '\0') {
        // Otherwise a mid-stream failure (card yank, decode desync) lands
        // back on the library with no visible reason at all.
        strncpy(toast_, player->lastError(), sizeof(toast_) - 1);
        toast_[sizeof(toast_) - 1] = '\0';
        toastMs_ = kToastMs;
      }
    }
    screen_ = Screen::Library;
    refreshList();
    dirty_ = true;
  }
  wasPlaying_ = playingNow;
}

void VideoApp::render() {
  Arduino_GFX* gfx = services_.display->canvas();
  if (gfx == nullptr) {
    return;
  }
  const SystemState* st = services_.state;
  const bool stateChanged = st != nullptr && st->version != lastStateVersion_;
  if (st != nullptr) {
    lastStateVersion_ = st->version;
  }
  switch (screen_) {
    case Screen::Library:
      if (dirty_ || stateChanged) {
        renderLibrary(*gfx);
        services_.display->markDirty();
        dirty_ = false;
      }
      return;
    case Screen::ConfirmStart:
      if (dirty_ || stateChanged) {
        renderConfirm(*gfx);
        services_.display->markDirty();
        dirty_ = false;
      }
      return;
    case Screen::Player:
      renderPlayer(*gfx);  // frame pixels come from VideoPlayer directly
      return;
  }
}

void VideoApp::renderLibrary(Arduino_GFX& gfx) {
  gfx.fillScreen(theme::kBg);
  statusBar_.render(gfx, *services_.state, services_.amoled->shiftX(),
                    services_.amoled->shiftY());
  const bool atRoot = strcmp(dir_, paths::kVideo) == 0;
  const char* slash = strrchr(dir_, '/');
  int16_t y = widgets::header(gfx, atRoot ? "Video" : (slash ? slash + 1 : dir_),
                              services_.amoled->shiftX(), services_.amoled->shiftY());
  if (itemCount_ == 0) {
    // Mirrors ReaderApp/NotesApp: an empty list can mean "no episodes" or
    // "no readable card", and those are not the same message.
    const SdCardState sd = services_.state->sd;
    const bool readable = sd == SdCardState::Mounted || sd == SdCardState::ReadOnly ||
                          sd == SdCardState::Full;
    if (readable) {
      widgets::textBlock(gfx, theme::kPadding, y + theme::kPadding,
                         DISPLAY_WIDTH - 2 * theme::kPadding,
                         "No episodes.\n\nPack one on the Mac:\n"
                         "./scripts/pack-video.sh show.mkv\n"
                         "then copy the .lcv to\n/littlecube/video/ on the card.",
                         widgets::TextStyle::Body, theme::kTextDim);
    } else {
      char line[64];
      snprintf(line, sizeof(line), "SD card: %s", sdCardStateName(sd));
      widgets::textBlock(gfx, theme::kPadding, y + theme::kPadding,
                         DISPLAY_WIDTH - 2 * theme::kPadding, line, widgets::TextStyle::Body,
                         theme::kWarn);
    }
  } else {
    char sub[24];
    for (size_t i = 0; i < itemCount_; i++) {
      if (items_[i].isDir) {
        snprintf(sub, sizeof(sub), "folder");
      } else {
        formatMs(items_[i].durationMs, sub, sizeof(sub));
      }
      rowRects_[i] = widgets::listItem(gfx, theme::kPadding, y,
                                       DISPLAY_WIDTH - 2 * theme::kPadding, items_[i].name,
                                       sub, false);
      y = rowRects_[i].y + rowRects_[i].h + 6;
    }
    if (totalItems_ > itemCount_) {
      char more[32];
      snprintf(more, sizeof(more), "+%u more — swipe up",
               static_cast<unsigned>(totalItems_ - itemCount_));
      widgets::textCentered(gfx, 0, y + 4, DISPLAY_WIDTH, more, widgets::TextStyle::Caption,
                            theme::kTextDim);
    }
  }
  if (toastMs_ > 0 && toast_[0] != '\0') {
    widgets::toast(gfx, toast_);
  }
}

void VideoApp::renderConfirm(Arduino_GFX& gfx) {
  gfx.fillScreen(theme::kBg);
  char body[96];
  if (batteryWarned_) {
    snprintf(body, sizeof(body), "Battery is low.\nVideo lasts ~30-45 min per charge.");
    confirmRect_ = widgets::modalConfirm(gfx, "Low battery", body, cancelRect_);
  } else {
    char at[16];
    formatMs(pendingResumeMs_, at, sizeof(at));
    snprintf(body, sizeof(body), "Resume at %s?\nCancel starts over.", at);
    confirmRect_ = widgets::modalConfirm(gfx, "Resume", body, cancelRect_);
  }
}

bool VideoApp::handleInput(const InputEvent& event) {
  switch (screen_) {
    case Screen::Player:
      return playerInput(event);
    case Screen::ConfirmStart:
      if (event.action == InputAction::Tap) {
        if (confirmRect_.contains(event.x, event.y)) {
          if (batteryWarned_) {
            // Battery acknowledged; fall through to the resume question.
            batteryWarned_ = false;
            if (pendingResumeMs_ > 0) {
              dirty_ = true;
              return true;
            }
            beginPlayback(0);
            return true;
          }
          beginPlayback(pendingResumeMs_);
          return true;
        }
        if (cancelRect_.contains(event.x, event.y)) {
          if (batteryWarned_) {
            screen_ = Screen::Library;  // declined at the battery warning
          } else {
            beginPlayback(0);  // "start over"
          }
          dirty_ = true;
          return true;
        }
      }
      if (event.action == InputAction::Back) {
        screen_ = Screen::Library;
        dirty_ = true;
        return true;
      }
      return false;
    case Screen::Library:
      break;
  }
  switch (event.action) {
    case InputAction::Tap:
      for (size_t i = 0; i < itemCount_; i++) {
        if (rowRects_[i].contains(event.x, event.y)) {
          openItem(i);
          return true;
        }
      }
      return false;
    case InputAction::SwipeUp:
      if (totalItems_ > static_cast<size_t>(page_ + 1) * kMaxListed) {
        showPage(page_ + 1);
      }
      return true;
    case InputAction::SwipeDown:
      if (page_ > 0) {
        showPage(page_ - 1);
      }
      return true;
    case InputAction::Back:
      if (strcmp(dir_, paths::kVideo) != 0) {
        onOpen();  // leave the folder, back to the root listing
        return true;
      }
      return false;  // at the root: fall through to the router
    default:
      return false;
  }
}

// ---- Player rendering & input --------------------------------------------

void VideoApp::renderPlayer(Arduino_GFX& gfx) {
  // The video image area is owned by VideoPlayer::decodeFrame(); this method
  // touches ONLY the right-hand strip (the "bottom" once the device is
  // turned). Never clear the whole screen here — it would fight the decoder.
  if (!chromeDirty_) {
    return;
  }
  chromeDirty_ = false;
  const bool upright = uprightMode();
  if (!chromeVisible_) {
    if (upright) {
      gfx.fillRect(0, DISPLAY_HEIGHT - kUprightChromeH, DISPLAY_WIDTH, kUprightChromeH,
                   RGB565_BLACK);
    } else {
      gfx.fillRect(DISPLAY_WIDTH - kChromeH, 0, kChromeH, DISPLAY_HEIGHT, RGB565_BLACK);
    }
    services_.display->markDirty();
    return;
  }
  if (upright) {
    renderChromeUpright(gfx);
  } else {
    renderChrome(gfx);
  }
  services_.display->markDirty();
}

// The readout both layouts show, right of their scrub area.
void VideoApp::formatReadout(char* out, size_t len) const {
  const VideoPlayer* player = services_.videoPlayer;
  if (player == nullptr) {
    out[0] = '\0';
    return;
  }
  if (volShownMs_ > 0) {
    // Sized for %u's worst case (uint8_t, 3 digits) — well under 64.
    snprintf(out, len, "vol %u%%", static_cast<unsigned>(services_.audio->volumePercent()));
    return;
  }
  char pos[16];
  char dur[16];
  formatMs(player->positionMs(), pos, sizeof(pos));
  formatMs(player->durationMs(), dur, sizeof(dur));
  // Sized for %d's worst case (a full signed 32-bit range), not the ~0-100
  // the field actually holds, so -Wformat-truncation has nothing to warn
  // about — batteryPercent's declared type is a plain int with no compile-
  // time-provable bound.
  char batt[20] = "";
  if (services_.state->batteryPercent >= 0) {
    snprintf(batt, sizeof(batt), " - %d%%", services_.state->batteryPercent);
  }
  snprintf(out, len, "%s / %s%s", pos, dur, batt);
}

// Upright chrome: a horizontal strip along the bottom, drawn STRAIGHT into
// the frame canvas. The only reason renderChrome() needs its own canvas is to
// draw landscape text and then turn it; here the text is already the right
// way up, so there is nothing to transpose and no buffer to hold it in. Hit
// rects therefore come back from widgets::button() already in portrait space.
//
// The band is 136 tall from y=312, in three rows: scrub, buttons, then
// widgets::footer() for the title and time readout. footer() owns the bottom
// of the panel deliberately — it is the only thing that gets the corner-radius
// inset and the burn-in offsets right (CLAUDE.md), and hand-placing text down
// there is the exact bug it exists to prevent.
void VideoApp::renderChromeUpright(Arduino_GFX& gfx) {
  VideoPlayer* player = services_.videoPlayer;
  if (player == nullptr) {
    return;
  }
  const int16_t top = DISPLAY_HEIGHT - kUprightChromeH;  // 312
  // Unshifted, so shifted content inside it still lands within the band.
  gfx.fillRect(0, top, DISPLAY_WIDTH, kUprightChromeH, RGB565_BLACK);

  const int16_t sx = theme::kSafeInset;
  const int16_t sw = DISPLAY_WIDTH - 2 * theme::kSafeInset;  // 328

  // Row 1: scrub bar, drawn 6px thin with a 30px touch band around it. A 6px
  // target is unhittable, and a hit rect is not ink, so it can be generous
  // where the drawing cannot.
  const int16_t barY = top + 18;  // 330
  gfx.fillRect(sx, barY, sw, 6, theme::kPanelAlt);
  if (player->durationMs() > 0) {
    const int16_t fill = static_cast<int16_t>(
        static_cast<int64_t>(sw) * player->positionMs() / player->durationMs());
    gfx.fillRect(sx, barY, fill, 6, theme::kAccent);
  }
  scrubRect_ = {sx, static_cast<int16_t>(top + 6), sw, 30};

  // Row 2: the same six buttons as landscape, same order, spread across the
  // safe width. 6*48 + 5*8 = 328 = DISPLAY_WIDTH - 2*kSafeInset exactly.
  struct Btn {
    const char* label;
    widgets::Rect* rect;
  };
  const char* playLabel = player->paused() ? ">" : "||";
  static constexpr int16_t kBtnW = 48;
  static constexpr int16_t kBtnGap = 8;
  Btn btns[6] = {{"<-", &backRect_},  {"|<", &prevRect_},   {playLabel, &playRect_},
                 {">|", &nextRect_}, {"-", &volDownRect_}, {"+", &volUpRect_}};
  int16_t bx = sx;
  for (auto& b : btns) {
    *b.rect = widgets::button(gfx, bx, top + 40, kBtnW, 48, b.label);  // 352..400
    bx += kBtnW + kBtnGap;
  }

  // Row 3: title and readout. footer() truncates the left with an ellipsis
  // when the two would collide — the rule the rotated path open-codes.
  char times[64];
  formatReadout(times, sizeof(times));
  const char* leaf = strrchr(pendingPath_, '/');
  leaf = leaf != nullptr ? leaf + 1 : pendingPath_;
  widgets::footer(gfx, leaf, times, services_.amoled->shiftX(), services_.amoled->shiftY());
}

void VideoApp::renderChrome(Arduino_GFX& gfx) {
  VideoPlayer* player = services_.videoPlayer;
  if (player == nullptr) {
    return;
  }
  if (chrome_ == nullptr) {
    return;  // syncChromeBuffer() could not get one; the picture still plays
  }
  // 1) Draw the chrome in LANDSCAPE into the 448x58 canvas using the normal
  //    text helpers. Layout left->right: back, prev, play/pause, next,
  //    vol down, vol up, then the scrub bar with the time readout above it.
  //    Button width dropped 44 -> 40 to make room for the sixth button
  //    (stop, redundant with back, was removed) while keeping the scrub
  //    area comfortably over 100 px: bx = 20 + 6*(40+6) = 296, sx = 300,
  //    sw = 448 - 20 - 300 = 128 px.
  Arduino_GFX& c = *chrome_;
  c.fillScreen(RGB565_BLACK);
  struct Btn {
    const char* label;
    widgets::Rect* rect;
  };
  const char* playLabel = player->paused() ? ">" : "||";
  static constexpr int16_t kBtnW = 40;
  Btn btns[6] = {{"<-", &backRect_},  {"|<", &prevRect_}, {playLabel, &playRect_},
                 {">|", &nextRect_}, {"-", &volDownRect_}, {"+", &volUpRect_}};
  int16_t bx = theme::kSafeInset;  // inset from the panel's rounded corner
  for (auto& b : btns) {
    const widgets::Rect r = widgets::button(c, bx, 4, kBtnW, kChromeH - 8, b.label);
    // Store the PORTRAIT-space hit rect now (see the mapping note below).
    b.rect->x = DISPLAY_WIDTH - kChromeH;
    b.rect->y = r.x;
    b.rect->w = kChromeH;
    b.rect->h = r.w;
    bx += kBtnW + 6;
  }
  // Scrub bar in the remaining width.
  const int16_t sx = bx + 4;
  const int16_t sw = kChromeW - theme::kSafeInset - sx;
  char times[64];
  formatReadout(times, sizeof(times));
  // Episode title beside the time readout, same row, splitting the scrub
  // area's width rather than reworking the 58px-tall layout: the time text
  // moves from centered to right-aligned, and the title fills whatever's
  // left, truncated to fit.
  const int16_t timesW = widgets::textWidth(c, times, widgets::TextStyle::Caption);
  widgets::textRight(c, sx + sw, 6, times, widgets::TextStyle::Caption, theme::kText);
  const int16_t titleAreaW = sw - timesW - 8;
  if (titleAreaW > 10) {
    const char* leaf = strrchr(pendingPath_, '/');
    leaf = leaf != nullptr ? leaf + 1 : pendingPath_;
    char title[48];
    strncpy(title, leaf, sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    while (title[0] != '\0' &&
           widgets::textWidth(c, title, widgets::TextStyle::Caption) > titleAreaW) {
      title[strlen(title) - 1] = '\0';
    }
    widgets::text(c, sx, 6, title, widgets::TextStyle::Caption, theme::kTextDim);
  }
  const int16_t barY = kChromeH - 18;
  c.fillRect(sx, barY, sw, 6, theme::kPanelAlt);
  if (player->durationMs() > 0) {
    const int16_t fill = static_cast<int16_t>(
        static_cast<int64_t>(sw) * player->positionMs() / player->durationMs());
    c.fillRect(sx, barY, fill, 6, theme::kAccent);
  }
  scrubRect_ = {static_cast<int16_t>(DISPLAY_WIDTH - kChromeH), sx, kChromeH, sw};

  // 2) Transpose onto the panel strip. Same handedness as the frames
  //    (ffmpeg transpose=1, 90 deg CW): dst(x, y) = chrome(y, kChromeH-1-x).
  //    If chrome text reads upside-down relative to the video on device,
  //    change the source index to src[x * kChromeW + (kChromeW - 1 - y)].
  DisplayAdapter* display = services_.display;
  if (!display->hasCanvas()) {
    return;  // no framebuffer to transpose into (degraded direct-draw mode)
  }
  uint16_t* dst = static_cast<Arduino_Canvas*>(display->canvas())->getFramebuffer();
  const uint16_t* src = chrome_->getFramebuffer();
  const int16_t stripX = DISPLAY_WIDTH - kChromeH;
  for (int16_t y = 0; y < DISPLAY_HEIGHT; y++) {
    uint16_t* row = dst + y * DISPLAY_WIDTH + stripX;
    for (int16_t x = 0; x < kChromeH; x++) {
      row[x] = src[(kChromeH - 1 - x) * kChromeW + y];
    }
  }
}

bool VideoApp::playerInput(const InputEvent& event) {
  VideoPlayer* player = services_.videoPlayer;
  if (player == nullptr) {
    return false;
  }
  // Any interaction (re)shows the chrome and rearms the hide timer.
  auto poke = [&]() {
    chromeMs_ = 0;
    if (!chromeVisible_) {
      chromeVisible_ = true;
      chromeDirty_ = true;
    }
  };
  switch (event.action) {
    case InputAction::Tap:
      if (!chromeVisible_) {
        poke();
        return true;
      }
      poke();
      if (playRect_.contains(event.x, event.y)) {
        player->setPaused(!player->paused());
        chromeDirty_ = true;
        return true;
      }
      if (backRect_.contains(event.x, event.y)) {
        stopAndSavePosition();
        screen_ = Screen::Library;
        refreshList();
        dirty_ = true;
        return true;
      }
      if (volDownRect_.contains(event.x, event.y) || volUpRect_.contains(event.x, event.y)) {
        stepVolume(volUpRect_.contains(event.x, event.y));
        return true;
      }
      if (nextRect_.contains(event.x, event.y) || prevRect_.contains(event.x, event.y)) {
        const bool fwd = nextRect_.contains(event.x, event.y);
        char sib[160];
        if ((fwd ? services_.video->nextInFolder(pendingPath_, sib, sizeof(sib))
                 : services_.video->prevInFolder(pendingPath_, sib, sizeof(sib)))) {
          services_.video->savePosition(player->path(), player->positionMs(),
                                        player->durationMs());
          player->requestStop();
          strncpy(pendingPath_, sib, sizeof(pendingPath_) - 1);
          pendingPath_[sizeof(pendingPath_) - 1] = '\0';
          // beginPlayback() once the stop lands: reuse the natural-end path
          // by waiting for idle in update() — simplest is to poll here:
          pendingResumeMs_ = 0;
          // Mark so update()'s idle edge starts the pending sibling.
          wasPlaying_ = true;
          nextQueued_ = true;
        }
        return true;
      }
      if (scrubRect_.contains(event.x, event.y)) {
        // Rotated draws the bar turned onto the vertical strip, so portrait y
        // is the fraction; upright draws it normally, so portrait x is.
        const bool upright = uprightMode();
        const int32_t frac = upright ? event.x - scrubRect_.x : event.y - scrubRect_.y;
        const int32_t span = upright ? scrubRect_.w : scrubRect_.h;
        const uint32_t target = static_cast<uint32_t>(
            static_cast<int64_t>(player->durationMs()) * frac / span);
        player->requestSeek(static_cast<int32_t>(target) -
                            static_cast<int32_t>(player->positionMs()));
        return true;
      }
      chromeVisible_ = false;  // tap on the picture: hide the chrome
      chromeDirty_ = true;
      return true;
    case InputAction::DoubleTap:
      poke();
      player->setPaused(!player->paused());
      chromeDirty_ = true;
      return true;
    // One table, two orientations — see gestureFor(). The physical gesture the
    // user makes is the same in both modes; only its panel-space name differs,
    // because Rotated asks them to turn the device 90 degrees.
    case InputAction::SwipeUp:
    case InputAction::SwipeDown:
    case InputAction::SwipeLeft:
    case InputAction::SwipeRight:
      poke();
      switch (gestureFor(event.action)) {
        case Gesture::SeekForward:
          player->requestSeek(kSeekStepMs);
          break;
        case Gesture::SeekBack:
          player->requestSeek(-kSeekStepMs);
          break;
        case Gesture::VolumeUp:
          stepVolume(true);
          break;
        case Gesture::VolumeDown:
          stepVolume(false);
          break;
        case Gesture::None:
          break;
      }
      return true;
    case InputAction::Back:
      stopAndSavePosition();
      screen_ = Screen::Library;
      refreshList();
      dirty_ = true;
      return true;
    default:
      return false;  // Home falls through: the router homes, onPause saves+stops
  }
}

#endif  // FEATURE_VIDEO
