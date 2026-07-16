#include "AudioAdapter.h"

#include "Es8311.h"

// TODO(task-15/16): ES8311 init (MCLK = 256 x fs, I2S slave, analog mic),
// ESP_I2S bring-up on MCLK16/BCK9/WS45/DOUT8/DIN10, PA enable GPIO46,
// record task with RIFF header back-patch, WAV playback.

bool AudioAdapter::begin() {
  ready_ = false;
  return ready_;
}

bool AudioAdapter::playWavFile(const char* path) {
  (void)path;
  return false;
}

void AudioAdapter::stopPlayback() {
  playing_ = false;
}

void AudioAdapter::playTone(uint16_t freqHz, uint16_t durationMs) {
  (void)freqHz;
  (void)durationMs;
}

void AudioAdapter::setVolumePercent(uint8_t percent) {
  volume_ = percent > 100 ? 100 : percent;
}

bool AudioAdapter::startRecordWav(const char* path, uint32_t sampleRate) {
  (void)path;
  (void)sampleRate;
  return false;
}

bool AudioAdapter::stopRecord() {
  recording_ = false;
  return false;
}

void AudioAdapter::update(uint32_t deltaMs) {
  (void)deltaMs;
}
