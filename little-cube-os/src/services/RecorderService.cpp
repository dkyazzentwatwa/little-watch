#include "RecorderService.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD_MMC.h>

#include <time.h>

#include "../board_config.h"
#include "../core/EventBus.h"
#include "../core/SystemState.h"
#include "../hardware/SdCardAdapter.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../storage/AtomicFile.h"
#include "../storage/SdStorage.h"
#include "../storage/StoragePaths.h"

namespace {
constexpr uint64_t kMinFreeBytesToStart = 1024 * 1024;    // 1 MB
constexpr uint64_t kLowSpaceStopBytes = 256 * 1024;       // stop cleanly here
constexpr uint32_t kSpaceCheckIntervalMs = 10000;
constexpr const char* kRecoveryPath = "/recovery_recording.json";
}  // namespace

void RecorderService::begin(AudioAdapter* audio, SdCardAdapter* card, SdStorage* storage,
                            EventBus* events, SystemState* state) {
  audio_ = audio;
  card_ = card;
  storage_ = storage;
  events_ = events;
  systemState_ = state;

  // Surface an interrupted recording from a previous session, once.
  fs::File f = LittleFS.open(kRecoveryPath, FILE_READ);
  if (f) {
    JsonDocument doc;
    if (!deserializeJson(doc, f)) {
      Serial.printf("[recorder] previous recording ended abnormally (%s): %s\n",
                    doc["reason"] | "?", doc["path"] | "?");
    }
    f.close();
    LittleFS.remove(kRecoveryPath);
  }
}

bool RecorderService::start() {
  if (recording_ || audio_ == nullptr || card_ == nullptr) {
    return false;
  }
  if (!card_->writable()) {
    Serial.printf("[recorder] cannot record: SD %s\n", sdCardStateName(card_->state()));
    return false;
  }
  if (card_->freeBytes() < kMinFreeBytesToStart) {
    Serial.println("[recorder] cannot record: card nearly full");
    return false;
  }

  struct tm t = {};
  time_t nowEpoch = time(nullptr);
  if (nowEpoch > 1700000000) {
    localtime_r(&nowEpoch, &t);
    snprintf(finalPath_, sizeof(finalPath_), "%s/REC_%04d%02d%02d_%02d%02d%02d.wav",
             paths::kRecordings, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
             t.tm_sec);
  } else {
    snprintf(finalPath_, sizeof(finalPath_), "%s/REC_%010lu.wav", paths::kRecordings,
             (unsigned long)millis());
  }
  const String partial = AtomicFile::partialPath(finalPath_);
  strncpy(partialPath_, partial.c_str(), sizeof(partialPath_) - 1);
  partialPath_[sizeof(partialPath_) - 1] = '\0';

  if (!audio_->startRecordWav(partialPath_, AUDIO_SAMPLE_RATE)) {
    Serial.println("[recorder] audio start failed");
    return false;
  }
  recording_ = true;
  spaceCheckMs_ = 0;
  if (systemState_ != nullptr) {
    systemState_->recording = true;
    systemState_->version++;
  }
  if (events_ != nullptr) {
    events_->publish(SystemEvent::RecordingStarted);
  }
  Serial.printf("[recorder] recording -> %s\n", finalPath_);
  return true;
}

void RecorderService::setPaused(bool paused) {
  if (audio_ != nullptr && recording_) {
    audio_->pauseRecording(paused);
    if (events_ != nullptr) {
      events_->publish(paused ? SystemEvent::RecordingPaused : SystemEvent::RecordingStarted);
    }
  }
}

bool RecorderService::paused() const {
  return audio_ != nullptr && audio_->recordingPaused();
}

uint32_t RecorderService::elapsedMs() const {
  return audio_ != nullptr ? audio_->recordedMs() : 0;
}

