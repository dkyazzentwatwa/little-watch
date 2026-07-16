#pragma once

#include <Arduino.h>

#include "SdCardState.h"

class EventBus;

// SD_MMC 1-bit card manager. Mounting uses the proven ladder (pullups on
// CLK/CMD/D0, then 25/20/10/4 MHz retries). There is no card-detect pin,
// so removal is caught by a throttled root-open probe and insertion by a
// periodic re-mount attempt that cycles the ladder one step per try.
class SdCardAdapter {
 public:
  void begin(EventBus* events);
  void update(uint32_t deltaMs);

  SdCardState state() const { return state_; }
  bool mounted() const {
    return state_ == SdCardState::Mounted || state_ == SdCardState::ReadOnly;
  }
  bool writable() const { return state_ == SdCardState::Mounted; }

  // Full-ladder mount attempt. Publishes events on state changes.
  bool mount();

  // Clean unmount for physical removal. Callers must stop writers first
  // (RecorderService refuses eject while recording).
  bool safeEject();

  uint64_t totalBytes() const;
  uint64_t freeBytes() const;

 private:
  void setState(SdCardState next);
  bool probeWrite();

  EventBus* events_ = nullptr;
  SdCardState state_ = SdCardState::NotPresent;
  uint32_t removalPollMs_ = 0;
  uint32_t insertPollMs_ = 0;
  uint8_t ladderIndex_ = 0;
};
