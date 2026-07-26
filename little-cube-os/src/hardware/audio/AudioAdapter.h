#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ES8311 codec + in-core ESP_I2S. The codec is I2S slave with MCLK = 256 x
// sample rate, 16-bit, onboard analog mic; the power amp enables via GPIO.
// Everything long-running happens on small FreeRTOS tasks: recording
// streams I2S -> SD (RIFF header back-patched on stop), playback streams
// SD -> I2S. Sequential half-duplex only — never record and play at once.
// Hardware bring-up is lazy: nothing powers on until first use.
//
// Task lifecycle contract (the loop must never block):
//   * playState_/recState_ are written by the LOOP TASK ONLY. A worker task
//     never touches them. A state leaves Idle only in update(), and only by
//     successfully taking that worker's "done" semaphore — so there is no
//     window where isPlaying() is false while its task is still alive.
//   * A worker's last two statements are xSemaphoreGive(gate) then
//     vTaskDelete(nullptr). Nothing may touch `self` after the give. The give
//     is a release barrier and the take an acquire barrier, so everything the
//     task produced (recordedBytes_, recordFailed_) is visible afterwards.
//   * Stops are REQUESTS. requestStopRecord() and stopPlayback() return
//     immediately; the file is closed and the header patched a few frames
//     later. Poll recordIdle()/playbackIdle() to know when it is safe to tear
//     down SD_MMC or I2S.
//   * A worker task is NEVER killed from outside. vTaskDelete() on a task
//     sitting inside a FATFS call strands the volume mutex forever, after
//     which every later filesystem operation blocks for the full FATFS
//     timeout and fails. Tasks only ever exit themselves — do not "helpfully"
//     add a force-kill here.
class AudioAdapter {
 public:
  bool begin();
  bool ready() const { return ready_; }

  // ---- Playback -----------------------------------------------------------
  // Starts only when fully idle; returns false otherwise.
  bool playWavFile(const char* path);
  // Stop-then-play without blocking: if something is playing it is stopped and
  // this path starts automatically once the old task has been reaped.
  bool requestPlayWavFile(const char* path);
  // Compressed-audio playback (mp3 / aac / flac / wav) decoded by ESP8266Audio,
  // fed into the same ES8311/I2S output the recorder uses. Same PlayState gate
  // and half-duplex rule as WAV playback — never plays while recording.
  bool playMusicFile(const char* path);
  bool requestPlayMusicFile(const char* path);
  // True when the last playback ended because the track finished, not because
  // it was stopped. The Music UI reads this on the playing->idle edge to
  // auto-advance without advancing on a user stop.
  bool lastPlayCompleted() const { return playCompleted_; }
  // Read by the file-local music output sink; a stop request must unwind the
  // decode loop the same way it unwinds the WAV loop.
  bool stopRequested() const { return stopPlay_; }
  // Called by the sink when the decoder reports the stream's real sample rate:
  // re-init I2S + codec at that rate (MCLK tracks fs, so the ES8311 stays valid
  // across rates) and raise the amp.
  void musicConfigureRate(uint32_t hz);
  void stopPlayback();  // request; idempotent; non-blocking
  // Pauses/resumes. Besides gating the decode loop, this mutes the codec so
  // audio stops the instant the button is pressed regardless of what the I2S
  // DMA still holds — done here on the loop thread, never from the audio task
  // (the ES8311 shares the I2C bus with touch, so cross-thread writes race).
  void pausePlayback(bool paused);
  bool playbackPaused() const { return playPaused_; }
  bool isPlaying() const { return playState_ != PlayState::Idle; }
  bool playbackIdle() const { return playState_ == PlayState::Idle; }
  const char* playingPath() const { return playPath_; }
  void playTone(uint16_t freqHz, uint16_t durationMs);
  void setVolumePercent(uint8_t percent);
  uint8_t volumePercent() const { return volume_; }

  // Sleep timer: stop playback after N minutes (0 cancels). Counts down in
  // update(); good for music while reading / at bedtime.
  void setSleepTimerMinutes(uint32_t minutes);
  uint32_t sleepRemainingSec() const { return sleepRemainingMs_ / 1000; }

