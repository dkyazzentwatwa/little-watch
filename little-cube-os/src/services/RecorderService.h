#pragma once

#include <Arduino.h>

class AudioAdapter;
class SdCardAdapter;
class SdStorage;
class EventBus;
struct SystemState;

// Voice recording workflow above AudioAdapter: preflight (card mounted,
// writable, free space -> estimated minutes), REC_YYYYMMDD_HHMMSS.wav
// naming, .partial while writing, atomic rename on stop, recovery metadata
// on surprise card removal. Blocks safe-eject while active.
class RecorderService {
 public:
  void begin(AudioAdapter* audio, SdCardAdapter* card, SdStorage* storage, EventBus* events,
             SystemState* state);
  void update(uint32_t deltaMs);

  bool start();
  bool stop();
  bool recording() const { return recording_; }
  uint32_t elapsedMs() const { return elapsedMs_; }

  // Estimated remaining recording capacity at the current sample rate.
  uint32_t estimatedRemainingSec() const;

 private:
  AudioAdapter* audio_ = nullptr;
  SdCardAdapter* card_ = nullptr;
  SdStorage* storage_ = nullptr;
  EventBus* events_ = nullptr;
  SystemState* systemState_ = nullptr;
  bool recording_ = false;
  uint32_t elapsedMs_ = 0;
};
