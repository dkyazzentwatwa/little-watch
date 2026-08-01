#include "VideoApp.h"

#if FEATURE_VIDEO

#include <Arduino_GFX_Library.h>

#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/SdCardState.h"
#include "../hardware/audio/AudioAdapter.h"
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
  if (chrome_ == nullptr) {
    chrome_ = new Arduino_Canvas(kChromeW, kChromeH, nullptr);
    chrome_->begin(GFX_SKIP_OUTPUT_BEGIN);
  }
  dirty_ = true;

  adoptExternalPlayback();
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
  screen_ = Screen::Player;
  chromeVisible_ = true;
  chromeDirty_ = true;
  chromeMs_ = 0;
  saveMs_ = 0;
  wasPlaying_ = true;
  // The decoder only writes the centered band; blank the margins once.
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
  screen_ = Screen::Player;
  chromeVisible_ = true;
  chromeDirty_ = true;
  chromeMs_ = 0;
  saveMs_ = 0;
  wasPlaying_ = true;
  // Full black once; the decoder owns the image area from here on. The
  // decoder only ever writes the centered 252-wide band, so this is the
  // only place the margins get blanked before playback starts.
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
  const int16_t stripX = DISPLAY_WIDTH - kChromeH;  // 310
  if (!chromeVisible_) {
    gfx.fillRect(stripX, 0, kChromeH, DISPLAY_HEIGHT, RGB565_BLACK);
    services_.display->markDirty();
    return;
  }
  renderChrome(gfx);
  services_.display->markDirty();
}

void VideoApp::renderChrome(Arduino_GFX& gfx) {
  VideoPlayer* player = services_.videoPlayer;
  if (chrome_ == nullptr || player == nullptr) {
    return;
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
  char times[64];
  if (volShownMs_ > 0) {
    // Sized for %u's worst case (uint8_t, 3 digits) — well under 64.
    snprintf(times, sizeof(times), "vol %u%%",
             static_cast<unsigned>(services_.audio->volumePercent()));
  } else {
    snprintf(times, sizeof(times), "%s / %s%s", pos, dur, batt);
  }
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
        const bool up = volUpRect_.contains(event.x, event.y);
        const uint8_t cur = services_.audio->volumePercent();
        services_.audio->setVolumePercent(up ? (cur >= 90 ? 100 : cur + 10)
                                             : (cur <= 10 ? 0 : cur - 10));
        volShownMs_ = kVolShowMs;
        chromeDirty_ = true;
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
        // Portrait y within the scrub rect maps to landscape x = fraction.
        const int32_t frac = event.y - scrubRect_.y;
        const uint32_t target = static_cast<uint32_t>(
            static_cast<int64_t>(player->durationMs()) * frac / scrubRect_.h);
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
    // Rotated-90 gesture map (device turned CCW to watch): an in-hand
    // horizontal swipe arrives as portrait Up/Down = seek; an in-hand
    // vertical swipe arrives as portrait Left/Right = volume.
    case InputAction::SwipeDown:
      poke();
      player->requestSeek(kSeekStepMs);
      return true;
    case InputAction::SwipeUp:
      poke();
      player->requestSeek(-kSeekStepMs);
      return true;
    case InputAction::SwipeRight:
      poke();
      services_.audio->setVolumePercent(
          services_.audio->volumePercent() >= 90 ? 100 : services_.audio->volumePercent() + 10);
      volShownMs_ = kVolShowMs;
      chromeDirty_ = true;
      return true;
    case InputAction::SwipeLeft:
      poke();
      services_.audio->setVolumePercent(
          services_.audio->volumePercent() <= 10 ? 0 : services_.audio->volumePercent() - 10);
      volShownMs_ = kVolShowMs;
      chromeDirty_ = true;
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
