#include "VideoPlayer.h"

#if FEATURE_VIDEO

#include <esp_heap_caps.h>
#include <string.h>

#include "../board_config.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../storage/SdStorage.h"

namespace {

void reason(char* out, size_t len, const char* msg) {
  if (out != nullptr && len > 0) {
    strncpy(out, msg, len - 1);
    out[len - 1] = '\0';
  }
}

}  // namespace

// Reader task: stream frame groups from SD into the ring + I2S. Runs on
// core 0 so decode/render on the loop task never waits behind SD I/O.
void videoReaderTask(void* arg) {
  auto* self = static_cast<VideoPlayer*>(arg);
  LcvReader& r = self->reader_;
  uint32_t frame = self->taskStartFrame_;
  bool failed = false;

  while (!self->stopReq_) {
    LcvReader::ChunkType type;
    uint32_t size = 0;
    if (!r.nextChunk(type, size)) {
      failed = !r.endOfData();
      break;
    }
    if (type == LcvReader::ChunkType::Video) {
      if (size == 0 || size > self->header_.maxFrameBytes) {
        failed = true;  // size lies about the header contract: desynced
        break;
      }
      // Wait for a free slot; the ring being full is the normal steady state.
      while (static_cast<uint8_t>(self->head_ - self->tail_) >= VideoPlayer::kRingSlots &&
             !self->stopReq_) {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      if (self->stopReq_) {
        break;
      }
      const uint8_t s = self->head_ % VideoPlayer::kRingSlots;
      if (!r.readChunk(self->slots_[s], size)) {
        failed = true;
        break;
      }
      self->slotBytes_[s] = size;
      self->slotFrame_[s] = frame;
      self->head_ = self->head_ + 1;
      frame++;
    } else {  // Audio
      // maxFrameBytes bounds video only; audio is bounded by OUR buffer.
      if (size == 0 || size > self->audioBufBytes_) {
        failed = true;
        break;
      }
      if (!r.readChunk(reinterpret_cast<uint8_t*>(self->audioBuf_), size)) {
        failed = true;
        break;
      }
      // Blocks on DMA backpressure — that backpressure is the pacing.
      self->audio_->writePcm(self->audioBuf_, size / sizeof(int16_t));
    }
  }

  self->taskEof_ = r.endOfData() && !failed;
  self->taskFailed_ = failed;
  xSemaphoreGive(self->done_);
  vTaskDelete(nullptr);
}

void VideoPlayer::begin(AudioAdapter* audio, DisplayAdapter* display, SdStorage* storage) {
  audio_ = audio;
  display_ = display;
  storage_ = storage;
  done_ = xSemaphoreCreateBinary();
}

bool VideoPlayer::allocBuffers() {
  for (uint8_t i = 0; i < kRingSlots; i++) {
    slots_[i] = static_cast<uint8_t*>(
        heap_caps_malloc(header_.maxFrameBytes, MALLOC_CAP_SPIRAM));
    if (slots_[i] == nullptr) {
      freeBuffers();
      return false;
    }
  }
  // One frame group of audio, with headroom for a short final group.
  audioBufBytes_ = (header_.audioRateHz / header_.fps) * sizeof(int16_t) + 64;
  audioBuf_ = static_cast<int16_t*>(heap_caps_malloc(audioBufBytes_, MALLOC_CAP_SPIRAM));
  if (audioBuf_ == nullptr) {
    freeBuffers();
    return false;
  }
  return true;
}

void VideoPlayer::freeBuffers() {
  for (uint8_t i = 0; i < kRingSlots; i++) {
    heap_caps_free(slots_[i]);
    slots_[i] = nullptr;
  }
  heap_caps_free(audioBuf_);
  audioBuf_ = nullptr;
  audioBufBytes_ = 0;
}

bool VideoPlayer::startTask(uint32_t startFrame) {
  head_ = 0;
  tail_ = 0;
  stopReq_ = false;
  taskEof_ = false;
  taskFailed_ = false;
  taskDone_ = false;
  taskStartFrame_ = startFrame;
  baseFrame_ = startFrame;
  if (startFrame > 0 && !reader_.seekToFrame(startFrame)) {
    return false;
  }
  xSemaphoreTake(done_, 0);  // drain any stale give, mirroring AudioAdapter's starts
  // Priority 3, above audioRecordTask's 2: safe because record and video are
  // mutually exclusive by the half-duplex gate, and this task feeds the audio
  // DMA — starving it means audible underrun, the one failure the design
  // forbids.
  if (xTaskCreatePinnedToCore(videoReaderTask, "vidread", 6144, this, 3, nullptr, 0) !=
      pdPASS) {
    return false;
  }
  taskRunning_ = true;
  return true;
}

bool VideoPlayer::play(const char* path, uint32_t startMs, char* reasonOut, size_t reasonLen) {
  if (state_ != State::Idle) {
    reason(reasonOut, reasonLen, "already playing");
    return false;
  }
  if (audio_ == nullptr || audio_->isRecording()) {
    reason(reasonOut, reasonLen, "recording in progress");
    return false;
  }
  String safe;
  if (storage_ == nullptr || !storage_->sanitizePath(path, safe)) {
    reason(reasonOut, reasonLen, "path refused");
    return false;
  }
  if (!reader_.open(safe.c_str(), reasonOut, reasonLen)) {
    return false;
  }
  header_ = reader_.header();
  if (!allocBuffers()) {
    reader_.close();
    reason(reasonOut, reasonLen, "out of memory");
    return false;
  }
  if (!audio_->beginPcmStream(header_.audioRateHz, 1)) {
    freeBuffers();
    reader_.close();
    reason(reasonOut, reasonLen, "audio busy");
    return false;
  }
  uint32_t startFrame = static_cast<uint32_t>(
      static_cast<uint64_t>(startMs) * header_.fps / 1000);
  if (startFrame >= header_.frameCount) {
    startFrame = 0;
  }
  if (!startTask(startFrame)) {
    audio_->endPcmStream();
    freeBuffers();
    reader_.close();
    reason(reasonOut, reasonLen, "cannot start reader");
    return false;
  }
  strncpy(path_, safe.c_str(), sizeof(path_) - 1);
  path_[sizeof(path_) - 1] = '\0';
  paused_ = false;
  completed_ = false;
  framesShown_ = 0;
  framesDropped_ = 0;
  consecutiveBad_ = 0;
  state_ = State::Playing;
  return true;
}

void VideoPlayer::requestStop() {
  if (state_ != State::Playing) {
    return;
  }
  seekPending_ = false;
  stopReq_ = true;
  if (paused_) {
    setPaused(false);  // un-park writePcm so the task can reach its exit
  }
  state_ = State::Stopping;
}

void VideoPlayer::requestSeek(int32_t deltaMs) {
  if (state_ != State::Playing) {
    return;
  }
  const int64_t target = static_cast<int64_t>(positionMs()) + deltaMs;
  seekTargetMs_ = target < 0 ? 0
                : target >= header_.durationMs ? header_.durationMs - 1
                                               : static_cast<uint32_t>(target);
  seekPending_ = true;
  stopReq_ = true;
  if (paused_) {
    setPaused(false);
  }
  state_ = State::Stopping;
}

void VideoPlayer::setPaused(bool paused) {
  if (state_ != State::Playing && !(state_ == State::Stopping && paused_)) {
    return;
  }
  paused_ = paused;
  audio_->pcmPause(paused);
}

uint32_t VideoPlayer::clockSamples() const {
  const uint32_t played = audio_->pcmSamplesPlayed();
  // Once the reader has exited, writePcm no longer advances the counter while
  // the DMA drains in real time; keeping the correction would freeze target
  // ~3 frames short of the end and the ring would never drain (EOF hang).
  const uint32_t dma =
      taskDone_ ? 0 : (played > kDmaDepthSamples ? kDmaDepthSamples : played);
  return played - dma;
}

uint32_t VideoPlayer::positionMs() const {
  if (state_ == State::Idle) {
    return 0;
  }
  const uint64_t baseMs = static_cast<uint64_t>(baseFrame_) * 1000 / header_.fps;
  const uint64_t audioMs =
      static_cast<uint64_t>(clockSamples()) * 1000 / header_.audioRateHz;
  const uint64_t pos = baseMs + audioMs;
  return pos > header_.durationMs ? header_.durationMs : static_cast<uint32_t>(pos);
}

// NOTE: endPcmStream() never flushes the I2S DMA, so up to ~370 ms of this
// episode's audio tail keeps draining after completion — an auto-advance that
// starts the next file immediately splices in behind it, same class of
// limitation as the documented seek splice. Accepted for v1.
void VideoPlayer::finishPlayback() {
  if (paused_) {
    paused_ = false;
    audio_->pcmPause(false);  // never hand the next player a muted codec
  }
  audio_->endPcmStream();
  reader_.close();
  freeBuffers();
  state_ = State::Idle;
}

void VideoPlayer::consumeFrames() {
  if (paused_ || static_cast<uint8_t>(head_ - tail_) == 0) {
    return;
  }
  // Audio is the master clock; subtract the DMA depth estimate so the frame
  // on screen matches what the speaker is saying, not what was buffered.
  const uint32_t target =
      baseFrame_ + static_cast<uint32_t>(static_cast<uint64_t>(clockSamples()) *
                                         header_.fps / header_.audioRateHz);
  // Drop everything older than the clock, keeping at least the newest.
  while (static_cast<uint8_t>(head_ - tail_) > 1 &&
         slotFrame_[tail_ % kRingSlots] < target) {
    tail_ = tail_ + 1;
    framesDropped_++;
  }
  const uint8_t s = tail_ % kRingSlots;
  if (slotFrame_[s] > target) {
    return;  // newest frame is still in the future; hold the current image
  }
  // The panel flush is capped ~30 fps; decoding while a flush is pending
  // would draw a frame that is overwritten before it is ever seen.
  if (uiActive_ && display_ != nullptr && !display_->flushPending()) {
    if (decodeFrame(s)) {
      framesShown_++;
      consecutiveBad_ = 0;
    } else {
      consecutiveBad_++;
      if (consecutiveBad_ >= kMaxConsecutiveBad) {
        requestStop();
        return;
      }
    }
    tail_ = tail_ + 1;
  } else if (!uiActive_) {
    tail_ = tail_ + 1;  // no screen: keep position/audio honest, drop video
    framesDropped_++;
  }
}

bool VideoPlayer::decodeFrame(uint8_t slot) {
  (void)slot;
  return true;  // Task 6 replaces this with the JPEGDEC decode
}

void VideoPlayer::update(uint32_t deltaMs) {
  (void)deltaMs;
  switch (state_) {
    case State::Idle:
      return;
    case State::Playing:
      if (taskRunning_ && xSemaphoreTake(done_, 0) == pdTRUE) {
        taskRunning_ = false;
        taskDone_ = true;
        if (taskFailed_) {
          // Card yanked or file corrupt mid-play: stop now, not completed.
          completed_ = false;
          finishPlayback();
          return;
        }
      }
      consumeFrames();
      if (taskDone_ && static_cast<uint8_t>(head_ - tail_) == 0) {
        completed_ = taskEof_;  // natural end: the ring drained after EOF
        finishPlayback();
      }
      return;
    case State::Stopping: {
      if (taskRunning_) {
        if (xSemaphoreTake(done_, 0) != pdTRUE) {
          return;  // reader still unwinding; check again next tick
        }
        taskRunning_ = false;
      }
      // Reader is gone (just reaped, or it had already exited before the
      // stop/seek request — the post-EOF drain window). Resolve now.
      completed_ = false;
      if (seekPending_) {
        // Restart at the seek target, new audio-clock epoch.
        seekPending_ = false;
        audio_->endPcmStream();
        // Known v1 limitation: up to ~370 ms of pre-seek audio still sits in
        // the I2S DMA and plays across the splice — ESP_I2S has no flush, and
        // re-initing the driver here would click. Accepted.
        uint32_t frame = static_cast<uint32_t>(
            static_cast<uint64_t>(seekTargetMs_) * header_.fps / 1000);
        if (frame >= header_.frameCount) {
          frame = header_.frameCount - 1;
        }
        if (audio_->beginPcmStream(header_.audioRateHz, 1) && startTask(frame)) {
          state_ = State::Playing;
        } else {
          finishPlayback();
        }
      } else {
        finishPlayback();
      }
      return;
    }
  }
}

#endif  // FEATURE_VIDEO
