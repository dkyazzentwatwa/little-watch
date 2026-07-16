#pragma once

#include <Arduino.h>

// ES8311 codec + in-core ESP_I2S. The codec is I2S slave with MCLK = 256 x
// sample rate, 16-bit, onboard analog mic; the power amp enables via GPIO.
// Everything long-running happens on small FreeRTOS tasks: recording
// streams I2S -> SD (RIFF header back-patched on stop), playback streams
// SD -> I2S. Sequential half-duplex only — never record and play at once.
// Hardware bring-up is lazy: nothing powers on until first use.
class AudioAdapter {
 public:
  bool begin();  // stores config only; codec starts on first use
  bool ready() const { return ready_; }

  // Playback (task-based; returns immediately)
  bool playWavFile(const char* path);
  void stopPlayback();
  bool isPlaying() const { return playing_; }
  void playTone(uint16_t freqHz, uint16_t durationMs);  // short + blocking
  void setVolumePercent(uint8_t percent);
  uint8_t volumePercent() const { return volume_; }

  // Recording (path is the literal target, typically a .partial)
  bool startRecordWav(const char* path, uint32_t sampleRate);
  void pauseRecording(bool paused);
  bool recordingPaused() const { return recordPaused_; }
  bool stopRecord();  // waits for the task to patch the header + close
  bool isRecording() const { return recording_; }
  bool recordingFailed() const { return recordFailed_; }
  uint32_t recordedBytes() const { return recordedBytes_; }
  uint32_t recordedMs() const;

  void update(uint32_t deltaMs);

 private:
  friend void audioRecordTask(void* arg);
  friend void audioPlayTask(void* arg);

  bool ensureStarted(uint32_t sampleRate);

  bool ready_ = false;
  bool started_ = false;
  uint32_t rate_ = 16000;

  volatile bool recording_ = false;
  volatile bool recordPaused_ = false;
  volatile bool recordFailed_ = false;
  volatile uint32_t recordedBytes_ = 0;
  void* recTask_ = nullptr;

  volatile bool playing_ = false;
  volatile bool stopPlay_ = false;
  char playPath_[128] = "";

  uint8_t volume_ = 70;
};
