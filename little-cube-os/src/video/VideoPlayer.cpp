#include "VideoPlayer.h"

#if FEATURE_VIDEO

#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <string.h>

#include <new>

#include "../board_config.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../services/SettingsService.h"
#include "../storage/SdStorage.h"

namespace {

void reason(char* out, size_t len, const char* msg) {
  if (out != nullptr && len > 0) {
    strncpy(out, msg, len - 1);
    out[len - 1] = '\0';
  }
}

// File-scope by choice, not just size (~17.5 KB): reaching the display from
// JPEGDEC's C-style draw callback needs either its pUser slot plus a cast, or
// a global anchor — with a single kernel-owned VideoPlayer the global
// (jpegTarget) is the simpler of the two. Decode runs on the loop task only.
//
// Allocated in MALLOC_CAP_INTERNAL by allocBuffers() and destroyed/freed by
// freeBuffers() — per-playback, not for the app's lifetime, so the ~17.5 KB
// of internal RAM is only held while a video is actually open. Internal RAM
// is exactly what TLS handshakes (Wi-Fi, weather, etc.) compete for; see
// AudioAdapter::releaseDriverIfIdle for the same trade made on the audio
// side.
JPEGDEC* jpegDecoder = nullptr;
Arduino_GFX* jpegTarget = nullptr;

int jpegDrawBlock(JPEGDRAW* d) {
  if (jpegTarget == nullptr) {
    return 0;
  }
  jpegTarget->draw16bitRGBBitmap(d->x, d->y, d->pPixels, d->iWidth, d->iHeight);
  return 1;
}

// Parameters for the rotating blit, set by decodeFrame() before each decode.
// Same file-scope-anchor reasoning as jpegTarget above: JPEGDEC's callback is
// a plain C function pointer with no user slot we already use.
struct RotateBlit {
  uint16_t* fb = nullptr;  // canvas framebuffer, DISPLAY_WIDTH stride
  int16_t dstX = 0;        // picture origin on the canvas
  int16_t dstY = 0;
  int16_t outW = 0;  // scaled picture size
  int16_t outH = 0;
  int16_t srcW = 0;  // stored frame size, as it sits in the file
  int16_t srcH = 0;
  bool clockwise = false;      // turn direction needed to reach the active mode
  const int16_t* mapDx = nullptr;  // dst x -> source coordinate on the srcH axis
  const int16_t* mapDy = nullptr;  // dst y -> source coordinate on the srcW axis
};
RotateBlit rotateBlit;

// Turn each decoded block 90 deg and nearest-neighbour scale it into the
// canvas. Derivation, writing S for the stored frame and D for what we draw:
//
//   ffmpeg transpose=1 (90 deg CW) is  R(x, y) = N(y, Hn-1-x).
//
// Undoing it (a Rotated file shown Upright) inverts that; applying it (an
// Upright file shown Rotated) uses it as-is. Both collapse to "one axis is a
// scale, the other is a mirrored scale", which is the `clockwise` flip below.
//
// Iteration is DESTINATION-major so canvas writes run along contiguous rows —
// the framebuffer is in PSRAM, where a strided write pattern is what actually
// costs. The strided side is the read, and it lands in d->pPixels: a block of
// up to 128x16 in internal RAM, which is cache-resident either way.
//
// The source coordinates come from per-playback lookup tables, never from
// arithmetic here. A divide in this loop runs ~76k times per frame.
int jpegDrawBlockRotate(JPEGDRAW* d) {
  const RotateBlit& r = rotateBlit;
  if (r.fb == nullptr || r.mapDx == nullptr || r.mapDy == nullptr || r.outW <= 0 ||
      r.outH <= 0) {
    return 0;
  }
  // iWidth is the BUFFER PITCH; iWidthUsed is how much of it is real picture.
  // They differ on the right-hand strip of every frame whose width is not a
  // multiple of the pitch (252 is not: the second strip carries 124 real
  // columns in a 128-wide buffer). Bounding by iWidth would pull four columns
  // of MCU padding into the picture — harmless-looking in the axis-aligned
  // blit, but a stripe straight across a turned one. iHeight needs no
  // equivalent: jpeg.inl trims it in place for the last row.
  const int32_t bx0 = d->x;
  const int32_t bx1 = d->x + d->iWidthUsed;
  const int32_t by0 = d->y;
  const int32_t by1 = d->y + d->iHeight;

  // Bound the destination rect this block can touch. Deliberately loose by a
  // pixel each way: the per-pixel guards below are what enforce correctness,
  // so this only has to avoid scanning the whole picture per block.
  int32_t dx0;
  int32_t dx1;
  int32_t dy0;
  int32_t dy1;
  if (r.clockwise) {
    dy0 = bx0 * r.outH / r.srcW - 1;
    dy1 = bx1 * r.outH / r.srcW + 1;
    dx0 = (r.srcH - by1) * r.outW / r.srcH - 1;
    dx1 = (r.srcH - by0) * r.outW / r.srcH + 1;
  } else {
    dy0 = (r.srcW - bx1) * r.outH / r.srcW - 1;
    dy1 = (r.srcW - bx0) * r.outH / r.srcW + 1;
    dx0 = by0 * r.outW / r.srcH - 1;
    dx1 = by1 * r.outW / r.srcH + 1;
  }
  if (dx0 < 0) {
    dx0 = 0;
  }
  if (dy0 < 0) {
    dy0 = 0;
  }
  if (dx1 > r.outW) {
    dx1 = r.outW;
  }
  if (dy1 > r.outH) {
    dy1 = r.outH;
  }

  const int32_t pitch = d->iWidth;  // buffer stride, NOT the valid width
  for (int32_t dy = dy0; dy < dy1; dy++) {
    const int32_t scaled = r.mapDy[dy];
    const int32_t sx = r.clockwise ? scaled : r.srcW - 1 - scaled;
    if (sx < bx0 || sx >= bx1) {
      continue;  // per row, not per pixel
    }
    uint16_t* dstRow = r.fb + static_cast<int32_t>(r.dstY + dy) * DISPLAY_WIDTH + r.dstX;
    const uint16_t* srcCol = d->pPixels + (sx - bx0);
    for (int32_t dx = dx0; dx < dx1; dx++) {
      const int32_t scaledX = r.mapDx[dx];
      const int32_t sy = r.clockwise ? r.srcH - 1 - scaledX : scaledX;
      if (sy < by0 || sy >= by1) {
        continue;
      }
      dstRow[dx] = srcCol[(sy - by0) * pitch];
    }
  }
  return 1;
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

void VideoPlayer::begin(AudioAdapter* audio, DisplayAdapter* display, SdStorage* storage,
                        SettingsService* settings) {
  audio_ = audio;
  display_ = display;
  storage_ = storage;
  settings_ = settings;
  done_ = xSemaphoreCreateBinary();
}

// Resolve the picture rect once per playback. Two independent facts feed in:
// how the file was packed (header_.orientation) and how the user wants to
// watch (the setting). When they agree the frame is already the right shape
// and lands 1:1; when they disagree it is turned 90 deg and scaled down to
// fit during decode, which is what keeps files packed before this existed
// playable in either mode.
void VideoPlayer::resolveLayout() {
  activeOrientation_ =
      settings_ != nullptr ? settings_->videoOrientation() : VideoOrientation::Rotated;
  layout_.rotate = header_.orientation != activeOrientation_;

  const int16_t boxW = pictureBoxW(activeOrientation_);
  const int16_t boxH = pictureBoxH(activeOrientation_);
  // Turning the frame swaps its axes, so that is the size we have to fit.
  const int32_t natW = layout_.rotate ? header_.height : header_.width;
  const int32_t natH = layout_.rotate ? header_.width : header_.height;

  // Fit, never fill and never upscale: the whole frame stays visible and a
  // frame already sized for this box (the 1:1 case) is left exactly alone.
  if (natW <= boxW && natH <= boxH) {
    layout_.w = static_cast<int16_t>(natW);
    layout_.h = static_cast<int16_t>(natH);
  } else if (natW * boxH >= natH * boxW) {
    layout_.w = boxW;  // width-bound
    layout_.h = static_cast<int16_t>(natH * boxW / natW);
  } else {
    layout_.h = boxH;  // height-bound
    layout_.w = static_cast<int16_t>(natW * boxH / natH);
  }
  layout_.x = static_cast<int16_t>((boxW - layout_.w) / 2);
  layout_.y = static_cast<int16_t>((boxH - layout_.h) / 2);
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
  // Internal RAM, not PSRAM: JPEGDEC's working state, not frame data. See the
  // file-scope comment on jpegDecoder for why this is allocated here instead
  // of living for the app's lifetime.
  void* jpegMem = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (jpegMem == nullptr) {
    freeBuffers();
    return false;
  }
  jpegDecoder = new (jpegMem) JPEGDEC();
  // Nearest-neighbour source coordinates, one entry per destination pixel on
  // each axis. Without these the rotating blit does an integer DIVIDE per
  // destination pixel — ~76k of them per frame for a 16:9 file, which on this
  // core is milliseconds the 66 ms frame budget cannot spare. The mapping
  // depends only on the layout, not on the block, so it is computed once per
  // playback. ~1.6 KB of internal RAM, and only when a rotation is actually
  // happening.
  if (layout_.rotate && layout_.w > 0 && layout_.h > 0) {
    rotMapDx_ = static_cast<int16_t*>(heap_caps_malloc(
        static_cast<size_t>(layout_.w) * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    rotMapDy_ = static_cast<int16_t*>(heap_caps_malloc(
        static_cast<size_t>(layout_.h) * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (rotMapDx_ == nullptr || rotMapDy_ == nullptr) {
      freeBuffers();
      return false;
    }
    for (int16_t dx = 0; dx < layout_.w; dx++) {
      rotMapDx_[dx] = static_cast<int16_t>(static_cast<int32_t>(dx) * header_.height / layout_.w);
    }
    for (int16_t dy = 0; dy < layout_.h; dy++) {
      rotMapDy_[dy] = static_cast<int16_t>(static_cast<int32_t>(dy) * header_.width / layout_.h);
    }
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
  heap_caps_free(rotMapDx_);
  rotMapDx_ = nullptr;
  heap_caps_free(rotMapDy_);
  rotMapDy_ = nullptr;
  if (jpegDecoder != nullptr) {
    jpegDecoder->~JPEGDEC();
    heap_caps_free(jpegDecoder);
    jpegDecoder = nullptr;
  }
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
  resolveLayout();
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
  lastError_[0] = '\0';  // a fresh play() outruns any stale failure
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
  // Defensive: nothing should still expect UI-driven decode once idle.
  // beginPlayback()/adoptExternalPlayback() re-arm this via setUiActive(true)
  // on the next play().
  uiActive_ = false;
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
        reason(lastError_, sizeof(lastError_), "too many undecodable frames");
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
  if (jpegDecoder == nullptr) {
    return false;  // buffers not (yet) allocated — should not happen while Playing
  }
  Arduino_GFX* gfx = display_->canvas();
  if (gfx == nullptr) {
    return false;
  }
  jpegTarget = gfx;
  // The rotating path writes into the framebuffer directly, so it needs one
  // to exist; in the degraded direct-draw mode (canvas allocation failed) we
  // fall back to the plain blit and the picture comes out sideways. A sideways
  // picture beats a black screen, and it is already the "no canvas" story for
  // the chrome, which cannot transpose either.
  const bool rotating =
      layout_.rotate && display_->hasCanvas() && rotMapDx_ != nullptr && rotMapDy_ != nullptr;
  int16_t offsetX = layout_.x;
  int16_t offsetY = layout_.y;
  if (rotating) {
    // Fully configured BEFORE openRAM, so the callback can never observe a
    // half-built descriptor.
    rotateBlit.fb = static_cast<Arduino_Canvas*>(gfx)->getFramebuffer();
    rotateBlit.dstX = layout_.x;
    rotateBlit.dstY = layout_.y;
    rotateBlit.outW = layout_.w;
    rotateBlit.outH = layout_.h;
    rotateBlit.srcW = static_cast<int16_t>(header_.width);
    rotateBlit.srcH = static_cast<int16_t>(header_.height);
    rotateBlit.mapDx = rotMapDx_;
    rotateBlit.mapDy = rotMapDy_;
    // An Upright file shown Rotated needs the packer's 90 deg CW applied; a
    // Rotated file shown Upright needs it undone.
    rotateBlit.clockwise = header_.orientation == VideoOrientation::Upright;
    // The callback computes its own destination, so the decoder must hand it
    // unshifted source coordinates.
    offsetX = 0;
    offsetY = 0;
  } else if (layout_.rotate) {
    // No canvas: center the stored frame as-is rather than at a rect sized
    // for the turned picture, which would push it off the panel.
    offsetX = static_cast<int16_t>((pictureBoxW(activeOrientation_) - header_.width) / 2);
    offsetY = static_cast<int16_t>((pictureBoxH(activeOrientation_) - header_.height) / 2);
    if (offsetX < 0) {
      offsetX = 0;
    }
    if (offsetY < 0) {
      offsetY = 0;
    }
  }
  if (!jpegDecoder->openRAM(slots_[slot], static_cast<int>(slotBytes_[slot]),
                            rotating ? jpegDrawBlockRotate : jpegDrawBlock)) {
    return false;
  }
  // If colors come out wrong on device, switch to RGB565_BIG_ENDIAN — the
  // canvas framebuffer byte order is the only open question here.
  jpegDecoder->setPixelType(RGB565_LITTLE_ENDIAN);
  const int ok = jpegDecoder->decode(offsetX, offsetY, 0);
  jpegDecoder->close();
  if (ok != 1) {
    return false;
  }
  display_->markDirty();
  return true;
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
          reason(lastError_, sizeof(lastError_), "read failed — card removed or file corrupt");
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
