#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../feature_flags.h"

#if FEATURE_VIDEO

#include "../services/VideoService.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"
#include "../video/VideoPlayer.h"

class Arduino_Canvas;

// Video (design: docs/superpowers/specs/2026-07-30-video-playback-design.md).
// Two screens: a portrait library over /littlecube/video (folders one level
// deep, resume offers, battery warning), and a player that comes in two
// orientations, chosen by the `videoOrient` setting:
//
//   Rotated — the user turns the device sideways. Frames are pre-rotated in
//     the file, chrome is drawn landscape into a small canvas and transposed
//     onto a vertical strip, and the gesture map is inverted to match.
//   Upright — the cube worn on a wrist, which cannot be turned. The picture
//     is fit to width and letterboxed, chrome is a horizontal strip along the
//     bottom drawn straight into the frame canvas, and gestures are natural.
//
// Everything orientation-dependent branches on uprightMode(); the two paths
// share the library, the confirm modal and all playback control.
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
  // What a swipe means, once the orientation has been taken out of it. The
  // two maps are 90 degrees apart, which is exactly the device rotation the
  // Rotated mode asks the user to perform.
  enum class Gesture : uint8_t { None, SeekForward, SeekBack, VolumeUp, VolumeDown };

  void renderPlayer(Arduino_GFX& gfx);
  void renderChrome(Arduino_GFX& gfx);
  void renderChromeUpright(Arduino_GFX& gfx);
  bool playerInput(const InputEvent& event);
  void formatMs(uint32_t ms, char* out, size_t len) const;
  // Readout shared by both chrome layouts: "vol NN%" while a volume change is
  // still showing, otherwise "position / duration - battery%".
  void formatReadout(char* out, size_t len) const;
  void stepVolume(bool up);
  void syncChromeBuffer();
  bool uprightMode() const;
  Gesture gestureFor(InputAction action) const;

  static constexpr int16_t kChromeW = 448;  // landscape chrome canvas
  static constexpr int16_t kChromeH = VideoPlayer::kChromeStripPx;  // = the panel strip width
  // Upright chrome needs two rows: six 48px buttons already span the panel's
  // full safe width, leaving nothing beside them for a scrub bar the way the
  // 448px landscape strip has.
  static constexpr int16_t kUprightChromeH = kVideoUprightChromeH;  // 92
  static constexpr uint32_t kChromeHideMs = 4000;
  static constexpr uint32_t kSaveEveryMs = 5000;
  static constexpr int32_t kSeekStepMs = 15000;
  static constexpr uint32_t kToastMs = 2500;

  Services& services_;
  StatusBar statusBar_;
  Screen screen_ = Screen::Library;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  // Refusal-reason toast, shown over the library (mirrors FilesApp's
  // toast_/toastMs_ pattern).
  char toast_[48] = "";
  uint32_t toastMs_ = 0;

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

  // Player chrome. The canvas is heavy (~52 KB) and exists only to draw
  // landscape text that then gets transposed, so it is allocated lazily by
  // the Rotated path and never at all in Upright mode, which draws straight
  // into the frame canvas. Deleted on close per the App lifecycle contract.
  Arduino_Canvas* chrome_ = nullptr;
  bool chromeVisible_ = true;
  bool chromeDirty_ = true;
  uint32_t chromeMs_ = 0;
  uint32_t saveMs_ = 0;
  uint32_t posShownS_ = 0xFFFFFFFF;  // last second drawn, to redraw chrome 1 Hz
  bool wasPlaying_ = false;
  // VideoPlayer::positionMs() reads 0 once state_ is Idle, and
  // videoPlayer.update() runs before appRouter.update() in the kernel loop,
  // so a stop can already be Idle by the tick update() observes the
  // playing->idle edge. Refreshed every Player-screen tick while playing, so
  // it always holds the last position seen before that happens.
  uint32_t lastKnownPosMs_ = 0;
  // Set when the prev/next chrome button queues a sibling episode; consumed
  // by update()'s playing->idle edge (Task 10) to start it without waiting
  // for a natural end. Declared now so Task 10 only touches the .cpp.
  bool nextQueued_ = false;
  // Chrome hit rects, always in PORTRAIT coordinates. Rotated has to convert
  // (its strip is vertical, so it stores them by hand); Upright gets them
  // that way straight out of widgets::button().
  widgets::Rect backRect_;
  widgets::Rect prevRect_;
  widgets::Rect playRect_;
  widgets::Rect nextRect_;
  widgets::Rect volDownRect_;
  widgets::Rect volUpRect_;
  widgets::Rect scrubRect_;

  // Volume readout: when > 0, renderChrome() shows "vol NN%" in place of the
  // normal time/battery readout, counting down to 0 via update().
  uint32_t volShownMs_ = 0;
  static constexpr uint32_t kVolShowMs = 1500;
};

#endif  // FEATURE_VIDEO
