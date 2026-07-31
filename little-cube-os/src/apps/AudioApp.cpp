#include "AudioApp.h"

#include <Arduino_GFX_Library.h>

#include <string.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/SdCardAdapter.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../services/SettingsService.h"
#include "../storage/StoragePaths.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"

namespace {
constexpr int16_t kTop = theme::kStatusBarHeight + 12;
const char* kCategories[4] = {"Recordings", "Music", "Podcasts", "Radio"};

// Clip a row label to the pixel width the list cell allows, so a long track
// title is truncated with a ".." ellipsis instead of overrunning into the
// transport controls. The built-in GFX font is a fixed 6px cell, scaled by the
// text size.
void clipToWidth(char* out, size_t outSz, const char* name, int16_t widthPx) {
  if (out == nullptr || outSz == 0) {
    return;
  }
  const int16_t cell = 6 * theme::kTextSizeSmall;
  size_t maxChars = (cell > 0 && widthPx > 0) ? static_cast<size_t>(widthPx / cell) : 0;
  if (maxChars > outSz - 1) {
    maxChars = outSz - 1;
  }
  const size_t len = name != nullptr ? strlen(name) : 0;
  if (len <= maxChars) {
    if (len > 0) {
      memcpy(out, name, len);
    }
    out[len] = '\0';
    return;
  }
  if (maxChars >= 3) {
    const size_t keep = maxChars - 2;
    memcpy(out, name, keep);
    out[keep] = '.';
    out[keep + 1] = '.';
    out[keep + 2] = '\0';
  } else {
    memcpy(out, name, maxChars);
    out[maxChars] = '\0';
  }
}
}  // namespace

void AudioApp::onOpen() {
  screen_ = Screen::Categories;
  dirty_ = true;
}

void AudioApp::onResume() {
  // Playback keeps running in the background on purpose, so a list screen can
  // have gone stale (a serial delete, a card swap, or a track finishing while
  // the app was away).
  if (screen_ == Screen::Recordings || isTrackScreen() || isRadioScreen()) {
    refreshList();
    syncPlayingIndex();
  }
  dirty_ = true;
}

const char* AudioApp::currentTrackDir() const {
  return screen_ == Screen::Podcasts ? paths::kPodcastDownloads : paths::kMusic;
}

void AudioApp::refreshList() {
  if (isRadioScreen()) {
    radioCount_ = 0;
    totalRadioStations_ = 0;
    if (services_.radio != nullptr) {
      radioCount_ = services_.radio->list(radioStations_, kMaxListed, &totalRadioStations_);
    }
    return;
  }
  if (isTrackScreen()) {
    trackCount_ = 0;
    totalTracks_ = 0;
    if (services_.music != nullptr) {
      trackCount_ = services_.music->list(currentTrackDir(), tracks_, kMaxListed, &totalTracks_,
                                          page_ > 0 ? pageAnchors_[page_] : nullptr);
      // A page can empty out under us (file removed, card swapped) — fall back
      // to the first page rather than showing a blank screen.
      if (trackCount_ == 0 && page_ > 0) {
        page_ = 0;
        trackCount_ = services_.music->list(currentTrackDir(), tracks_, kMaxListed, &totalTracks_,
                                            nullptr);
      }
    }
    return;
  }

  recordingCount_ = 0;
  totalRecordings_ = 0;
  if (services_.recorder == nullptr) {
    return;
  }
  recordingCount_ = services_.recorder->list(recordings_, kMaxListed, &totalRecordings_,
                                             page_ > 0 ? pageAnchors_[page_] : nullptr);
  if (recordingCount_ == 0 && page_ > 0) {
    page_ = 0;
    recordingCount_ = services_.recorder->list(recordings_, kMaxListed, &totalRecordings_, nullptr);
  }
}

void AudioApp::showPage(uint8_t page) {
  page_ = page;
  refreshList();
  // Row indices are page-local; re-derive the highlight from what is actually
  // playing so it lands on the right row (or on none).
  syncPlayingIndex();
  dirty_ = true;
}

