#include "SdCardAdapter.h"

#include <FS.h>
#include <SD_MMC.h>

#include "../board_config.h"
#include "../core/EventBus.h"

namespace {
constexpr uint32_t kLadderKhz[] = {25000, 20000, 10000, 4000};
constexpr uint8_t kLadderSteps = sizeof(kLadderKhz) / sizeof(kLadderKhz[0]);
constexpr uint32_t kRemovalPollIntervalMs = 1000;
constexpr uint32_t kInsertPollIntervalMs = 5000;
constexpr const char* kWriteProbePath = "/.lc_probe";
}  // namespace

const char* sdCardStateName(SdCardState state) {
  switch (state) {
    case SdCardState::NotPresent: return "not present";
    case SdCardState::Mounting: return "mounting";
    case SdCardState::Mounted: return "mounted";
    case SdCardState::ReadOnly: return "read-only";
    case SdCardState::UnsupportedFilesystem: return "unsupported filesystem";
    case SdCardState::Corrupted: return "corrupted";
    case SdCardState::Full: return "full";
    case SdCardState::RemovedUnexpectedly: return "removed unexpectedly";
    case SdCardState::Error: return "error";
  }
  return "?";
}

void SdCardAdapter::setState(SdCardState next) {
  if (state_ == next) {
    return;
  }
  state_ = next;
  Serial.printf("[sd] %s\n", sdCardStateName(state_));
  if (events_ == nullptr) {
    return;
  }
  switch (next) {
    case SdCardState::Mounted:
    case SdCardState::ReadOnly:
      events_->publish(SystemEvent::SdMounted);
      break;
    case SdCardState::RemovedUnexpectedly:
      // SdRemoved is published by beginTeardown() instead, so it fires exactly
      // once per teardown and covers the deliberate-eject path too.
      break;
    case SdCardState::Full:
      events_->publish(SystemEvent::SdFull);
      break;
    case SdCardState::Corrupted:
    case SdCardState::UnsupportedFilesystem:
    case SdCardState::Error:
      events_->publish(SystemEvent::SdError);
      break;
    default:
      break;
  }
}

void SdCardAdapter::begin(EventBus* events) {
  events_ = events;
  mount();  // boot must proceed either way; a missing card just stays NotPresent
}

void SdCardAdapter::setQuiesceHooks(QuiesceRequestFn request, QuiesceCheckFn check,
                                    void* context) {
  quiesceRequest_ = request;
  quiesceCheck_ = check;
  quiesceCtx_ = context;
}

bool SdCardAdapter::quiesced() const {
  return quiesceCheck_ == nullptr || quiesceCheck_(quiesceCtx_);
}

// Teardown is a request, never an immediate act: the audio tasks may be
// holding open file handles, and SD_MMC.end() would leave them writing into
// a filesystem that no longer exists.
void SdCardAdapter::beginTeardown(uint32_t timeoutMs) {
  if (ejectPhase_ == EjectPhase::Quiescing) {
    return;
  }
  ejectPhase_ = EjectPhase::Quiescing;
  ejectWaitMs_ = 0;
  ejectTimeoutMs_ = timeoutMs;
  if (events_ != nullptr) {
    events_->publish(SystemEvent::SdRemoved);  // "stop using the card now"
  }
  if (quiesceRequest_ != nullptr) {
    quiesceRequest_(quiesceCtx_);
  }
}

void SdCardAdapter::finishTeardown() {
  ejectPhase_ = EjectPhase::None;
  // If the deadline expired rather than the writers going idle, this call can
  // still block on the FATFS volume lock. That window cannot be closed without
  // forking SD_MMC; the abandon path keeps it short by skipping the header
  // patch and flush, leaving only a close().
  SD_MMC.end();
  cachedTotalBytes_ = 0;
  cachedFreeBytes_ = 0;
  if (ejectedByUser_) {
    setState(SdCardState::NotPresent);
    Serial.println("[sd] safe to remove the card");
  } else {
    Serial.println("[sd] card removed; unmounted");
  }
}

