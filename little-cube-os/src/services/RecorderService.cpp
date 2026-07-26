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

// Recording names are fixed width in both forms this service produces —
// REC_YYYYMMDD_HHMMSS.wav from the RTC, and REC_<10-digit millis>.wav when
// the clock has never been set — so strcmp orders each form by age and a
// reverse strcmp is newest-first.
//
// Mixing the forms is well defined rather than interleaved: the dated form
// starts with '2' and the fallback with '0', so every dated recording sorts
// above every fallback one. That is the order we want, because the fallback
// name is only ever used before the cube has known the date at all.
bool newerThan(const char* a, const char* b) { return strcmp(a, b) > 0; }
}  // namespace

void RecorderService::begin(AudioAdapter* audio, SdCardAdapter* card, SdStorage* storage,
                            EventBus* events, SystemState* state) {
  audio_ = audio;
  card_ = card;
  storage_ = storage;
  events_ = events;
  systemState_ = state;

  // Repair the recovery record itself if its write was interrupted.
  AtomicFile::cleanupSiblings(LittleFS, kRecoveryPath);

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
  card_->refreshCapacity();  // one FAT walk here, none for the rest of the take
  if (card_->freeBytes() < kMinFreeBytesToStart) {
    Serial.println("[recorder] cannot record: card nearly full");
    return false;
  }
  freeAtStartBytes_ = card_->freeBytes();

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
  stopping_ = false;
  stopWaitMs_ = 0;
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
  if (!recording_ || stopping_ || audio_ == nullptr) {
    return false;
  }
  stopping_ = true;
  stopWaitMs_ = 0;
  audio_->requestStopRecord();
  return true;  // accepted; finishStop() completes it in a later update()
}

// Runs once the record task has closed the file. Reached both by a requested
// stop and by the task ending itself (card pulled, write failed).
void RecorderService::finishStop() {
  const bool clean = audio_ != nullptr && !audio_->recordingFailed();
  recording_ = false;
  stopping_ = false;
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
    lastFinalPath_[sizeof(lastFinalPath_) - 1] = '\0';
    // Peak tells you at a glance whether the mic captured sound: ~0 = silence
    // (mic/gain problem), a healthy number (thousands) = the mic works.
    const uint16_t peak = audio_ != nullptr ? audio_->recordedPeak() : 0;
    Serial.printf("[recorder] saved %s (%lu ms, mic peak %u/32767%s)\n", finalPath_,
                  (unsigned long)(audio_ != nullptr ? audio_->recordedMs() : 0), peak,
                  peak < 40 ? " — SILENT, mic captured nothing" : "");
  } else {
    writeRecoveryMetadata(clean ? "finalize failed" : "write failed (card removed or full)");
    Serial.printf("[recorder] recording may be incomplete; kept %s\n", partialPath_);
  }
  if (card_ != nullptr) {
    card_->refreshCapacity();  // the real free-space number is worth one walk here
  }
  if (events_ != nullptr) {
    events_->publish(SystemEvent::RecordingStopped);
  }
}

void RecorderService::update(uint32_t deltaMs) {
  if (!recording_ || audio_ == nullptr) {
    return;
  }
  if (stopping_) {
    stopWaitMs_ += deltaMs;
  }

  // One exit path for both the requested stop and the task ending itself.
  // AudioAdapter::update() runs earlier in the same frame, so recordIdle()
  // is this frame's truth, not last frame's.
  if (audio_->recordIdle()) {
    finishStop();
    return;
  }
  if (stopping_) {
    if (stopWaitMs_ >= kStopTimeoutMs) {
      Serial.println("[recorder] stop timed out; giving up on the writer");
      finishStop();
    }
    return;  // no space guard while winding down
  }

  // Low-space guard: stop cleanly before the card is bone dry. Uses byte
  // arithmetic, not a filesystem call — see remainingBytes().
  spaceCheckMs_ += deltaMs;
  if (spaceCheckMs_ >= kSpaceCheckIntervalMs) {
    spaceCheckMs_ = 0;
    if (remainingBytes() < kLowSpaceStopBytes) {
      Serial.println("[recorder] card almost full — stopping recording cleanly");
      stop();
    }
  }
}

uint64_t RecorderService::remainingBytes() const {
  const uint64_t written = audio_ != nullptr ? audio_->recordedBytes() : 0;
  return freeAtStartBytes_ > written ? freeAtStartBytes_ - written : 0;
}

uint32_t RecorderService::estimatedRemainingSec() const {
  if (card_ == nullptr || !card_->writable()) {
    return 0;
  }
  const uint64_t bytesPerSec = AUDIO_SAMPLE_RATE * 2;  // mono 16-bit
  // While recording, derive it from bytes written so the render path never
  // triggers a full FAT walk (nor contends with the writer for the volume lock).
  const uint64_t free = recording_ ? remainingBytes() : card_->freeBytes();
  return (uint32_t)(free / bytesPerSec);
}

size_t RecorderService::list(RecordingInfo* out, size_t maxItems, size_t* totalOut,
                             const char* after) {
  if (totalOut != nullptr) {
    *totalOut = 0;
  }
  if (out == nullptr || maxItems == 0 || card_ == nullptr || !card_->mounted()) {
    return 0;
  }
  fs::File dir = SD_MMC.open(paths::kRecordings);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return 0;
  }

  const bool paged = after != nullptr && after[0] != '\0';
  size_t total = 0;
  size_t count = 0;
  // The whole directory is walked even when only maxItems fit, because the
  // total is what makes the "+N more" indicator honest. openNextFile() order
  // is whatever the FAT happens to hold, so the window is built by insertion
  // rather than by taking the first entries seen.
  for (fs::File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const String name(entry.name());
    if (!entry.isDirectory() && name.endsWith(".wav")) {
      total++;
      // Everything on a later page sorts strictly older than the anchor.
      if (!paged || newerThan(after, name.c_str())) {
        size_t pos = count < maxItems ? count : maxItems;
        while (pos > 0 && newerThan(name.c_str(), out[pos - 1].name)) {
          pos--;
        }
        if (pos < maxItems) {
          for (size_t j = (count < maxItems ? count : maxItems - 1); j > pos; j--) {
            out[j] = out[j - 1];
          }
          RecordingInfo& info = out[pos];
          snprintf(info.path, sizeof(info.path), "%s/%s", paths::kRecordings, entry.name());
          strncpy(info.name, entry.name(), sizeof(info.name) - 1);
          info.name[sizeof(info.name) - 1] = '\0';
          info.sizeBytes = entry.size();
          if (count < maxItems) {
            count++;
          }
        }
      }
    }
    entry.close();
  }
  dir.close();

  if (totalOut != nullptr) {
    *totalOut = total;
  }
  return count;
}

bool RecorderService::remove(const char* path) {
  if (storage_ == nullptr) {
    return false;
  }
  return storage_->removeFile(path);
}
