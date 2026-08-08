#pragma once

#include <Arduino.h>

#include "../feature_flags.h"

#if FEATURE_VIDEO

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../board_config.h"
#include "LcvReader.h"

class AudioAdapter;
class DisplayAdapter;
class SdStorage;
class SettingsService;

// Playback engine. One reader task streams SD -> PSRAM frame ring + I2S
// audio; the loop task decodes JPEG frames chasing the audio clock.
//
// Task lifecycle contract — identical to AudioAdapter's workers:
//   * state_ is written by the LOOP TASK ONLY. It leaves a non-Idle state
//     only in update(), and only by taking done_.
//   * The reader task's last two statements are xSemaphoreGive(done_) then
//     vTaskDelete(nullptr). Nothing touches `self` after the give.
//   * Stops and seeks are REQUESTS; the task exits itself and is never
//     killed from outside.
//
// This class is also the SOLE owner of its audio stream: nothing else —
// not stopPlayback(), not the sleep timer — ends a PCM stream. Every exit
// path here must reach endPcmStream().
class VideoPlayer {
 public:
  // SPSC ring capacity. MUST divide 256: head_/tail_ are free-running uint8
  // indices, so slot = index % kRingSlots is injective across the uint8 wrap
  // only when kRingSlots divides 2^8. (5 aliased two live entries every 256
  // pushes — one clobbered frame per ~17 s.) Public because the reader task
  // (a friend free function) sizes its wait loop against it.
  static constexpr uint8_t kRingSlots = 4;

  // The player chrome occupies a strip on the panel's right edge (portrait
  // space); frames are centered in what remains so the picture and chrome
  // never overlap. VideoApp::kChromeH must equal kChromeStripPx. The values
  // themselves live in board_config.h ("video player layout") so LcvReader
  // — the lower layer, which VideoPlayer includes but must not be included
  // by — can validate stored frame size without depending on this class.
  static constexpr int16_t kChromeStripPx = kVideoChromeStripPx;
  static constexpr int16_t kPictureAreaW = kVideoPictureAreaW;  // 310

  // Where the picture lands on the canvas, and how it gets there. Resolved
  // once per playback in play(), from the file's stored orientation and the
  // user's setting; VideoApp reads it to blank the letterbox margins and to
  // place its chrome. `rotate` is the compatibility path: the two disagree,
  // so every frame is turned 90 deg (and scaled to fit) during decode.
  struct PictureLayout {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
    bool rotate = false;
  };

  // `settings` supplies the orientation, sampled at each play() so a change
  // mid-video lands on the next start rather than re-laying-out under a
  // running decoder. May be null (serial-only builds); Rotated is assumed.
  void begin(AudioAdapter* audio, DisplayAdapter* display, SdStorage* storage,
             SettingsService* settings);

  // Starts playback (loop task only). Refuses — writing a short reason —
  // when already playing, recording is active, the path fails sanitizing,
  // or the header does not validate. startMs is rounded down to a frame.
  bool play(const char* path, uint32_t startMs, char* reasonOut, size_t reasonLen);
  void requestStop();                 // user stop; never counts as completed
  void requestSeek(int32_t deltaMs);  // relative, clamped to [0, duration)
  void setPaused(bool paused);
  bool paused() const { return paused_; }
  bool playing() const { return state_ != State::Idle; }
  bool idle() const { return state_ == State::Idle; }
  // True while a requested stop/seek is unwinding: the reader task is still
  // exiting, so playing() holds but this playback is already condemned.
  // Adoption logic uses this to never adopt a dying playback.
  bool stopping() const { return state_ == State::Stopping; }
  // True when the last playback reached the end of the file on its own.
  // Valid on the playing->idle edge; a requested stop clears it.
  bool completed() const { return completed_; }

  // Human-readable cause of an abnormal end (card yank, decode desync).
  // Valid on the playing->idle edge after an abnormal end; cleared by
  // play(). Empty for a user/serial stop or a natural completion.
  const char* lastError() const { return lastError_; }

  uint32_t positionMs() const;
  uint32_t durationMs() const { return header_.durationMs; }
  const char* path() const { return path_; }
  const LcvHeader& header() const { return header_; }
  const PictureLayout& pictureLayout() const { return layout_; }
  VideoOrientation activeOrientation() const { return activeOrientation_; }

