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
  // A Full card is still perfectly readable, so it counts as mounted; only
  // writes are gated. Excluding it here made every note and recording vanish
  // from the UI and left no way to delete anything to free space.
  bool mounted() const {
    return state_ == SdCardState::Mounted || state_ == SdCardState::ReadOnly ||
           state_ == SdCardState::Full;
  }
  // Creating or growing a file needs free space; deleting one does not, and
  // is the only way out of a Full card.
  bool writable() const { return state_ == SdCardState::Mounted; }
  bool canDelete() const {
    return state_ == SdCardState::Mounted || state_ == SdCardState::Full;
  }

  // Full-ladder mount attempt. Publishes events on state changes.
  bool mount();

  // Asks writers to quiesce, then unmounts once they have actually closed
  // their files. Returns immediately — watch ejecting() / state() for the
  // result, which is also printed to the console.
  bool requestEject();
  bool ejecting() const { return ejectPhase_ != EjectPhase::None; }

  // O(1) accessors over the cached values. SD_MMC.totalBytes()/usedBytes()
  // are each a full FAT walk (seconds on a large card) and must never be
  // called from the render path — use refreshCapacity() on a slow cadence.
  uint64_t totalBytes() const { return cachedTotalBytes_; }
  uint64_t freeBytes() const { return cachedFreeBytes_; }
  void refreshCapacity();  // loop task only

 private:
  enum class EjectPhase : uint8_t { None, Quiescing };

  void setState(SdCardState next);
  bool probeWrite();
  void beginTeardown(uint32_t timeoutMs);
  void finishTeardown();
  bool quiesced() const;
  bool shouldPollForInsert() const;
  uint32_t insertIntervalMs() const;
  void tryInsertProbe();

  static constexpr uint32_t kEjectQuiesceMs = 3000;
  static constexpr uint32_t kRemovalQuiesceMs = 3000;
  static constexpr uint32_t kCapacityRefreshMs = 30000;

  EventBus* events_ = nullptr;
  SdCardState state_ = SdCardState::NotPresent;
  uint32_t removalPollMs_ = 0;
  uint32_t insertPollMs_ = 0;
  uint32_t capacityMs_ = 0;
  uint8_t ladderIndex_ = 0;
  uint8_t insertBackoff_ = 0;

  EjectPhase ejectPhase_ = EjectPhase::None;
  uint32_t ejectWaitMs_ = 0;
  uint32_t ejectTimeoutMs_ = 0;
  bool ejectedByUser_ = false;

  uint64_t cachedTotalBytes_ = 0;
  uint64_t cachedFreeBytes_ = 0;

 public:
  // Writers register here so the adapter can ask them to stop and then
  // confirm they have closed their files, without hardware/ needing to know
  // about services/. Same idiom as EventBus::Handler; wired in Kernel.cpp.
  using QuiesceRequestFn = void (*)(void* context);
  using QuiesceCheckFn = bool (*)(void* context);
  void setQuiesceHooks(QuiesceRequestFn request, QuiesceCheckFn check, void* context);

 private:
  QuiesceRequestFn quiesceRequest_ = nullptr;
  QuiesceCheckFn quiesceCheck_ = nullptr;
  void* quiesceCtx_ = nullptr;
};
