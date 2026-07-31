#include "AudioAdapter.h"

#include <ESP_I2S.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <esp_heap_caps.h>

#include <math.h>
#include <string.h>

// ESP8266Audio: decoders for the music library. Their output is NOT wired to
// the library's own AudioOutputI2S (which would fight this file's ESP_I2S
// driver for the peripheral); instead a small sink below feeds the already
// proven ES8311/I2S path.
#include <AudioFileSource.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceICYStream.h>
#include <AudioGeneratorAAC.h>
#include <AudioGeneratorFLAC.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutput.h>

#include "../../board_config.h"
#include "Es8311.h"

namespace {

I2SClass i2s;
fs::File recFile;
uint8_t* recPcm = nullptr;
size_t recPcmCapacity = 0;

// The SD_MMC writes are electrically audible in the onboard analog mic path.
// Six MiB holds just over three minutes of 16 kHz mono PCM while preserving
// more than a MiB of the board's 8 MiB PSRAM for the framebuffer and services.
constexpr size_t kRecordSpoolBytes = 6 * 1024 * 1024;

// ESP8266Audio's stock file source uses the Arduino SD (SPI) global; this
// board's card is on SD_MMC, so the decoder reads through this thin wrapper.
class SdMmcFileSource : public AudioFileSource {
 public:
  SdMmcFileSource() = default;
  ~SdMmcFileSource() override { close(); }

  bool open(const char* path) override {
    file_ = SD_MMC.open(path, FILE_READ);
    return static_cast<bool>(file_);
  }
  uint32_t read(void* data, uint32_t len) override {
    return file_ ? file_.read(static_cast<uint8_t*>(data), len) : 0;
  }
  bool seek(int32_t pos, int dir) override {
    if (!file_) {
      return false;
    }
    uint32_t base = dir == SEEK_CUR ? file_.position() : (dir == SEEK_END ? file_.size() : 0);
    return file_.seek(base + pos);
  }
  bool close() override {
    if (file_) {
      file_.close();
    }
    return true;
  }
  bool isOpen() override { return static_cast<bool>(file_); }
  uint32_t getSize() override { return file_ ? file_.size() : 0; }
  uint32_t getPos() override { return file_ ? file_.position() : 0; }

 private:
  fs::File file_;
};

// Decoder output -> the shared stereo ES8311/I2S bus. The physical speaker is
// mono, but both wire slots must remain enabled because TX and RX share clocks.
// Downmix each decoder frame, duplicate it into L/R, and write in chunks.
class I2SMonoSink : public AudioOutput {
 public:
  explicit I2SMonoSink(AudioAdapter* owner) : owner_(owner) {}

  bool begin() override {
    count_ = 0;
    return true;
  }
  bool SetRate(int hz) override {
    hertz = hz;
    if (hz > 0 && static_cast<uint32_t>(hz) != appliedRate_) {
      appliedRate_ = static_cast<uint32_t>(hz);
      owner_->musicConfigureRate(appliedRate_);
    }
    return true;
  }
  bool SetChannels(int chan) override {
    channels = chan;
    return true;
  }
  bool ConsumeSample(int16_t sample[2]) override {
    const int16_t mono = channels >= 2
                             ? static_cast<int16_t>(
                                   (static_cast<int32_t>(sample[0]) + sample[1]) / 2)
                             : sample[0];
    buf_[2 * count_] = mono;
    buf_[2 * count_ + 1] = mono;
    ++count_;
    if (count_ >= kChunk) {
      i2s.write(reinterpret_cast<uint8_t*>(buf_), count_ * 2 * sizeof(int16_t));
      count_ = 0;
    }
    // Returning false tells the generator to stop: honour a stop request so
    // the decode loop unwinds promptly.
    return !owner_->stopRequested();
  }
  bool stop() override {
    if (count_ > 0) {
      i2s.write(reinterpret_cast<uint8_t*>(buf_), count_ * 2 * sizeof(int16_t));
      count_ = 0;
    }
    return true;
  }

