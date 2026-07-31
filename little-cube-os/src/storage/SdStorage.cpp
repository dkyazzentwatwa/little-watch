#include "SdStorage.h"

#include <SD_MMC.h>

#include "../hardware/SdCardAdapter.h"
#include "StoragePaths.h"

namespace {

// Parents before children; ensureTree walks this in order.
constexpr const char* kTree[] = {
    paths::kRoot,
    "/littlecube/notes",
    paths::kNotesText,
    paths::kNotesAudio,
    paths::kRecordings,
    paths::kMusic,
    "/littlecube/podcasts",
    paths::kPodcastFeeds,
    paths::kPodcastDownloads,
    paths::kRadio,
    paths::kVideo,
    paths::kCalendar,
    paths::kContacts,
    paths::kDocuments,
    paths::kExports,
    paths::kBackups,
    paths::kCache,
    "/littlecube/system",
    paths::kSystemIndexes,
    paths::kSystemRecovery,
};

bool ensureDir(const char* path) {
  if (SD_MMC.exists(path)) {
    return true;
  }
  return SD_MMC.mkdir(path);
}

bool hasPrefix(const String& value, const char* prefix) {
  const size_t n = strlen(prefix);
  if (value.length() < n) {
    return false;
  }
  if (strncmp(value.c_str(), prefix, n) != 0) {
    return false;
  }
  // Either the exact root or a path inside it — "/littlecubex" must fail.
  return value.length() == n || value.charAt(n) == '/';
}

}  // namespace

void SdStorage::begin(SdCardAdapter* card) {
  card_ = card;
}

bool SdStorage::ensureTree() {
  if (card_ == nullptr || !card_->writable()) {
    return false;
  }
  bool allOk = true;
  for (const char* dir : kTree) {
    if (!ensureDir(dir)) {
      Serial.printf("[storage] mkdir failed: %s\n", dir);
      allOk = false;
    }
  }
  if (allOk) {
    Serial.println("[storage] /littlecube tree ready");
  }
  return allOk;
}

void SdStorage::resetTreeCursor() {
  treeCursor_ = 0;
}

// One directory per call so tree creation can be spread across frames instead
// of stalling the loop with 20 filesystem round-trips at once.
bool SdStorage::ensureTreeStep() {
  const uint8_t total = sizeof(kTree) / sizeof(kTree[0]);
  if (card_ == nullptr || !card_->writable()) {
    return true;  // nothing to do; do not spin on an unwritable card
  }
  if (treeCursor_ >= total) {
    return true;
  }
  const char* dir = kTree[treeCursor_++];
  if (!ensureDir(dir)) {
    Serial.printf("[storage] mkdir failed: %s\n", dir);
  }
  if (treeCursor_ >= total) {
    Serial.println("[storage] /littlecube tree ready");
    return true;
  }
  return false;
}

bool SdStorage::makeDir(const char* path) {
  String safe;
  if (!sanitizePath(path, safe) || card_ == nullptr || !card_->writable()) {
    return false;
  }
  return ensureDir(safe.c_str());
}

bool SdStorage::removeFile(const char* path) {
  String safe;
  // Deletion is gated on canDelete(), not writable(): a Full card must stay
  // deletable or there is no way to free space from the device.
  if (!sanitizePath(path, safe) || card_ == nullptr || !card_->canDelete()) {
    return false;
  }
  if (SD_MMC.exists(safe)) {
    fs::File f = SD_MMC.open(safe);
    const bool isDir = f && f.isDirectory();
    if (f) {
      f.close();
    }
    return isDir ? SD_MMC.rmdir(safe) : SD_MMC.remove(safe);
  }
  return false;
}

bool SdStorage::renamePath(const char* from, const char* to) {
  String safeFrom;
  String safeTo;
  if (!sanitizePath(from, safeFrom) || !sanitizePath(to, safeTo) || card_ == nullptr ||
      !card_->writable()) {
    return false;
  }
  return SD_MMC.rename(safeFrom, safeTo);
}

bool SdStorage::copyFile(const char* from, const char* to) {
  String safeFrom;
  String safeTo;
  if (!sanitizePath(from, safeFrom) || !sanitizePath(to, safeTo) || card_ == nullptr ||
      !card_->writable()) {
    return false;
  }
  fs::File src = SD_MMC.open(safeFrom, FILE_READ);
  if (!src || src.isDirectory()) {
    if (src) {
      src.close();
    }
    return false;
  }
  fs::File dst = SD_MMC.open(safeTo, FILE_WRITE);
  if (!dst) {
    src.close();
    return false;
  }
  uint8_t chunk[256];
  bool ok = true;
  while (src.available() > 0) {
    const size_t n = src.read(chunk, sizeof(chunk));
    if (n == 0 || dst.write(chunk, n) != n) {
      ok = false;
      break;
    }
  }
  src.close();
  dst.close();
  if (!ok) {
    SD_MMC.remove(safeTo);
  }
  return ok;
}

bool SdStorage::sanitizePath(const char* raw, String& out) const {
  out = "";
  if (raw == nullptr) {
    return false;
  }
  String p(raw);
  p.trim();
  if (p.length() == 0 || p.length() > 200) {
    return false;
  }

  // Reject traversal, backslashes, control characters before any parsing.
  if (p.indexOf("..") >= 0 || p.indexOf('\\') >= 0) {
    return false;
  }
  for (size_t i = 0; i < p.length(); i++) {
    const char c = p.charAt(i);
    if (static_cast<uint8_t>(c) < 0x20 || c == 0x7F) {
      return false;
    }
  }

  // Relative paths live under /littlecube.
  if (p.charAt(0) != '/') {
    p = String(paths::kRoot) + "/" + p;
  }

  // Collapse duplicate slashes.
  while (p.indexOf("//") >= 0) {
    p.replace("//", "/");
  }
  // Trailing slash is only meaningful for the roots themselves.
  while (p.length() > 1 && p.endsWith("/")) {
    p.remove(p.length() - 1);
  }

  if (!hasPrefix(p, paths::kRoot) && !hasPrefix(p, paths::kDeckRoot)) {
    return false;  // file operations stay inside the cube + deck trees
  }

  out = p;
  return true;
}