void RecorderService::writeRecoveryMetadata(const char* reason) {
  JsonDocument doc;
  doc["path"] = finalPath_;
  doc["partial"] = partialPath_;
  doc["bytes"] = audio_ != nullptr ? audio_->recordedBytes() : 0;
  doc["reason"] = reason;
  String body;
  serializeJson(doc, body);
  AtomicFile::writeAll(LittleFS, kRecoveryPath, body);
}

bool RecorderService::stop() {
  if (!recording_ || audio_ == nullptr) {
    return false;
  }
  const bool clean = audio_->stopRecord();
  recording_ = false;
  if (systemState_ != nullptr) {
    systemState_->recording = false;
    systemState_->version++;
  }

  bool finalized = false;
  if (clean && card_ != nullptr && card_->writable()) {
    finalized = AtomicFile::finalizePartial(SD_MMC, finalPath_);
  }
  if (finalized) {
    strncpy(lastFinalPath_, finalPath_, sizeof(lastFinalPath_) - 1);
    Serial.printf("[recorder] saved %s (%lu ms)\n", finalPath_,
                  (unsigned long)audio_->recordedMs());
  } else {
    writeRecoveryMetadata(clean ? "finalize failed" : "write failed (card removed or full)");
    Serial.printf("[recorder] recording may be incomplete; kept %s\n", partialPath_);
  }
  if (events_ != nullptr) {
    events_->publish(SystemEvent::RecordingStopped);
  }
  return finalized;
}

void RecorderService::update(uint32_t deltaMs) {
  if (!recording_ || audio_ == nullptr) {
    return;
  }

  // The record task ends itself when a write fails (card pulled / full).
  if (!audio_->isRecording()) {
    recording_ = false;
    if (systemState_ != nullptr) {
      systemState_->recording = false;
      systemState_->version++;
    }
    writeRecoveryMetadata("card removed during recording");
    Serial.println("[recorder] recording stopped: card removed or write failed;"
                   " partial file kept, metadata saved");
    if (events_ != nullptr) {
      events_->publish(SystemEvent::RecordingStopped);
    }
    return;
  }

  // Low-space guard: stop cleanly before the card is bone dry.
  spaceCheckMs_ += deltaMs;
  if (spaceCheckMs_ >= kSpaceCheckIntervalMs) {
    spaceCheckMs_ = 0;
    if (card_ != nullptr && card_->freeBytes() < kLowSpaceStopBytes) {
      Serial.println("[recorder] card almost full — stopping recording cleanly");
      stop();
    }
  }
}

uint32_t RecorderService::estimatedRemainingSec() const {
  if (card_ == nullptr || !card_->writable()) {
    return 0;
  }
  const uint64_t bytesPerSec = AUDIO_SAMPLE_RATE * 2;  // mono 16-bit
  return (uint32_t)(card_->freeBytes() / bytesPerSec);
}

size_t RecorderService::list(RecordingInfo* out, size_t maxItems) {
  if (out == nullptr || maxItems == 0 || card_ == nullptr || !card_->mounted()) {
    return 0;
  }
  size_t count = 0;
  fs::File dir = SD_MMC.open(paths::kRecordings);
  if (!dir || !dir.isDirectory()) {
    return 0;
  }
  for (fs::File entry = dir.openNextFile(); entry && count < maxItems;
       entry = dir.openNextFile()) {
    const String name(entry.name());
    if (!entry.isDirectory() && name.endsWith(".wav")) {
      RecordingInfo& info = out[count];
      snprintf(info.path, sizeof(info.path), "%s/%s", paths::kRecordings, entry.name());
      strncpy(info.name, entry.name(), sizeof(info.name) - 1);
      info.name[sizeof(info.name) - 1] = '\0';
      info.sizeBytes = entry.size();
      count++;
    }
    entry.close();
  }
  dir.close();
  return count;
}

bool RecorderService::remove(const char* path) {
  if (storage_ == nullptr) {
    return false;
  }
  return storage_->removeFile(path);
}
