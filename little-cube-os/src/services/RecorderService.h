#pragma once

#include <Arduino.h>

class AudioAdapter;
class SdCardAdapter;
class SdStorage;
class EventBus;
struct SystemState;

// Voice recording workflow (spec §23): preflight (card writable, free
// space -> estimated minutes), REC_YYYYMMDD_HHMMSS.wav naming, .partial
// while writing, atomic rename on clean stop, recovery metadata in
// LittleFS when the card vanishes mid-recording. Blocks safe-eject while
// active (the storage command checks recording()).
struct RecordingInfo {
  char path[128] = "";
  char name[48] = "";
  size_t sizeBytes = 0;
};

class RecorderService {
 public:
  void begin(AudioAdapter* audio, SdCardAdapter* card, SdStorage* storage, EventBus* events,
             SystemState* state);
  void update(uint32_t deltaMs);

  bool start();
  bool stop();  // clean stop: finalize .partial -> .wav
  void setPaused(bool paused);
  bool paused() const;
  bool recording() const { return recording_; }
  uint32_t elapsedMs() const;
  const char* lastRecordingPath() const { return lastFinalPath_; }

  uint32_t estimatedRemainingSec() const;
  size_t list(RecordingInfo* out, size_t maxItems);
  bool remove(const char* path);

 private:
  void writeRecoveryMetadata(const char* reason);

  AudioAdapter* audio_ = nullptr;
  SdCardAdapter* card_ = nullptr;
  SdStorage* storage_ = nullptr;
  EventBus* events_ = nullptr;
  SystemState* systemState_ = nullptr;

  bool recording_ = false;
  char finalPath_[128] = "";
  char partialPath_[140] = "";
  char lastFinalPath_[128] = "";
  uint32_t spaceCheckMs_ = 0;
};