void AudioApp::syncPlayingIndex() {
  playingIndex_ = -1;
  AudioAdapter* audio = services_.audio;
  if (audio == nullptr || !audio->isPlaying()) {
    return;
  }
  if (isRadioScreen() && audio->isRadioPlaying()) {
    const char* cur = audio->radioUrl();
    for (size_t i = 0; i < listCount(); i++) {
      if (strcmp(rowPath(i), cur) == 0) {
        playingIndex_ = static_cast<int>(i);
        return;
      }
    }
    return;
  }
  const char* cur = audio->playingPath();
  if (cur == nullptr || cur[0] == '\0') {
    return;
  }
  for (size_t i = 0; i < listCount(); i++) {
    if (strcmp(rowPath(i), cur) == 0) {
      playingIndex_ = static_cast<int>(i);
      return;
    }
  }
}

void AudioApp::advanceTrack() {
  AudioAdapter* audio = services_.audio;
  // playingIndex_ < 0 means nothing from THIS list was playing (a user stop
  // clears it, and a recording playing in the background never matches a row),
  // so there is nothing to advance.
  if (audio == nullptr || playingIndex_ < 0) {
    return;
  }
  // A card pulled mid-track leaves a stale list whose files no longer open;
  // don't chase it — just stop.
  if (services_.sdCard != nullptr && !services_.sdCard->mounted()) {
    playingIndex_ = -1;
    dirty_ = true;
    return;
  }
  const size_t next = static_cast<size_t>(playingIndex_) + 1;
  if (next < trackCount_) {
    playingIndex_ = audio->requestPlayMusicFile(tracks_[next].path) ? static_cast<int>(next) : -1;
    dirty_ = true;
    return;
  }
  // End of this page: follow the track onto the next page if the directory has
  // more, mirroring the manual swipe-up paging, and play its first row.
  if (trackCount_ == kMaxListed &&
      static_cast<size_t>(page_ + 1) * kMaxListed < totalTracks_ && page_ + 1 < kMaxPages) {
    const uint8_t np = page_ + 1;
    strncpy(pageAnchors_[np], tracks_[trackCount_ - 1].name, sizeof(pageAnchors_[np]) - 1);
    pageAnchors_[np][sizeof(pageAnchors_[np]) - 1] = '\0';
    page_ = np;
    refreshList();
    playingIndex_ = (trackCount_ > 0 && audio->requestPlayMusicFile(tracks_[0].path)) ? 0 : -1;
    dirty_ = true;
    return;
  }
  // True end of the directory (or the paging ceiling): stop.
  playingIndex_ = -1;
  dirty_ = true;
}

void AudioApp::update(uint32_t deltaMs) {
  AudioAdapter* audio = services_.audio;
  const bool playing = audio != nullptr && audio->isPlaying();

  // Auto-advance on the playing->idle edge — Music/Podcasts only, and only when
  // the track ended on its own (lastPlayCompleted), never on a user stop and
  // never for recordings. AudioAdapter::update() runs earlier in this same
  // frame, so isPlaying() is this frame's truth, not last frame's.
  if (wasPlaying_ && !playing) {
    if (isTrackScreen() && audio != nullptr && audio->lastPlayCompleted()) {
      advanceTrack();
    }
    dirty_ = true;  // repaint away the "playing" label on any stop
  }

  // Keep the highlight/label fresh about once a second while something plays.
  if ((screen_ == Screen::Recordings || isTrackScreen() || isRadioScreen()) && audio != nullptr &&
      audio->isPlaying()) {
    tickMs_ += deltaMs;
    if (tickMs_ >= 1000) {
      tickMs_ = 0;
      dirty_ = true;
    }
  }

  // Recompute after a possible advance so the next frame's edge test is honest.
  wasPlaying_ = audio != nullptr && audio->isPlaying();
}