bool SdCardAdapter::probeWrite() {
  fs::File f = SD_MMC.open(kWriteProbePath, FILE_WRITE);
  if (!f) {
    return false;
  }
  const bool ok = f.print("ok") == 2;
  f.close();
  SD_MMC.remove(kWriteProbePath);
  return ok;
}

bool SdCardAdapter::mount() {
  setState(SdCardState::Mounting);

  SD_MMC.end();
  pinMode(PIN_SD_CLK, INPUT_PULLUP);
  pinMode(PIN_SD_CMD, INPUT_PULLUP);
  pinMode(PIN_SD_D0, INPUT_PULLUP);
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);

  bool mounted = false;
  for (uint8_t i = 0; i < kLadderSteps; i++) {
    if (SD_MMC.begin("/sdcard", true /*1-bit*/, false /*no format*/, kLadderKhz[i])) {
      mounted = true;
      Serial.printf("[sd] mounted at %u kHz\n", (unsigned)kLadderKhz[i]);
      break;
    }
  }
  // An explicit mount is the user asking for the card back.
  ejectedByUser_ = false;
  insertBackoff_ = 0;

  if (!mounted) {
    SD_MMC.end();
    setState(SdCardState::NotPresent);  // most likely simply no card
    return false;
  }

  if (SD_MMC.cardType() == CARD_NONE) {
    // The host talked to something that then reported no card type — that is
    // a fault, not an empty slot, and must not be reported as "not present".
    SD_MMC.end();
    setState(SdCardState::Error);
    return false;
  }

  // Mounted, but is the filesystem actually usable?
  fs::File root = SD_MMC.open("/");
  const bool rootOk = root && root.isDirectory();
  if (root) {
    root.close();
  }
  if (!rootOk) {
    SD_MMC.end();
    setState(SdCardState::Corrupted);
    return false;
  }

  if (events_ != nullptr) {
    events_->publish(SystemEvent::SdInserted);
  }

  refreshCapacity();

  if (!probeWrite()) {
    // Card answers but refuses writes: physical lock or FS trouble. Keep it
    // readable rather than failing the whole mount.
    setState(SdCardState::ReadOnly);
    return true;
  }

  // Only declare Full when capacity is actually known and genuinely exhausted.
  // A zero total means the read failed (e.g. an unreadable filesystem), which
  // is not the same as a full card — flagging it Full would wrongly block all
  // writes on a card that has space.
  if (cachedTotalBytes_ > 0 && cachedFreeBytes_ == 0) {
    setState(SdCardState::Full);
    return true;
  }

  setState(SdCardState::Mounted);
  return true;
}

bool SdCardAdapter::requestEject() {
  if (!mounted()) {
    return false;
  }
  ejectedByUser_ = true;
  beginTeardown(kEjectQuiesceMs);
  return true;
}

// The insert poll costs a synchronous SD_MMC.begin(), which stalls the loop.
// Back off so an empty slot does not eat a frame every 5 s indefinitely.
uint32_t SdCardAdapter::insertIntervalMs() const {
  switch (insertBackoff_) {
    case 0: return 5000;
    case 1: return 10000;
    case 2: return 20000;
    default: return 30000;
  }
}

bool SdCardAdapter::shouldPollForInsert() const {
  if (ejectedByUser_ || ejectPhase_ != EjectPhase::None) {
    return false;  // the user asked for the card out; do not drag it back
  }
  // Deliberately exhaustive with no default: adding a state to the enum must
  // fail the build here rather than silently freezing the adapter.
  switch (state_) {
    case SdCardState::NotPresent:
    case SdCardState::RemovedUnexpectedly:
    case SdCardState::Corrupted:
    case SdCardState::UnsupportedFilesystem:
    case SdCardState::Error:
      return true;
    case SdCardState::Mounting:
    case SdCardState::Mounted:
    case SdCardState::ReadOnly:
    case SdCardState::Full:
      return false;
  }
  return false;
}

