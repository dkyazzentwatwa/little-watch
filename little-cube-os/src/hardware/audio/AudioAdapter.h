#pragma once

#include <Arduino.h>

// ES8311 codec + ESP_I2S. Recording streams I2S -> SD on a FreeRTOS task
// (the only long-running task besides playback); playback is sequential
// half-duplex — never record and play simultaneously in v1.
class AudioAdapter {
 public:
  bool begin();
  bool ready() const { return ready_; }

  // Playback
  bool playWavFile(const char* path);
  void stopPlayback();
  bool isPlaying() const { return playing_; }
  void playTone(uint16_t freqHz, uint16_t durationMs);
  void setVolumePercent(uint8_t percent);
  uint8_t volumePercent() const { return volume_; }

  // Recording (writes <path>.partial-style targets; RecorderService owns
  // naming and the atomic rename on stop)
  bool startRecordWav(const char* path, uint32_t sampleRate);
  bool stopRecord();
  bool isRecording() const { return recording_; }
  uint32_t recordedMs() const { return recordedMs_; }

  void update(uint32_t deltaMs);

 private:
  bool ready_ = false;
  bool playing_ = false;
  bool recording_ = false;
  uint8_t volume_ = 70;
  uint32_t recordedMs_ = 0;
};