  // Picture box for a mode: what the frame must fit inside once the chrome
  // strip is taken out. Static so VideoApp can lay out chrome against the
  // same numbers without a live playback.
  static int16_t pictureBoxW(VideoOrientation mode) {
    return mode == VideoOrientation::Upright ? DISPLAY_WIDTH : kVideoPictureAreaW;
  }
  static int16_t pictureBoxH(VideoOrientation mode) {
    return mode == VideoOrientation::Upright ? kVideoUprightPictureH : DISPLAY_HEIGHT;
  }

  // The app owns the screen; decode only happens while it says so. When
  // false, frames are still popped and dropped on the clock so audio and
  // position stay correct.
  void setUiActive(bool active) { uiActive_ = active; }

  // Diagnostics for `video status` and the chrome.
  uint32_t framesShown() const { return framesShown_; }
  uint32_t framesDropped() const { return framesDropped_; }
  uint8_t ringDepth() const { return static_cast<uint8_t>(head_ - tail_); }

  void update(uint32_t deltaMs);  // kernel slot: state machine + decode

 private:
  enum class State : uint8_t { Idle, Playing, Stopping };

  friend void videoReaderTask(void* arg);

  void resolveLayout();  // fills layout_/activeOrientation_ from header_ + settings
  bool startTask(uint32_t startFrame);
  void finishPlayback();
  void consumeFrames();
  bool decodeFrame(uint8_t slot);  // JPEGDEC decode into the frame canvas (loop task only)
  bool allocBuffers();
  void freeBuffers();
  uint32_t clockSamples() const;  // audio clock minus the in-flight DMA estimate

  // I2S depth estimate; tune on device via the lip-sync item in
  // docs/hardware-validation.md §Video (lands with the final plan task).
  // INVARIANT: must stay <= kRingSlots * (audioRateHz / fps) (= 5880 at 4
  // slots, 22050/15) — above that the reader parks on a full ring before the
  // clock can reach the tail frame and playback hard-stalls. Raise
  // kRingSlots (power dividing 256) before raising this.
  static constexpr uint32_t kDmaDepthSamples = 4096;
  static constexpr uint8_t kMaxConsecutiveBad = 15;

  AudioAdapter* audio_ = nullptr;
  DisplayAdapter* display_ = nullptr;
  SdStorage* storage_ = nullptr;
  SettingsService* settings_ = nullptr;

  VideoOrientation activeOrientation_ = VideoOrientation::Rotated;
  PictureLayout layout_;

  State state_ = State::Idle;  // loop task only
  SemaphoreHandle_t done_ = nullptr;
  LcvReader reader_;           // instance methods used by the reader task only
  LcvHeader header_;
  char path_[160] = "";

  // SPSC ring: the reader task pushes (head_), the loop task pops (tail_).
  // Free-running uint8 indices; slot = index % kRingSlots.
  uint8_t* slots_[kRingSlots] = {nullptr};
  volatile uint32_t slotBytes_[kRingSlots] = {0};
  volatile uint32_t slotFrame_[kRingSlots] = {0};
  volatile uint8_t head_ = 0;
  volatile uint8_t tail_ = 0;
  int16_t* audioBuf_ = nullptr;  // one frame group of samples, PSRAM
  uint32_t audioBufBytes_ = 0;
  // Nearest-neighbour source coordinate per destination pixel, one table per
  // axis (layout_.w and layout_.h entries). Internal RAM, allocated only when
  // layout_.rotate — they exist purely to keep integer division out of the
  // blit's inner loop. See allocBuffers().
  int16_t* rotMapDx_ = nullptr;
  int16_t* rotMapDy_ = nullptr;

  volatile bool stopReq_ = false;
  volatile bool taskEof_ = false;
  volatile bool taskFailed_ = false;
  // taskRunning_: a reader task exists whose done_ give has not been reaped.
  // taskDone_: reaped from Playing with the ring still draining. Distinct on
  // purpose — Stopping resolves immediately when taskRunning_ is already
  // false, which is exactly the case a "simplification" merging these two
  // flags would break (the stuck-Stopping bug).
  bool taskDone_ = false;
  bool taskRunning_ = false;

  bool paused_ = false;
  bool completed_ = false;
  char lastError_[48] = "";
  volatile bool uiActive_ = false;
  bool seekPending_ = false;
  uint32_t seekTargetMs_ = 0;
  uint32_t taskStartFrame_ = 0;
  uint32_t baseFrame_ = 0;  // frame the current audio clock epoch started at

  uint32_t framesShown_ = 0;
  uint32_t framesDropped_ = 0;
  uint8_t consecutiveBad_ = 0;
};

#endif  // FEATURE_VIDEO
