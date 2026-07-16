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