void AudioApp::render() {
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
  statusBar_.render(gfx, state, services_.amoled->shiftX(), services_.amoled->shiftY());

  const char* title = "Audio";
  switch (screen_) {
    case Screen::Recordings:
      title = "Recordings";
      break;
    case Screen::Music:
      title = "Music";
      break;
    case Screen::Podcasts:
      title = "Podcasts";
      break;
    case Screen::Radio:
      title = "Radio";
      break;
    default:
      break;
  }
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print(title);

  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;

  if (screen_ == Screen::Categories) {
    int16_t y = kTop + 44;
    for (uint8_t i = 0; i < 4; i++) {
      categoryRects_[i] = widgets::button(gfx, theme::kPadding, y, w, 62, kCategories[i], i == 0);
      y += 74;
    }
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kTextDim);
    gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
    display->markDirty();  // without this the canvas is drawn but never flushed
    return;
  }

  // Shared player for Recordings (WAV), Music/Podcasts (decoded tracks), and
  // Radio (live MP3 stations).
  AudioAdapter* audio = services_.audio;
  const bool playing = audio != nullptr && audio->isPlaying();

  int16_t y = kTop + 40;
  char rowLabel[48];
  for (size_t i = 0; i < kMaxListed; i++) {
    if (i >= listCount()) {
      rowRects_[i] = widgets::Rect{};
      continue;
    }
    const bool isCurrent = playing && playingIndex_ == static_cast<int>(i);
    clipToWidth(rowLabel, sizeof(rowLabel), rowName(i), w - 16);
    rowRects_[i] = widgets::listItem(gfx, theme::kPadding, y, w, rowLabel,
                                     isCurrent ? (audio->playbackPaused() ? "paused" : "playing")
                                               : nullptr,
                                     isCurrent);
    y += isCurrent ? 62 : 46;
  }

  if (listCount() == 0) {
    const bool noCard = services_.sdCard != nullptr && !services_.sdCard->mounted();
    const char* msg;
    if (screen_ == Screen::Radio) {
      msg = noCard ? "No SD card. Insert one with /littlecube/radio/stations.txt."
                   : "No stations yet. Add stations over serial with radio add.";
    } else if (screen_ == Screen::Music) {
      msg = noCard ? "No SD card. Insert a card with music in /littlecube/music."
                   : "No music yet. Copy audio files to /littlecube/music.";
    } else if (screen_ == Screen::Podcasts) {
      msg = noCard ? "No SD card. Insert a card, then add episodes to "
                     "/littlecube/podcasts/downloads."
                   : "No downloads yet. Episodes go in /littlecube/podcasts/downloads.";
    } else {
      msg = "no recordings yet - use Recorder";
    }
    widgets::textBlock(gfx, theme::kPadding, y + 8, w, msg, theme::kTextSizeSmall, theme::kTextDim);
  }

  // Controls inset from the rounded-corner safe area and pulled up so the
  // volume row's bottom edge clears the bottom bezel (the +/- used to sit in
  // the corner and get clipped).
  const int16_t sx = theme::kSafeInset;
  const int16_t sw = DISPLAY_WIDTH - 2 * theme::kSafeInset;
  const int16_t controlsY = DISPLAY_HEIGHT - 128;

  // The list is a window onto the card, so say how big the card's list really
  // is — a bare four rows reads as "these are all of them".
  if (listTotal() > 0) {
    const size_t first = page_ * kMaxListed + 1;
    char pager[48];
    if (listCount() > 0 && listTotal() > listCount()) {
      snprintf(pager, sizeof(pager), "%u-%u of %u · swipe up/down", (unsigned)first,
               (unsigned)(first + listCount() - 1), (unsigned)listTotal());
    } else {
      const char* noun = isTrackScreen() ? "track" : (isRadioScreen() ? "station" : "recording");
      snprintf(pager, sizeof(pager), "%u %s%s", (unsigned)listTotal(), noun,
               listTotal() == 1 ? "" : "s");
    }
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kTextDim);
    gfx.setCursor(sx, controlsY - 24);
    gfx.print(pager);
  }

  pauseRect_ = widgets::button(gfx, sx, controlsY, (sw - 8) / 2, 50,
                               playing && audio->playbackPaused() ? "resume" : "pause", false);
  stopRect_ = widgets::button(gfx, sx + (sw + 8) / 2, controlsY, (sw - 8) / 2, 50, "stop", false);

  char vol[8];
  snprintf(vol, sizeof(vol), "%u%%",
           (unsigned)(services_.settings != nullptr ? services_.settings->volumePercent() : 0));
  volDownRect_ = widgets::button(gfx, sx, controlsY + 56, 56, 44, "-", false);
  volUpRect_ = widgets::button(gfx, sx + sw - 56, controlsY + 56, 56, 44, "+", false);
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(DISPLAY_WIDTH / 2 - 20, controlsY + 66);
  gfx.print(vol);

  if (isRadioScreen() && audio->isRadioPlaying()) {
    char rawStatus[228];
    snprintf(rawStatus, sizeof(rawStatus), "%s: %s%s%s", audio->radioStationName(),
             audio->radioStatus(), audio->radioMetadata()[0] ? " · " : "",
             audio->radioMetadata());
    char status[128];
    clipToWidth(status, sizeof(status), rawStatus, w);
    widgets::textBlock(gfx, theme::kPadding, controlsY - 48, w, status,
                       theme::kTextSizeSmall, theme::kTextDim);
  } else if (isRadioScreen() && strcmp(audio->radioStatus(), "idle") != 0) {
    widgets::textBlock(gfx, theme::kPadding, controlsY - 48, w, audio->radioStatus(),
                       theme::kTextSizeSmall, theme::kTextDim);
  }

  display->markDirty();
}

