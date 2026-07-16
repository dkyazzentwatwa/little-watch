#include "AudioAdapter.h"

#include <ESP_I2S.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>

#include <math.h>

#include "../../board_config.h"
#include "Es8311.h"

namespace {

I2SClass i2s;
fs::File recFile;

// Standard 44-byte PCM header; written as a placeholder at start and
// patched with the real byte count when recording stops.
void writeWavHeader(fs::File& f, uint32_t sampleRate, uint16_t channels, uint16_t bits,
                    uint32_t dataBytes) {
  const uint32_t byteRate = sampleRate * channels * bits / 8;
  const uint16_t blockAlign = channels * bits / 8;
  const uint32_t chunkSize = 36 + dataBytes;
  const uint32_t sub1 = 16;
  const uint16_t fmt = 1;
  f.seek(0);
  f.write(reinterpret_cast<const uint8_t*>("RIFF"), 4);
  f.write(reinterpret_cast<const uint8_t*>(&chunkSize), 4);
  f.write(reinterpret_cast<const uint8_t*>("WAVE"), 4);
  f.write(reinterpret_cast<const uint8_t*>("fmt "), 4);
  f.write(reinterpret_cast<const uint8_t*>(&sub1), 4);
  f.write(reinterpret_cast<const uint8_t*>(&fmt), 2);
  f.write(reinterpret_cast<const uint8_t*>(&channels), 2);
  f.write(reinterpret_cast<const uint8_t*>(&sampleRate), 4);
  f.write(reinterpret_cast<const uint8_t*>(&byteRate), 4);
  f.write(reinterpret_cast<const uint8_t*>(&blockAlign), 2);
  f.write(reinterpret_cast<const uint8_t*>(&bits), 2);
  f.write(reinterpret_cast<const uint8_t*>("data"), 4);
  f.write(reinterpret_cast<const uint8_t*>(&dataBytes), 4);
}

}  // namespace

void audioRecordTask(void* arg) {
  AudioAdapter* self = static_cast<AudioAdapter*>(arg);
  uint8_t buf[1024];
  uint32_t sinceFlush = 0;
  while (self->recording_) {
    const size_t n = i2s.readBytes(reinterpret_cast<char*>(buf), sizeof(buf));
    if (n == 0) {
      vTaskDelay(1);
      continue;
    }
    if (self->recordPaused_) {
      continue;  // keep draining the mic, drop the samples
    }
    if (recFile.write(buf, n) != n) {
      // Card pulled or full mid-write: stop cleanly, keep what we have.
      self->recordFailed_ = true;
      break;
    }
    self->recordedBytes_ += n;
    sinceFlush += n;
    if (sinceFlush >= 32768) {
      recFile.flush();  // periodic flush bounds data loss on power cut
      sinceFlush = 0;
    }
  }
  writeWavHeader(recFile, self->rate_, 1, 16, self->recordedBytes_);
  recFile.flush();
  recFile.close();
  self->recording_ = false;
  self->recTask_ = nullptr;
  vTaskDelete(nullptr);
}

