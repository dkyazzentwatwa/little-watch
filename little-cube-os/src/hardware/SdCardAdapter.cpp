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
      events_->publish(SystemEvent::SdRemoved);
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
  if (!mounted) {
    SD_MMC.end();
    setState(SdCardState::NotPresent);
    return false;
  }

  if (SD_MMC.cardType() == CARD_NONE) {
    SD_MMC.end();
    setState(SdCardState::NotPresent);
    return false;
  }

  if (events_ != nullptr) {
    events_->publish(SystemEvent::SdInserted);
  }

  if (!probeWrite()) {
    // Card answers but refuses writes: physical lock or FS trouble. Keep it
    // readable rather than failing the whole mount.
    setState(SdCardState::ReadOnly);
    return true;
  }

  if (freeBytes() == 0) {
    setState(SdCardState::Full);
    return true;
  }

  setState(SdCardState::Mounted);
  return true;
}

bool SdCardAdapter::safeEject() {
  if (!mounted() && state_ != SdCardState::Full) {
    return false;
  }
  SD_MMC.end();
  setState(SdCardState::NotPresent);
  Serial.println("[sd] safe to remove the card");
  return true;
}

void SdCardAdapter::update(uint32_t deltaMs) {
  if (mounted() || state_ == SdCardState::Full) {
    removalPollMs_ += deltaMs;
    if (removalPollMs_ < kRemovalPollIntervalMs) {
      return;
    }
    removalPollMs_ = 0;
    // No card-detect line: probe the filesystem root. A pulled card makes
    // this fail immediately.
    fs::File root = SD_MMC.open("/");
    const bool alive = static_cast<bool>(root);
    if (root) {
      root.close();
    }
    if (!alive) {
      SD_MMC.end();
      setState(SdCardState::RemovedUnexpectedly);
    }
    return;
  }

  if (state_ == SdCardState::NotPresent || state_ == SdCardState::RemovedUnexpectedly) {
    insertPollMs_ += deltaMs;
    if (insertPollMs_ < kInsertPollIntervalMs) {
      return;
    }
    insertPollMs_ = 0;
    // One ladder step per attempt keeps the loop stall bounded while still
    // eventually matching marginal cards that need a slower clock.
    SD_MMC.end();
    pinMode(PIN_SD_CLK, INPUT_PULLUP);
    pinMode(PIN_SD_CMD, INPUT_PULLUP);
    pinMode(PIN_SD_D0, INPUT_PULLUP);
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    if (SD_MMC.begin("/sdcard", true, false, kLadderKhz[ladderIndex_]) &&
        SD_MMC.cardType() != CARD_NONE) {
      if (events_ != nullptr) {
        events_->publish(SystemEvent::SdInserted);
      }
      if (!probeWrite()) {
        setState(SdCardState::ReadOnly);
      } else if (freeBytes() == 0) {
        setState(SdCardState::Full);
      } else {
        setState(SdCardState::Mounted);
      }
      ladderIndex_ = 0;
    } else {
      SD_MMC.end();
      ladderIndex_ = static_cast<uint8_t>((ladderIndex_ + 1) % kLadderSteps);
    }
  }
}

uint64_t SdCardAdapter::totalBytes() const {
  if (!mounted() && state_ != SdCardState::Full) {
    return 0;
  }
  return SD_MMC.totalBytes();
}

uint64_t SdCardAdapter::freeBytes() const {
  if (!mounted() && state_ != SdCardState::Full) {
    return 0;
  }
  const uint64_t total = SD_MMC.totalBytes();
  const uint64_t used = SD_MMC.usedBytes();
  return total > used ? total - used : 0;
}