bool AudioApp::handleInput(const InputEvent& event) {
  if (screen_ == Screen::Categories) {
    if (event.action == InputAction::Tap) {
      for (uint8_t i = 0; i < 4; i++) {
        if (categoryRects_[i].contains(event.x, event.y)) {
          switch (i) {
            case 0:
              screen_ = Screen::Recordings;
              break;
            case 1:
              screen_ = Screen::Music;
              break;
            case 2:
              screen_ = Screen::Podcasts;
              break;
            default:
              screen_ = Screen::Radio;
              page_ = 0;
              refreshList();
              syncPlayingIndex();
              dirty_ = true;
              return true;
          }
          // screen_ is set before refreshList(), which dispatches on it.
          page_ = 0;
          refreshList();
          syncPlayingIndex();
          dirty_ = true;
          return true;
        }
      }
      return true;
    }
    return false;
  }

  if (event.action == InputAction::Back || event.action == InputAction::Cancel ||
      event.action == InputAction::SwipeRight) {
    screen_ = Screen::Categories;
    dirty_ = true;
    return true;
  }

  // Paging through the list window (newest-first recordings, alphabetical tracks).
  if (event.action == InputAction::SwipeUp) {
    if (listCount() == kMaxListed &&
        static_cast<size_t>(page_ + 1) * kMaxListed < listTotal() && page_ + 1 < kMaxPages) {
      const uint8_t next = page_ + 1;
      strncpy(pageAnchors_[next], rowName(listCount() - 1), sizeof(pageAnchors_[next]) - 1);
      pageAnchors_[next][sizeof(pageAnchors_[next]) - 1] = '\0';
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

  AudioAdapter* audio = services_.audio;
  if (audio == nullptr) {
    return false;
  }
  if (event.action == InputAction::Tap) {
    if (pauseRect_.contains(event.x, event.y)) {
      audio->pausePlayback(!audio->playbackPaused());
      dirty_ = true;
      return true;
    }
    if (stopRect_.contains(event.x, event.y)) {
      audio->stopPlayback();
      playingIndex_ = -1;  // a user stop must not auto-advance
      dirty_ = true;
      return true;
    }
    if (volDownRect_.contains(event.x, event.y) || volUpRect_.contains(event.x, event.y)) {
      const int step = volUpRect_.contains(event.x, event.y) ? 10 : -10;
      int next = (services_.settings != nullptr ? services_.settings->volumePercent() : 70) + step;
      if (next < 0) next = 0;
      if (next > 100) next = 100;
      if (services_.settings != nullptr) {
        services_.settings->setVolumePercent(static_cast<uint8_t>(next));
      }
      audio->setVolumePercent(static_cast<uint8_t>(next));
      dirty_ = true;
      return true;
    }
    for (size_t i = 0; i < listCount(); i++) {
      if (rowRects_[i].contains(event.x, event.y)) {
        bool ok = false;
        if (isRadioScreen()) {
          if (services_.state == nullptr || !services_.state->internet) {
            audio->noteRadioStatus("no internet");
          } else {
            ok = services_.radio != nullptr &&
                 audio->requestPlayRadio(rowPath(i), rowName(i));
          }
        } else {
          ok = isTrackScreen() ? audio->requestPlayMusicFile(rowPath(i))
                               : audio->requestPlayWavFile(rowPath(i));
        }
        if (ok) {
          playingIndex_ = static_cast<int>(i);
        }
        dirty_ = true;
        return true;
      }
    }
    return true;
  }
  return false;
}