 private:
  static constexpr size_t kChunk = 512;
  AudioAdapter* owner_;
  uint32_t appliedRate_ = 0;
  int16_t buf_[kChunk * 2];
  size_t count_ = 0;
};

void radioStatusCallback(void* data, int /*code*/, const char* message) {
  AudioAdapter* owner = static_cast<AudioAdapter*>(data);
  if (owner != nullptr && message != nullptr) {
    owner->noteRadioStatus(message);
  }
}

void radioMetadataCallback(void* data, const char* type, bool /*unicode*/, const char* value) {
  AudioAdapter* owner = static_cast<AudioAdapter*>(data);
  if (owner != nullptr && value != nullptr && type != nullptr && strcmp(type, "StreamTitle") == 0) {
    owner->noteRadioMetadata(value);
  }
}

// --- Soft noise gate (downward expander) ------------------------------------
// Pauses in a voice note are where the mic chain's hiss floor is most audible.
// Classify 8 ms windows against a per-take floor estimate and ease the quiet
// ones down 12 dB, with a fast-open / slow-close gain smoother so speech
// attacks survive and word tails are not chopped. Integer-only; three linear
// passes over the PSRAM spool. Runs after capture and BEFORE normalization —
// the thresholds belong to the raw take.
constexpr size_t kGateWin = 128;          // 8 ms at 16 kHz
constexpr uint32_t kGateOpenShift = 5;    // ~2 ms attack time constant
constexpr uint32_t kGateCloseShift = 10;  // ~64 ms release time constant
constexpr int32_t kGateAttenQ15 = 8192;   // -12 dB
constexpr int32_t kGateOpenQ15 = 32768;

uint32_t meanAbs(const int16_t* p, size_t n) {
  uint32_t acc = 0;
  for (size_t i = 0; i < n; i++) {
    acc += p[i] < 0 ? static_cast<uint32_t>(-static_cast<int32_t>(p[i]))
                    : static_cast<uint32_t>(p[i]);
  }
  return acc / static_cast<uint32_t>(n);
}

void applyNoiseGate(int16_t* pcm, size_t count) {
  const size_t wins = count / kGateWin;
  if (wins < 16) {
    return;  // under ~130 ms there is no floor to estimate
  }
  // Pass 1: log2-bucketed histogram of per-window mean-abs level; the 10th
  // percentile bucket locates the floor without per-window storage.
  uint32_t hist[17] = {0};
  for (size_t w = 0; w < wins; w++) {
    const uint32_t maa = meanAbs(pcm + w * kGateWin, kGateWin);
    uint8_t b = 0;
    while (b < 16 && (1u << (b + 1)) <= maa) {
      b++;
    }
    hist[b]++;
  }
  const size_t need = wins / 10 + 1;
  size_t seen = 0;
  uint8_t floorBucket = 0;
  for (uint8_t b = 0; b <= 16; b++) {
    seen += hist[b];
    if (seen >= need) {
      floorBucket = b;
      break;
    }
  }
  // Pass 2: refine the floor as the mean of windows near that bucket.
  const uint32_t lo = (1u << floorBucket) / 2;
  const uint32_t hi = (1u << floorBucket) * 4;
  uint64_t sum = 0;
  uint32_t refined = 0;
  for (size_t w = 0; w < wins; w++) {
    const uint32_t maa = meanAbs(pcm + w * kGateWin, kGateWin);
    if (maa >= lo && maa < hi) {
      sum += maa;
      refined++;
    }
  }
  const uint32_t floorMaa =
      refined > 0 ? static_cast<uint32_t>(sum / refined) : (1u << floorBucket);
  // Open ~8 dB over the floor; +4 keeps digital near-silence from producing a
  // zero threshold that would never gate anything.
  const uint32_t threshold = (floorMaa * 5 + 1) / 2 + 4;
  // Pass 3: per-window target gain, smoothed per sample (fast up, slow down).
  int32_t g = kGateOpenQ15;
  for (size_t w = 0; w < wins; w++) {
    int16_t* win = pcm + w * kGateWin;
    const int32_t target =
        meanAbs(win, kGateWin) < threshold ? kGateAttenQ15 : kGateOpenQ15;
    for (size_t i = 0; i < kGateWin; i++) {
      g += (target - g) >> (target > g ? kGateOpenShift : kGateCloseShift);
      win[i] = static_cast<int16_t>((static_cast<int32_t>(win[i]) * g) >> 15);
    }
  }
  // The sub-window tail (<8 ms) keeps the last gain — inaudible either way.
  int16_t* tail = pcm + wins * kGateWin;
  const size_t rest = count - wins * kGateWin;
  for (size_t i = 0; i < rest; i++) {
    tail[i] = static_cast<int16_t>((static_cast<int32_t>(tail[i]) * g) >> 15);
  }
}

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
  // Read the exact raw stereo framing proven clean in Waveshare's example,
  // then explicitly keep the left slot. Avoid ESP_I2S's RX transform and
  // never mix a mono RX configuration with the shared stereo TX clocks.
  int16_t stereo[1024];
  int16_t mono[512];

