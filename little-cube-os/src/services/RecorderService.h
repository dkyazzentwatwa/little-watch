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
  // Requests a stop; the header patch, file close and .partial -> .wav
  // rename complete over the next few frames in update(). Never blocks.
  bool stop();
  bool stopping() const { return stopping_; }
  void setPaused(bool paused);
  bool paused() const;
  // Capture quieting (see updateRecordingQuietCapture in Kernel.cpp): while a
  // take runs, the kernel pauses the 1 Hz AXP2101/PCF85063 housekeeping reads
  // and the unproven-INT touch fallback poll — I2C on the bus shared with the
  // ES8311 is audible in the capture. `recordings quiet off` keeps them live
  // for A/B diagnostics.
  void setQuietCapture(bool on) { quietCapture_ = on; }
  bool quietCapture() const { return quietCapture_; }
  // Stays true through Stopping, so the UI keeps showing "recording" until
  // the file is actually finalized.
  bool recording() const { return recording_; }
  uint32_t elapsedMs() const;
  const char* lastRecordingPath() const { return lastFinalPath_; }

  uint32_t estimatedRemainingSec() const;
  // Free space derived from what the writer has produced — no filesystem
  // call, so this is safe on the render path and does not contend with the
  // record task for the FATFS volume lock.
  uint64_t remainingBytes() const;

  // Fills out[] newest-first and returns how many were written.
  //
  // `totalOut`, when given, receives how many recordings are on the card —
  // not how many were returned. A caller that only knows the second number
  // shows a list that silently pretends to be complete.
  //
  // `after` is keyset paging: pass the name of the last entry of the previous
  // page to get the next one. Unlike an offset, it cannot duplicate or skip a
  // row when a recording is added or deleted between pages.
  size_t list(RecordingInfo* out, size_t maxItems, size_t* totalOut = nullptr,
              const char* after = nullptr);
  bool remove(const char* path);

 private:
  void writeRecoveryMetadata(const char* reason);
  void finishStop();

  bool quietCapture_ = true;  // default on while the buzz investigation runs

  // A wedged filesystem must not pin the recorder forever.
  static constexpr uint32_t kStopTimeoutMs = 5000;

  AudioAdapter* audio_ = nullptr;
  SdCardAdapter* card_ = nullptr;
  SdStorage* storage_ = nullptr;
  EventBus* events_ = nullptr;
  SystemState* systemState_ = nullptr;

  bool recording_ = false;
  bool stopping_ = false;
  uint32_t stopWaitMs_ = 0;
  uint64_t freeAtStartBytes_ = 0;
  char finalPath_[128] = "";
  char partialPath_[140] = "";
  char lastFinalPath_[128] = "";
  uint32_t spaceCheckMs_ = 0;
};
