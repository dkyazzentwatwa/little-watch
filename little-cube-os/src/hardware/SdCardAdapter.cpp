#include "SdCardAdapter.h"

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

// TODO(task-7): mount ladder (INPUT_PULLUP + 25000/20000/10000/4000 kHz),
// throttled removal poll, safe eject, EventBus notifications.

void SdCardAdapter::begin(EventBus* events) {
  events_ = events;
  state_ = SdCardState::NotPresent;
}

void SdCardAdapter::update(uint32_t deltaMs) {
  sincePollMs_ += deltaMs;
}

bool SdCardAdapter::mount() {
  return false;
}

bool SdCardAdapter::safeEject() {
  return false;
}

uint64_t SdCardAdapter::totalBytes() const {
  return 0;
}

uint64_t SdCardAdapter::freeBytes() const {
  return 0;
}