  // Let the analog reference/PGA and RX DMA settle, keeping the discarded
  // samples out of the WAV and out of its level report. Measured on-device:
  // the bias thump decays into the noise floor ~300 ms after RX starts (and
  // clips outright at high ADC gain, which is where the phantom "peak 32768"
  // reports came from), so drop a full 400 ms.
  size_t discardBytes = static_cast<size_t>(self->rate_) * 2 * sizeof(int16_t) * 400 / 1000;
  while (discardBytes > 0 && !self->stopRec_ && !self->abandonRec_) {
    size_t request = discardBytes;
    if (request > sizeof(stereo)) {
      request = sizeof(stereo);
    }
    const size_t n =
        i2s.readBytes(reinterpret_cast<char*>(stereo), request);
    if (n == 0) {
      vTaskDelay(1);
      continue;
    }
    discardBytes = n >= discardBytes ? 0 : discardBytes - n;
  }

  while (!self->stopRec_ && !self->abandonRec_) {
    const size_t n = i2s.readBytes(reinterpret_cast<char*>(stereo), sizeof(stereo));
    if (n == 0) {
      vTaskDelay(1);
      continue;
    }
    if (self->recordPaused_) {
      continue;  // keep draining the mic, drop the samples
    }
    const size_t frames = n / (2 * sizeof(int16_t));
    for (size_t i = 0; i < frames; ++i) {
      mono[i] = stereo[2 * i];
    }
    const size_t monoBytes = frames * sizeof(int16_t);

    // Track the loudest sample so a finished recording can report whether the
    // mic actually captured signal (peak ~0 = silence: gain/analog problem;
    // recordedBytes_ == 0 = the I2S RX produced nothing at all).
    for (size_t i = 0; i < frames; i++) {
      int32_t a = mono[i] < 0 ? -mono[i] : mono[i];
      if (a > self->recordPeak_) {
        self->recordPeak_ = static_cast<uint16_t>(a);
      }
    }
    const size_t remaining = recPcmCapacity - self->recordedBytes_;
    const size_t copyBytes = monoBytes < remaining ? monoBytes : remaining;
    if (copyBytes > 0) {
      memcpy(recPcm + self->recordedBytes_, mono, copyBytes);
      self->recordedBytes_ += copyBytes;
    }
    if (copyBytes < monoBytes) {
      Serial.println("[audio] recording stopped: PSRAM spool full");
      break;
    }
  }
  if (self->abandonRec_) {
    // The card is already gone; touching FATFS further would only block. PCM
    // remained safe in PSRAM until this point, but there is nowhere to save it.
    self->recordFailed_ = true;
  } else {
    if (self->recGate_) {
      applyNoiseGate(reinterpret_cast<int16_t*>(recPcm),
                     self->recordedBytes_ / sizeof(int16_t));
    }
    // Voice-note normalize: lift the take's peak to ~-3 dBFS (capped at
    // +18 dB so a near-silent take does not become pure amplified hiss).
    // Conversational speech through this mic chain peaks far below full scale
    // while the DAC tops out at 0 dB, so un-normalized notes played at 100%
    // volume are barely audible. Runs before the SD write so the WAV and the
    // peak report describe what will actually play.
    const uint32_t rawPeak = self->recordPeak_;
    if (self->recNormalize_ && rawPeak >= 200) {
      uint32_t gainQ15 = (23170u << 15) / rawPeak;  // target ~-3 dBFS
      if (gainQ15 > (8u << 15)) {
        gainQ15 = 8u << 15;
      }
      if (gainQ15 > (1u << 15)) {  // never attenuate a hot take
        int16_t* pcm = reinterpret_cast<int16_t*>(recPcm);
        const size_t count = self->recordedBytes_ / sizeof(int16_t);
        for (size_t i = 0; i < count; i++) {
          // No clamp needed: |sample| * gain <= rawPeak * gain <= 23170 << 15.
          pcm[i] = static_cast<int16_t>(
              (static_cast<int32_t>(pcm[i]) * static_cast<int32_t>(gainQ15)) >> 15);
        }
        self->recordPeak_ =
            static_cast<uint16_t>((static_cast<uint64_t>(rawPeak) * gainQ15) >> 15);
      }
    }
    // The microphone and RX DMA are stopped before the first data-sector
    // write. This avoids the SD power/ground disturbance visible as a
    // 31.25 Hz harmonic ladder in every prior recording.
    const uint32_t capturedBytes = self->recordedBytes_;
    uint32_t writtenBytes = 0;
    while (writtenBytes < capturedBytes) {
      size_t chunk = capturedBytes - writtenBytes;
      if (chunk > 32768) {
        chunk = 32768;
      }
      const size_t n = recFile.write(recPcm + writtenBytes, chunk);
      writtenBytes += n;
      if (n != chunk) {
        self->recordFailed_ = true;
        break;
      }
    }
    self->recordedBytes_ = writtenBytes;
    writeWavHeader(recFile, self->rate_, 1, 16, self->recordedBytes_);
    recFile.flush();
  }
  recFile.close();
  heap_caps_free(recPcm);
  recPcm = nullptr;
  recPcmCapacity = 0;
  xSemaphoreGive(self->recDone_);  // release barrier — nothing may touch self after this
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
      // ESP32-S3 standard I2S always clocks two physical slots. A recorder WAV
      // is mono, so sending its raw one-sample stream to that bus misplaces
      // alternating samples between left and right slots and turns intelligible
      // speech into a loud buzz. Play every file on a stereo bus; duplicate
      // mono input into both slots, and pass native stereo through unchanged.
      const bool reconfig = rate > 0;
      if (reconfig) {
        i2s.configureTX(rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                        I2S_STD_SLOT_BOTH);
      }
      uint8_t buf[1024];
      int16_t stereo[512];
      size_t n;
      while (!self->stopPlay_ &&
             (n = f.read(buf, channels >= 2 ? sizeof(buf) : sizeof(buf) / 2)) > 0) {
        while (self->playPaused_ && !self->stopPlay_) {
          vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (channels >= 2) {
          i2s.write(buf, n);
        } else {
          const int16_t* mono = reinterpret_cast<const int16_t*>(buf);
          const size_t frames = n / sizeof(int16_t);
          for (size_t i = 0; i < frames; i++) {
            stereo[2 * i] = mono[i];
            stereo[2 * i + 1] = mono[i];
          }
          i2s.write(reinterpret_cast<const uint8_t*>(stereo), frames * 2 * sizeof(int16_t));
        }
      }
      if (reconfig) {
        i2s.configureTX(self->rate_, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                        I2S_STD_SLOT_BOTH);
      }
    }
    f.close();
  }
  self->playCompleted_ = !self->stopPlay_;  // natural end vs. user stop
  xSemaphoreGive(self->playDone_);  // release barrier — nothing may touch self after this
  vTaskDelete(nullptr);
}