void audioPlayTask(void* arg) {
  AudioAdapter* self = static_cast<AudioAdapter*>(arg);
  fs::File f = SD_MMC.open(self->playPath_, FILE_READ);
  if (f) {
    uint8_t hdr[44];
    if (f.read(hdr, 44) == 44) {
      const uint32_t rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
      const uint16_t channels = hdr[22] | (hdr[23] << 8);
      const bool reconfig = rate > 0 && rate != self->rate_;
      if (reconfig) {
        i2s.configureTX(rate, I2S_DATA_BIT_WIDTH_16BIT,
                        channels >= 2 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
      }
      uint8_t buf[1024];
      size_t n;
      while (!self->stopPlay_ && (n = f.read(buf, sizeof(buf))) > 0) {
        i2s.write(buf, n);
      }
      if (reconfig) {
        i2s.configureTX(self->rate_, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
      }
    }
    f.close();
  }
  self->playing_ = false;
  vTaskDelete(nullptr);
}

bool AudioAdapter::begin() {
  // Lazy: the codec + amp power up on first use, not at boot.
  return true;
}

bool AudioAdapter::ensureStarted(uint32_t sampleRate) {
  if (started_ && rate_ == sampleRate) {
    return ready_;
  }
  if (started_) {
    i2s.end();
    started_ = false;
  }
  rate_ = sampleRate;

  if (!Es8311::init(Wire, I2C_ADDR_CODEC_ES8311, sampleRate)) {
    Serial.println("[audio] ES8311 not found");
    ready_ = false;
    return false;
  }
  pinMode(PIN_PA_ENABLE, OUTPUT);
  digitalWrite(PIN_PA_ENABLE, HIGH);

  i2s.setPins(PIN_I2S_BCK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("[audio] I2S begin failed");
    ready_ = false;
    return false;
  }
  i2s.configureRX(sampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  Es8311::setVolume(volume_);
  started_ = true;
  ready_ = true;
  Serial.printf("[audio] ES8311 ok @ %u Hz\n", (unsigned)sampleRate);
  return true;
}

void AudioAdapter::setVolumePercent(uint8_t percent) {
  volume_ = percent > 100 ? 100 : percent;
  if (ready_) {
    Es8311::setVolume(volume_);
  }
}

void AudioAdapter::playTone(uint16_t freqHz, uint16_t durationMs) {
  if (recording_ || playing_ || freqHz == 0 || durationMs == 0) {
    return;
  }
  if (!ensureStarted(rate_)) {
    return;
  }
  const size_t total = static_cast<uint32_t>(rate_) * durationMs / 1000;
  const float step = 2.0f * PI * freqHz / rate_;
  int16_t buf[256];
  float phase = 0;
  size_t done = 0;
  while (done < total) {
    size_t chunk = total - done;
    if (chunk > 256) {
      chunk = 256;
    }
    for (size_t i = 0; i < chunk; i++) {
      buf[i] = static_cast<int16_t>(sinf(phase) * 8000.0f);
      phase += step;
      if (phase > 2.0f * PI) {
        phase -= 2.0f * PI;
      }
    }
    i2s.write(reinterpret_cast<uint8_t*>(buf), chunk * 2);
    done += chunk;
  }
}

bool AudioAdapter::playWavFile(const char* path) {
  if (recording_ || playing_ || path == nullptr) {
    return false;  // sequential half-duplex only
  }
  if (!ensureStarted(rate_)) {
    return false;
  }
  strncpy(playPath_, path, sizeof(playPath_) - 1);
  playPath_[sizeof(playPath_) - 1] = '\0';
  stopPlay_ = false;
  playing_ = true;
  if (xTaskCreate(audioPlayTask, "wavplay", 4096, this, 1, nullptr) != pdPASS) {
    playing_ = false;
    return false;
  }
  return true;
}

void AudioAdapter::stopPlayback() {
  stopPlay_ = true;
}

bool AudioAdapter::startRecordWav(const char* path, uint32_t sampleRate) {
  if (recording_ || playing_) {
    return false;
  }
  if (!ensureStarted(sampleRate)) {
    return false;
  }
  recFile = SD_MMC.open(path, FILE_WRITE);
  if (!recFile) {
    return false;
  }
  recordedBytes_ = 0;
  recordFailed_ = false;
  recordPaused_ = false;
  writeWavHeader(recFile, sampleRate, 1, 16, 0);  // placeholder
  recording_ = true;
  TaskHandle_t handle = nullptr;
  if (xTaskCreate(audioRecordTask, "wavrec", 4096, this, 2, &handle) != pdPASS) {
    recording_ = false;
    recFile.close();
    return false;
  }
  recTask_ = handle;
  return true;
}

void AudioAdapter::pauseRecording(bool paused) {
  recordPaused_ = paused;
}

bool AudioAdapter::stopRecord() {
  if (!recording_ && recTask_ == nullptr) {
    return false;
  }
  recording_ = false;
  while (recTask_ != nullptr) {
    delay(5);  // the task patches the header and closes the file
  }
  return !recordFailed_;
}

uint32_t AudioAdapter::recordedMs() const {
  const uint32_t bytesPerSec = rate_ * 2;  // mono 16-bit
  return bytesPerSec > 0 ? (uint32_t)((uint64_t)recordedBytes_ * 1000 / bytesPerSec) : 0;
}

void AudioAdapter::update(uint32_t deltaMs) {
  (void)deltaMs;
}
