#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../feature_flags.h"

#if FEATURE_VIDEO

#include "../services/VideoService.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

class Arduino_Canvas;

// Video (design: docs/superpowers/specs/2026-07-30-video-playback-design.md).
// Two screens: a portrait library over /littlecube/video (folders one level
// deep, resume offers, battery warning), and a rotated-landscape player —
// the user turns the device sideways; frames are pre-rotated in the file,
// chrome is drawn landscape into a small canvas and transposed on.
class VideoApp : public App {
 public:
  explicit VideoApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override;
  void onResume() override;

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  enum class Screen {
    Library,
    // One screen, two modals, driven by three fields set in openItem():
    //   (battery ok, no resume)   -> screen skipped, plays immediately
    //   (battery ok, resume)      -> resume modal: confirm=resume, cancel=start over
    //   (battery low, no resume)  -> battery modal: confirm=play, cancel=library
    //   (battery low, resume)     -> battery modal, then resume modal on confirm
    ConfirmStart,
    Player,
  };

  void refreshList();
  void showPage(uint8_t page);
  void openItem(size_t index);
  void beginPlayback(uint32_t startMs);
  void stopAndSavePosition();
  void adoptExternalPlayback();
  void renderLibrary(Arduino_GFX& gfx);
  void renderConfirm(Arduino_GFX& gfx);
  void renderPlayer(Arduino_GFX& gfx);
  void renderChrome(Arduino_GFX& gfx);
  bool playerInput(const InputEvent& event);
  void formatMs(uint32_t ms, char* out, size_t len) const;

  static constexpr int16_t kChromeW = 448;  // landscape chrome canvas
  static constexpr int16_t kChromeH = 58;   // = the panel strip width
  static constexpr uint32_t kChromeHideMs = 4000;
  static constexpr uint32_t kSaveEveryMs = 5000;
  static constexpr int32_t kSeekStepMs = 15000;

  Services& services_;
  StatusBar statusBar_;
  Screen screen_ = Screen::Library;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  // Library, mirroring AudioApp's paging pattern.
  static constexpr size_t kMaxListed = 4;
  static constexpr uint8_t kMaxPages = 8;
  VideoInfo items_[kMaxListed];
  size_t itemCount_ = 0;
  size_t totalItems_ = 0;
  char pageAnchors_[kMaxPages][66] = {};
  uint8_t page_ = 0;
  char dir_[160] = "";  // current folder; paths::kVideo at the root
  widgets::Rect rowRects_[kMaxListed];

  // Pending start (ConfirmStart screen).
  char pendingPath_[160] = "";
  uint32_t pendingResumeMs_ = 0;
  bool batteryWarned_ = false;
  widgets::Rect confirmRect_;
  widgets::Rect cancelRect_;

  // Player chrome. The canvas is heavy (~52 KB) — allocated on open,
  // deleted on close per the App lifecycle contract.
  Arduino_Canvas* chrome_ = nullptr;
  bool chromeVisible_ = true;
  bool chromeDirty_ = true;
  uint32_t chromeMs_ = 0;
  uint32_t saveMs_ = 0;
  uint32_t posShownS_ = 0xFFFFFFFF;  // last second drawn, to redraw chrome 1 Hz
  bool wasPlaying_ = false;
  // Set when the prev/next chrome button queues a sibling episode; consumed
  // by update()'s playing->idle edge (Task 10) to start it without waiting
  // for a natural end. Declared now so Task 10 only touches the .cpp.
  bool nextQueued_ = false;
  // Chrome hit rects in PORTRAIT coordinates (the strip is vertical).
  widgets::Rect backRect_;
  widgets::Rect prevRect_;
  widgets::Rect playRect_;
  widgets::Rect nextRect_;
  widgets::Rect stopRect_;
  widgets::Rect scrubRect_;
};

#endif  // FEATURE_VIDEO