void SdCardAdapter::tryInsertProbe() {
  SD_MMC.end();
  pinMode(PIN_SD_CLK, INPUT_PULLUP);
  pinMode(PIN_SD_CMD, INPUT_PULLUP);
  pinMode(PIN_SD_D0, INPUT_PULLUP);
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  // Auto-probe only at full speed: the slow ladder steps are what make this
  // expensive, and a card that needs them is rare enough to deserve an
  // explicit `storage mount`, which still walks the whole ladder.
  if (SD_MMC.begin("/sdcard", true, false, kLadderKhz[0]) && SD_MMC.cardType() != CARD_NONE) {
    if (events_ != nullptr) {
      events_->publish(SystemEvent::SdInserted);
    }
    refreshCapacity();
    if (!probeWrite()) {
      setState(SdCardState::ReadOnly);
    } else if (cachedTotalBytes_ > 0 && cachedFreeBytes_ == 0) {
      setState(SdCardState::Full);
    } else {
      setState(SdCardState::Mounted);
    }
    insertBackoff_ = 0;
    return;
  }
  SD_MMC.end();
  if (insertBackoff_ < 3) {
    insertBackoff_++;
  }
}

void SdCardAdapter::update(uint32_t deltaMs) {
  // While quiescing, make no filesystem calls at all — the writers are still
  // finishing and the card may already be physically gone.
  if (ejectPhase_ == EjectPhase::Quiescing) {
    ejectWaitMs_ += deltaMs;
    if (quiesced() || ejectWaitMs_ >= ejectTimeoutMs_) {
      finishTeardown();
    }
    return;
  }

  if (mounted()) {
    removalPollMs_ += deltaMs;
    capacityMs_ += deltaMs;
    if (removalPollMs_ >= kRemovalPollIntervalMs) {
      removalPollMs_ = 0;
      // No card-detect line: probe the filesystem root. A pulled card makes
      // this fail immediately.
      fs::File root = SD_MMC.open("/");
      const bool alive = static_cast<bool>(root);
      if (root) {
        root.close();
      }
      if (!alive) {
        // Latch the state and let writers close their files; the actual
        // SD_MMC.end() happens in finishTeardown() a few frames later.
        setState(SdCardState::RemovedUnexpectedly);
        beginTeardown(kRemovalQuiesceMs);
        return;
      }
    }
    if (capacityMs_ >= kCapacityRefreshMs) {
      capacityMs_ = 0;
      refreshCapacity();
    }
    return;
  }

  if (!shouldPollForInsert()) {
    return;
  }
  insertPollMs_ += deltaMs;
  if (insertPollMs_ < insertIntervalMs()) {
    return;
  }
  insertPollMs_ = 0;
  tryInsertProbe();
}

// The only place that actually walks the FAT. SD_MMC.totalBytes() and
// usedBytes() are an f_getfree each, which can take seconds on a large card,
// so this runs on a slow cadence and everyone else reads the cached values.
void SdCardAdapter::refreshCapacity() {
  // No enum-state guard here: this is called from mount()/tryInsertProbe()
  // while the state is still Mounting/NotPresent, i.e. BEFORE the card counts
  // as mounted(). Gating on mounted() left capacity at zero, and mount() then
  // read that as "free == 0" and flagged every card Full. The card is
  // physically present at every call site; when it is not, totalBytes()
  // returns 0 on its own.
  if (cachedTotalBytes_ == 0) {
    cachedTotalBytes_ = SD_MMC.totalBytes();  // immutable while mounted; cleared on unmount
  }
  const uint64_t used = SD_MMC.usedBytes();
  cachedFreeBytes_ = cachedTotalBytes_ > used ? cachedTotalBytes_ - used : 0;
}