  // ---- Recording (path is the literal target, typically a .partial) -------
  bool startRecordWav(const char* path, uint32_t sampleRate);
  void requestStopRecord();  // non-blocking; finishes over the next frames
  // The card is gone: skip the header patch and flush, just close the handle.
  void abandonRecording();
  void pauseRecording(bool paused);
  bool recordingPaused() const { return recordPaused_; }
  bool isRecording() const { return recState_ != RecState::Idle; }
  bool recordIdle() const { return recState_ == RecState::Idle; }
  bool recordingFailed() const { return recordFailed_; }  // valid once recordIdle()
  uint32_t recordedBytes() const { return recordedBytes_; }
  // Loudest |sample| seen during the last recording (0..32767). ~0 with
  // bytes > 0 means the mic captured silence; used to diagnose the mic path.
  uint16_t recordedPeak() const { return recordPeak_; }
  uint32_t recordedMs() const;

  // Capture post-processing on the PSRAM spool before the SD write (serial:
  // `recordings normalize|gate on|off`). Written from the loop task between
  // takes, read by the record task at stop time.
  void setRecordNormalize(bool on) { recNormalize_ = on; }
  bool recordNormalize() const { return recNormalize_; }
  void setRecordGate(bool on) { recGate_ = on; }
  bool recordGate() const { return recGate_; }
  // ADC gain, 0..7 = 0..42 dB in 6 dB steps; survives the per-take cold codec
  // init. Do not change while a recording is running (codec is live).
  void setMicGain(uint8_t setting);
  uint8_t micGain() const;

  // Frees the I2S driver early when fully idle (its DMA buffers live in the
  // contiguous internal RAM a TLS handshake competes for). The next record or
  // playback cold-starts the driver again via ensureStarted(); the 10 s idle
  // teardown in update() remains the automatic path.
  void releaseDriverIfIdle();

  // Bounded blocking wait. NEVER call from kernelLoop() — reboot/shutdown only.
  bool waitIdle(uint32_t timeoutMs);

  void update(uint32_t deltaMs);

 private:
  enum class PlayState : uint8_t { Idle, Playing, Stopping };
  enum class RecState : uint8_t { Idle, Recording, Stopping };

  friend void audioRecordTask(void* arg);
  friend void audioPlayTask(void* arg);
  friend void audioMusicTask(void* arg);

  bool ensureStarted(uint32_t sampleRate);
  void setPa(bool on);

  // Amp stays live briefly so back-to-back tracks do not click; I2S is torn
  // down after a longer idle so the codec stops drawing current entirely.
  static constexpr uint32_t kPaHoldMs = 750;
  static constexpr uint32_t kI2sIdleMs = 10000;

  bool ready_ = false;
  bool started_ = false;
  bool paOn_ = false;
  uint32_t rate_ = 16000;
  uint32_t idleMs_ = 0;

  PlayState playState_ = PlayState::Idle;  // loop task only
  RecState recState_ = RecState::Idle;     // loop task only
  SemaphoreHandle_t playDone_ = nullptr;
  SemaphoreHandle_t recDone_ = nullptr;

  volatile bool stopPlay_ = false;
  volatile bool playPaused_ = false;
  volatile bool stopRec_ = false;
  volatile bool recordPaused_ = false;
  volatile bool abandonRec_ = false;
  volatile bool recordFailed_ = false;
  volatile bool recNormalize_ = true;
  volatile bool recGate_ = true;
  volatile uint32_t recordedBytes_ = 0;
  volatile uint16_t recordPeak_ = 0;

  volatile bool playCompleted_ = false;  // last play ended naturally, not stopped

  uint32_t sleepRemainingMs_ = 0;

  char playPath_[128] = "";
  char pendingPath_[128] = "";
  bool pendingPlay_ = false;
  bool pendingMusic_ = false;  // the queued play is a decoded track, not a WAV

  uint8_t volume_ = 70;
};
