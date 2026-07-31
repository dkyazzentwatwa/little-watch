#pragma once

#include <Arduino.h>

#include "../feature_flags.h"

#if FEATURE_VIDEO

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "LcvReader.h"

class AudioAdapter;
class DisplayAdapter;
class SdStorage;

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

  void begin(AudioAdapter* audio, DisplayAdapter* display, SdStorage* storage);

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
  // True when the last playback reached the end of the file on its own.
  // Valid on the playing->idle edge; a requested stop clears it.
  bool completed() const { return completed_; }

  uint32_t positionMs() const;
  uint32_t durationMs() const { return header_.durationMs; }
  const char* path() const { return path_; }
  const LcvHeader& header() const { return header_; }

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

  bool startTask(uint32_t startFrame);
  void finishPlayback();
  void consumeFrames();
  bool decodeFrame(uint8_t slot);  // Task 6 fills this in with JPEGDEC
  bool allocBuffers();
  void freeBuffers();
  uint32_t clockSamples() const;  // audio clock minus the in-flight DMA estimate

  static constexpr uint32_t kDmaDepthSamples = 4096;  // I2S depth estimate; tune on device
  static constexpr uint8_t kMaxConsecutiveBad = 15;

  AudioAdapter* audio_ = nullptr;
  DisplayAdapter* display_ = nullptr;
  SdStorage* storage_ = nullptr;

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

  volatile bool stopReq_ = false;
  volatile bool taskEof_ = false;
  volatile bool taskFailed_ = false;
  bool taskDone_ = false;  // loop task: reader exited, ring may still drain
  bool taskRunning_ = false;

  bool paused_ = false;
  bool completed_ = false;
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
