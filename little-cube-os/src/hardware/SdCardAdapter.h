#pragma once

#include <Arduino.h>

#include "SdCardState.h"

class EventBus;

// SD_MMC 1-bit card manager: mount ladder, state tracking, hot-removal
// detection (no card-detect pin — throttled polling), and safe eject.
class SdCardAdapter {
 public:
  void begin(EventBus* events);
  void update(uint32_t deltaMs);

  SdCardState state() const { return state_; }
  bool mounted() const { return state_ == SdCardState::Mounted || state_ == SdCardState::ReadOnly; }

  bool mount();
  bool safeEject();

  uint64_t totalBytes() const;
  uint64_t freeBytes() const;

 private:
  EventBus* events_ = nullptr;
  SdCardState state_ = SdCardState::NotPresent;
  uint32_t sincePollMs_ = 0;
};
