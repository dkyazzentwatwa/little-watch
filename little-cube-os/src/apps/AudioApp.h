#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../services/MusicService.h"
#include "../services/RecorderService.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

// Audio (spec §24): four categories. Recordings (WAV), Music, and Podcasts are
// fully playable in v1 — the same transport UI (rows + play / pause / stop /
// volume + paging) over three sources: RecorderService for recordings,
// MusicService for the /littlecube/music and /littlecube/podcasts/downloads
// libraries. Only Radio is still an honest ComingSoon (station streaming is a
// later phase). Music and Podcasts auto-advance to the next track when one
// finishes on its own; recordings never auto-advance.
class AudioApp : public App {
 public:
  explicit AudioApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override {}
  void onPause() override {}
  void onResume() override;

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  enum class Screen {
    Categories,
    Recordings,
    Music,
    Podcasts,
    ComingSoon,
  };

  // Music and Podcasts share every bit of player logic; only the source
  // directory and the empty-state wording differ.
  bool isTrackScreen() const { return screen_ == Screen::Music || screen_ == Screen::Podcasts; }
  const char* currentTrackDir() const;

  void refreshList();
  void showPage(uint8_t page);
  // Re-point the now-playing highlight at whatever the engine is actually
  // playing, matched by path against the visible page (or clear it). Robust
  // across paging and screen switches — a stale row never highlights.
  void syncPlayingIndex();
  // On a natural track end (Music/Podcasts only): start the next row, turning
  // the page if needed; stop at the true end of the directory.
  void advanceTrack();

  // Screen-agnostic views over whichever list is active, so render(), paging,
  // and tap handling need no per-screen branching.
  size_t listCount() const { return isTrackScreen() ? trackCount_ : recordingCount_; }
  size_t listTotal() const { return isTrackScreen() ? totalTracks_ : totalRecordings_; }
  const char* rowName(size_t i) const {
    return isTrackScreen() ? tracks_[i].name : recordings_[i].name;
  }
  const char* rowPath(size_t i) const {
    return isTrackScreen() ? tracks_[i].path : recordings_[i].path;
  }

  Services& services_;
  StatusBar statusBar_;
  Screen screen_ = Screen::Categories;
  const char* comingSoonWhat_ = "";
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;
  uint32_t tickMs_ = 0;
  bool wasPlaying_ = false;  // last tick's isPlaying(), for the playing->idle edge

  // Four rows, because five ran into the transport controls once a row
  // expanded to show "playing". The rest are reachable by paging rather than
  // simply absent — see RecorderService::list / MusicService::list.
  static constexpr size_t kMaxListed = 4;
  static constexpr uint8_t kMaxPages = 8;
  RecordingInfo recordings_[kMaxListed];
  size_t recordingCount_ = 0;
  size_t totalRecordings_ = 0;
  TrackInfo tracks_[kMaxListed];
  size_t trackCount_ = 0;
  size_t totalTracks_ = 0;
  // Sized to TrackInfo::name so a long music filename survives as a paging
  // anchor; recording names (<= 48) fit with room to spare.
  char pageAnchors_[kMaxPages][64] = {};
  uint8_t page_ = 0;
  int playingIndex_ = -1;

  widgets::Rect categoryRects_[4];
  widgets::Rect rowRects_[kMaxListed];
  widgets::Rect pauseRect_;
  widgets::Rect stopRect_;
  widgets::Rect volDownRect_;
  widgets::Rect volUpRect_;
};