// Decodes a compressed track (mp3 / aac / flac / wav) through ESP8266Audio,
// feeding the mono I2S sink above. Same gate contract as audioPlayTask: last
// two statements give playDone_ and delete self.
void audioMusicTask(void* arg) {
  AudioAdapter* self = static_cast<AudioAdapter*>(arg);
  SdMmcFileSource src;
  I2SMonoSink sink(self);
  AudioGenerator* gen = nullptr;

  if (src.open(self->playPath_)) {
    const char* ext = strrchr(self->playPath_, '.');
    if (ext != nullptr) {
      if (strcasecmp(ext, ".mp3") == 0) {
        gen = new AudioGeneratorMP3();
      } else if (strcasecmp(ext, ".aac") == 0 || strcasecmp(ext, ".m4a") == 0) {
        gen = new AudioGeneratorAAC();
      } else if (strcasecmp(ext, ".flac") == 0) {
        gen = new AudioGeneratorFLAC();
      } else {  // .wav and anything else the WAV parser can read
        gen = new AudioGeneratorWAV();
      }
    }
    if (gen != nullptr && gen->begin(&src, &sink)) {
      while (!self->stopPlay_ && gen->isRunning()) {
        while (self->playPaused_ && !self->stopPlay_) {
          vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (self->stopPlay_ || !gen->loop()) {
          break;
        }
      }
      gen->stop();
    }
  }
  if (gen != nullptr) {
    delete gen;
  }
  src.close();
  self->playCompleted_ = !self->stopPlay_;
  xSemaphoreGive(self->playDone_);
  vTaskDelete(nullptr);
}

// Live ICY MP3 radio through the same decoder and I2S sink as SD music. The
// source owns reconnect attempts; the task remains non-blocking to the main
// loop and exits through the same semaphore gate as every other playback task.
void audioRadioTask(void* arg) {
  AudioAdapter* self = static_cast<AudioAdapter*>(arg);
  AudioFileSourceICYStream* src = new AudioFileSourceICYStream();
  AudioFileSourceBuffer* buffer = nullptr;
  AudioGeneratorMP3* gen = nullptr;
  I2SMonoSink sink(self);

  self->noteRadioStatus("connecting");
  if (src != nullptr) {
    src->RegisterStatusCB(radioStatusCallback, self);
    src->RegisterMetadataCB(radioMetadataCallback, self);
    src->SetReconnect(3, 1500);
    if (src->open(self->radioUrl_)) {
      buffer = new AudioFileSourceBuffer(src, 16384);
      gen = new AudioGeneratorMP3();
      if (buffer != nullptr && gen != nullptr) {
        buffer->RegisterStatusCB(radioStatusCallback, self);
        gen->RegisterStatusCB(radioStatusCallback, self);
        if (gen->begin(buffer, &sink)) {
          self->noteRadioStatus("playing");
          while (!self->stopPlay_ && gen->isRunning()) {
            while (self->playPaused_ && !self->stopPlay_) {
              vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (self->stopPlay_ || !gen->loop()) {
              break;
            }
          }
          gen->stop();
        }
      }
    }
  }
  if (gen != nullptr) {
    delete gen;
  }
  if (buffer != nullptr) {
    buffer->close();
    delete buffer;
  }
  if (src != nullptr) {
    delete src;
  }
  if (self->stopPlay_) {
    self->noteRadioStatus("stopped");
  } else if (strcmp(self->radioStatus_, "playing") == 0) {
    self->noteRadioStatus("stream ended");
  }
  self->radioPlaying_ = false;
  self->playCompleted_ = false;
  xSemaphoreGive(self->playDone_);
  vTaskDelete(nullptr);
}

bool AudioAdapter::begin() {
  // Lazy: the codec powers up on first use, not at boot. The amp pin is
  // driven low here so it is defined from boot rather than floating.
  pinMode(PIN_PA_ENABLE, OUTPUT);
  digitalWrite(PIN_PA_ENABLE, LOW);
  paOn_ = false;
  if (playDone_ == nullptr) {
    playDone_ = xSemaphoreCreateBinary();
  }
  if (recDone_ == nullptr) {
    recDone_ = xSemaphoreCreateBinary();
  }
  return playDone_ != nullptr && recDone_ != nullptr;
}

void AudioAdapter::setPa(bool on) {
  if (on == paOn_) {
    return;
  }
  if (!on) {
    Es8311::mute(true);  // mute before cutting the amp so it does not pop
    delay(5);
  }
  digitalWrite(PIN_PA_ENABLE, on ? HIGH : LOW);
  paOn_ = on;
  if (on) {
    Es8311::mute(false);
  }
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

  // The ES8311 is an I2S slave. Start BCLK/LRCK/MCLK before its register
  // sequence so its analog and serial paths latch against live clocks, as in
  // Waveshare's exact-board example.
  i2s.setPins(PIN_I2S_BCK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[audio] I2S begin failed");
    ready_ = false;
    return false;
  }
  if (!Es8311::init(Wire, I2C_ADDR_CODEC_ES8311, sampleRate)) {
    Serial.println("[audio] ES8311 not found");
    i2s.end();
    ready_ = false;
    return false;
  }
  // Leave RX in the exact raw-stereo mode established by begin(). TX and RX
  // share BCLK/WS/MCLK, so neither direction is ever reconfigured as mono.
  // The record task selects the microphone slot explicitly.
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
  if (recState_ != RecState::Idle || playState_ != PlayState::Idle || freqHz == 0 ||
      durationMs == 0) {
    return;
  }
  if (!ensureStarted(rate_)) {
    return;
  }
  setPa(true);
  const size_t total = static_cast<uint32_t>(rate_) * durationMs / 1000;
  const float step = 2.0f * PI * freqHz / rate_;
  int16_t buf[512];
  float phase = 0;
  size_t done = 0;
  while (done < total) {
    size_t chunk = total - done;
    if (chunk > 256) {
      chunk = 256;
    }
    for (size_t i = 0; i < chunk; i++) {
      const int16_t sample = static_cast<int16_t>(sinf(phase) * 8000.0f);
      buf[2 * i] = sample;
      buf[2 * i + 1] = sample;
      phase += step;
      if (phase > 2.0f * PI) {
        phase -= 2.0f * PI;
      }
    }
    i2s.write(reinterpret_cast<uint8_t*>(buf), chunk * 2 * sizeof(int16_t));
    done += chunk;
  }
}

bool AudioAdapter::playWavFile(const char* path) {
  if (recState_ != RecState::Idle || playState_ != PlayState::Idle || path == nullptr) {
    return false;  // sequential half-duplex only
  }
  if (pcmActive_) {
    return false;
  }
  if (!ensureStarted(rate_)) {
    return false;
  }
  strncpy(playPath_, path, sizeof(playPath_) - 1);
  playPath_[sizeof(playPath_) - 1] = '\0';
  stopPlay_ = false;
  playPaused_ = false;
  playCompleted_ = false;
  radioPlaying_ = false;
  setPa(true);
  xSemaphoreTake(playDone_, 0);   // drain a stale give before arming
  playState_ = PlayState::Playing;  // published BEFORE the task can observe it
  if (xTaskCreate(audioPlayTask, "wavplay", 6144, this, 1, nullptr) != pdPASS) {
    playState_ = PlayState::Idle;  // safe: no task exists to contradict this
    return false;
  }
  return true;  // nothing is assigned after xTaskCreate
}

bool AudioAdapter::playMusicFile(const char* path) {
  if (recState_ != RecState::Idle || playState_ != PlayState::Idle || path == nullptr) {
    return false;  // sequential half-duplex only
  }
  if (pcmActive_) {
    return false;
  }
  strncpy(playPath_, path, sizeof(playPath_) - 1);
  playPath_[sizeof(playPath_) - 1] = '\0';
  stopPlay_ = false;
  playPaused_ = false;
  playCompleted_ = false;
  radioPlaying_ = false;
  // The codec + I2S are (re)configured to the stream's real rate by the sink's
  // first SetRate -> musicConfigureRate; not here, where the rate is unknown.
  // A compressed decode (libmad/libflac) needs a far deeper stack than the
  // raw-WAV copy loop.
  xSemaphoreTake(playDone_, 0);
  playState_ = PlayState::Playing;
  if (xTaskCreate(audioMusicTask, "music", 20480, this, 1, nullptr) != pdPASS) {
    playState_ = PlayState::Idle;
    return false;
  }
  return true;
}

void AudioAdapter::musicConfigureRate(uint32_t hz) {
  ensureStarted(hz);  // MCLK = 256*fs, so the ES8311 config holds across rates
  setPa(true);
}

void AudioAdapter::noteRadioStatus(const char* status) {
  if (status == nullptr) {
    return;
  }
  strncpy(radioStatus_, status, sizeof(radioStatus_) - 1);
  radioStatus_[sizeof(radioStatus_) - 1] = '\0';
}

void AudioAdapter::noteRadioMetadata(const char* title) {
  if (title == nullptr) {
    return;
  }
  strncpy(radioMetadata_, title, sizeof(radioMetadata_) - 1);
  radioMetadata_[sizeof(radioMetadata_) - 1] = '\0';
}

bool AudioAdapter::playRadio(const char* url, const char* stationName) {
  if (recState_ != RecState::Idle || playState_ != PlayState::Idle || url == nullptr ||
      stationName == nullptr || url[0] == '\0' || stationName[0] == '\0') {
    return false;
  }
  if (pcmActive_) {
    return false;
  }
  strncpy(radioUrl_, url, sizeof(radioUrl_) - 1);
  radioUrl_[sizeof(radioUrl_) - 1] = '\0';
  strncpy(radioStation_, stationName, sizeof(radioStation_) - 1);
  radioStation_[sizeof(radioStation_) - 1] = '\0';
  strncpy(playPath_, radioUrl_, sizeof(playPath_) - 1);
  playPath_[sizeof(playPath_) - 1] = '\0';
  radioMetadata_[0] = '\0';
  noteRadioStatus("connecting");
  radioPlaying_ = true;
  stopPlay_ = false;
  playPaused_ = false;
  playCompleted_ = false;
  xSemaphoreTake(playDone_, 0);
  playState_ = PlayState::Playing;
  if (xTaskCreate(audioRadioTask, "radio", 20480, this, 1, nullptr) != pdPASS) {
    playState_ = PlayState::Idle;
    radioPlaying_ = false;
    noteRadioStatus("task failed");
    return false;
  }
  return true;
}

// Stop-then-play without blocking the loop. The pending path is started by
// update() on a later frame, once the old task has actually been reaped.
bool AudioAdapter::requestPlayWavFile(const char* path) {
  if (path == nullptr || recState_ != RecState::Idle) {
    return false;
  }
  if (playState_ == PlayState::Idle) {
    return playWavFile(path);
  }
  strncpy(pendingPath_, path, sizeof(pendingPath_) - 1);
  pendingPath_[sizeof(pendingPath_) - 1] = '\0';
  pendingPlay_ = true;  // one slot, last write wins
  pendingMusic_ = false;
  pendingRadio_ = false;
  stopPlayback();
  return true;  // accepted, not yet started
}

bool AudioAdapter::requestPlayMusicFile(const char* path) {
  if (path == nullptr || recState_ != RecState::Idle) {
    return false;
  }
  if (playState_ == PlayState::Idle) {
    return playMusicFile(path);
  }
  strncpy(pendingPath_, path, sizeof(pendingPath_) - 1);
  pendingPath_[sizeof(pendingPath_) - 1] = '\0';
  pendingPlay_ = true;
  pendingMusic_ = true;
  pendingRadio_ = false;
  stopPlayback();
  return true;
}

bool AudioAdapter::requestPlayRadio(const char* url, const char* stationName) {
  if (url == nullptr || stationName == nullptr || recState_ != RecState::Idle) {
    return false;
  }
  if (playState_ == PlayState::Idle) {
    return playRadio(url, stationName);
  }
  strncpy(pendingPath_, url, sizeof(pendingPath_) - 1);
  pendingPath_[sizeof(pendingPath_) - 1] = '\0';
  strncpy(radioStation_, stationName, sizeof(radioStation_) - 1);
  radioStation_[sizeof(radioStation_) - 1] = '\0';
  pendingPlay_ = true;
  pendingMusic_ = false;
  pendingRadio_ = true;
  stopPlayback();
  return true;
}

void AudioAdapter::stopPlayback() {
  if (playState_ == PlayState::Playing) {
    playState_ = PlayState::Stopping;
  }
  stopPlay_ = true;
  if (radioPlaying_) {
    noteRadioStatus("stopping");
  }
}

void AudioAdapter::setSleepTimerMinutes(uint32_t minutes) {
  sleepRemainingMs_ = minutes * 60u * 1000u;
}

void AudioAdapter::pausePlayback(bool paused) {
  playPaused_ = paused;
  // Mute the DAC for instant silence. A new track re-unmutes via setPa(true)
  // in ensureStarted, so leaving the codec muted after a stop is harmless.
  if (ready_) {
    Es8311::mute(paused);
  }
}

// ---- Externally-fed PCM stream (video audio) --------------------------------

bool AudioAdapter::beginPcmStream(uint32_t sampleRate, uint8_t channels) {
  if (!ready_ || channels != 1) {
    return false;
  }
  if (playState_ != PlayState::Idle || recState_ != RecState::Idle || pcmActive_) {
    return false;
  }
  if (!ensureStarted(sampleRate)) {
    return false;
  }
  setPa(true);
  pcmSamples_ = 0;
  pcmPaused_ = false;
  pcmActive_ = true;
  return true;
}

size_t AudioAdapter::writePcm(const int16_t* samples, size_t count) {
  static constexpr size_t kChunkFrames = 256;
  int16_t stereo[kChunkFrames * 2];
  size_t done = 0;
  while (done < count && pcmActive_) {
    if (pcmPaused_) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    size_t n = count - done;
    if (n > kChunkFrames) {
      n = kChunkFrames;
    }
    for (size_t i = 0; i < n; i++) {
      stereo[2 * i] = samples[done + i];
      stereo[2 * i + 1] = samples[done + i];
    }
    i2s.write(reinterpret_cast<uint8_t*>(stereo), n * 2 * sizeof(int16_t));
    pcmSamples_ += n;
    done += n;
  }
  return done;
}

void AudioAdapter::pcmPause(bool paused) {
  pcmPaused_ = paused;
  if (started_) {
    Es8311::mute(paused);  // loop task — same cross-thread I2C rule as pausePlayback()
  }
}

void AudioAdapter::endPcmStream() {
  // Flags only: writePcm unblocks on !pcmActive_, and the normal idle path
  // in update() handles PA hold and I2S teardown. Leaving the codec muted
  // after a paused end is harmless — setPa(true) in the next start unmutes.
  pcmActive_ = false;
  pcmPaused_ = false;
}

bool AudioAdapter::startRecordWav(const char* path, uint32_t sampleRate) {
  if (recState_ != RecState::Idle || playState_ != PlayState::Idle) {
    return false;
  }
  if (pcmActive_) {
    return false;
  }
  // A recording always gets a cold codec/I2S initialization. This prevents a
  // preceding music stream from leaving shared clocks or slot state behind.
  setPa(false);
  if (started_) {
    i2s.end();
    started_ = false;
  }
  if (!ensureStarted(sampleRate)) {
    return false;
  }
  setPa(false);  // amp off while the mic is live, or it feeds back
  // The codec's analog-mic gain and 16-bit ADC framing were applied by init().
  // Do not rewrite the codec while the capture task owns the I2S RX stream.
  recPcm = static_cast<uint8_t*>(
      heap_caps_malloc(kRecordSpoolBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (recPcm == nullptr) {
    Serial.println("[audio] recorder PSRAM spool allocation failed");
    return false;
  }
  recPcmCapacity = kRecordSpoolBytes;
  recFile = SD_MMC.open(path, FILE_WRITE);
  if (!recFile) {
    heap_caps_free(recPcm);
    recPcm = nullptr;
    recPcmCapacity = 0;
    return false;
  }
  recordedBytes_ = 0;
  recordPeak_ = 0;
  recordFailed_ = false;
  recordPaused_ = false;
  stopRec_ = false;
  abandonRec_ = false;
  writeWavHeader(recFile, sampleRate, 1, 16, 0);  // placeholder
  recFile.flush();  // finish all SD work before the microphone task starts
  xSemaphoreTake(recDone_, 0);      // drain a stale give before arming
  recState_ = RecState::Recording;  // published BEFORE xTaskCreate: the task
                                    // runs at a higher priority and would
                                    // otherwise race this assignment
  if (xTaskCreate(audioRecordTask, "wavrec", 6144, this, 2, nullptr) != pdPASS) {
    recState_ = RecState::Idle;  // safe: no task exists to contradict this
    recFile.close();
    heap_caps_free(recPcm);
    recPcm = nullptr;
    recPcmCapacity = 0;
    return false;
  }
  return true;  // nothing is assigned after xTaskCreate
}

void AudioAdapter::pauseRecording(bool paused) {
  recordPaused_ = paused;
}

void AudioAdapter::setMicGain(uint8_t setting) {
  Es8311::setMicGain(setting);
}

uint8_t AudioAdapter::micGain() const {
  return Es8311::micGain();
}

void AudioAdapter::requestStopRecord() {
  if (recState_ == RecState::Recording) {
    recState_ = RecState::Stopping;
    recordPaused_ = false;  // let the task reach its exit check
    stopRec_ = true;
  }
}

void AudioAdapter::abandonRecording() {
  abandonRec_ = true;  // set BEFORE the stop request so the exit path
  requestStopRecord();  // is guaranteed to take the abandon branch
}

void AudioAdapter::releaseDriverIfIdle() {
  if (started_ && recordIdle() && playbackIdle()) {
    setPa(false);
    i2s.end();
    started_ = false;  // ready_ stays true; ensureStarted() re-inits on next use
  }
}

bool AudioAdapter::waitIdle(uint32_t timeoutMs) {
  requestStopRecord();
  stopPlayback();
  const uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(millis() - deadline) < 0) {
    update(0);
    if (recState_ == RecState::Idle && playState_ == PlayState::Idle) {
      return true;
    }
    delay(5);
  }
  return false;
}

uint32_t AudioAdapter::recordedMs() const {
  const uint32_t bytesPerSec = rate_ * 2;  // mono 16-bit
  return bytesPerSec > 0 ? (uint32_t)((uint64_t)recordedBytes_ * 1000 / bytesPerSec) : 0;
}

void AudioAdapter::update(uint32_t deltaMs) {
  // Sleep timer: count down while something is playing and stop when it fires.
  if (sleepRemainingMs_ > 0 && playState_ != PlayState::Idle) {
    if (sleepRemainingMs_ <= deltaMs) {
      sleepRemainingMs_ = 0;
      stopPlayback();
    } else {
      sleepRemainingMs_ -= deltaMs;
    }
  }

  // Taking a gate is the ONLY transition out of a non-Idle state, so a task
  // is guaranteed to have closed its file before the owner sees Idle.
  if (playState_ != PlayState::Idle && xSemaphoreTake(playDone_, 0) == pdTRUE) {
    playState_ = PlayState::Idle;
  }
  if (recState_ != RecState::Idle && xSemaphoreTake(recDone_, 0) == pdTRUE) {
    recState_ = RecState::Idle;
  }

  if (pendingPlay_ && playState_ == PlayState::Idle && recState_ == RecState::Idle &&
      !pcmActive_) {
    pendingPlay_ = false;
    if (pendingRadio_) {
      playRadio(pendingPath_, radioStation_);
    } else if (pendingMusic_) {
      playMusicFile(pendingPath_);
    } else {
      playWavFile(pendingPath_);
    }
    pendingRadio_ = false;
  }

  if (playState_ != PlayState::Idle || recState_ != RecState::Idle || pcmActive_) {
    idleMs_ = 0;
    return;
  }

  if (idleMs_ < kI2sIdleMs) {
    idleMs_ += deltaMs;
  }
  // Hold the amp briefly so back-to-back tracks do not click, then drop it;
  // tear the I2S driver down after a longer idle so the codec stops drawing.
  if (paOn_ && idleMs_ >= kPaHoldMs) {
    setPa(false);
  }
  if (started_ && idleMs_ >= kI2sIdleMs) {
    i2s.end();
    started_ = false;  // ready_ stays true; ensureStarted() re-inits on next use
  }
}
