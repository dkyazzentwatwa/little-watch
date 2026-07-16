#include "SdStorage.h"

// TODO(task-8): ensureTree over SD_MMC, full sanitizer with allowlist roots
// (paths::kRoot, paths::kDeckRoot).

void SdStorage::begin(SdCardAdapter* card) {
  card_ = card;
}

bool SdStorage::ensureTree() {
  return false;
}

bool SdStorage::sanitizePath(const char* raw, String& out) const {
  (void)raw;
  out = "";
  return false;
}
