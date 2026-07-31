#include "VideoService.h"

#if FEATURE_VIDEO

#include <SD_MMC.h>

#include <ctype.h>
#include <string.h>

#include "../board_config.h"
#include "../hardware/SdCardAdapter.h"
#include "../storage/SdStorage.h"
#include "../storage/StoragePaths.h"
#include "../video/LcvReader.h"

namespace {

constexpr const char* kResumeKey = "vidresume";

// Same total order as MusicService: case-insensitive, exact bytes tiebreak.
int nameCompare(const char* a, const char* b) {
  const char* pa = a;
  const char* pb = b;
  for (;; ++pa, ++pb) {
    const unsigned char ca = static_cast<unsigned char>(*pa);
    const unsigned char cb = static_cast<unsigned char>(*pb);
    const int la = tolower(ca);
    const int lb = tolower(cb);
    if (la != lb) {
      return la - lb;
    }
    if (ca == '\0') {
      break;
    }
  }
  return strcmp(a, b);
}

bool sortsBefore(const char* a, const char* b) { return nameCompare(a, b) < 0; }

// Directories sort before files: prefix the sort key with 0/1. The key
// buffer mirrors pageAnchors_ usage in VideoApp (`after` uses the same
// form — see VideoService::list()'s doc comment). Keys carry at most 64
// name chars (matching VideoInfo::name). Two files identical in their
// first 64 characters can still misbehave at a page edge — same latent
// cap MusicService has at 63; accepted.
void makeKey(bool isDir, const char* name, char* key, size_t keyLen) {
  key[0] = isDir ? '0' : '1';
  strncpy(key + 1, name, keyLen - 2);
  key[keyLen - 1] = '\0';
}

bool isLcvName(const char* name) {
  if (name == nullptr || name[0] == '.') {
    return false;  // hidden + AppleDouble entries
  }
  const char* dot = strrchr(name, '.');
  if (dot == nullptr) {
    return false;
  }
  const char* ext = dot + 1;
  return (tolower(static_cast<unsigned char>(ext[0])) == 'l' &&
          tolower(static_cast<unsigned char>(ext[1])) == 'c' &&
          tolower(static_cast<unsigned char>(ext[2])) == 'v' && ext[3] == '\0');
}

bool isDirName(const char* name) { return name != nullptr && name[0] != '.'; }

// Standard CRC-32 (poly 0xEDB88320, reflected), bitwise — keys the resume blob.
uint32_t crc32Path(const char* s) {
  uint32_t crc = 0xFFFFFFFFu;
  while (*s != '\0') {
    crc ^= static_cast<uint8_t>(*s++);
    for (int k = 0; k < 8; k++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (-(static_cast<int32_t>(crc & 1))));
    }
  }
  return ~crc;
}

}  // namespace

void VideoService::begin(SdStorage* storage) {
  storage_ = storage;
  prefsReady_ = prefs_.begin(PREF_NAMESPACE, false);
}

size_t VideoService::list(const char* dir, VideoInfo* out, size_t maxItems, size_t* totalOut,
                          const char* after) {
  if (totalOut != nullptr) {
    *totalOut = 0;
  }
  if (dir == nullptr || out == nullptr || maxItems == 0 || storage_ == nullptr) {
    return 0;
  }
  SdCardAdapter* card = storage_->card();
  if (card == nullptr || !card->mounted()) {
    return 0;
  }
  fs::File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) {
    if (d) {
      d.close();
    }
    return 0;
  }

  const bool paged = after != nullptr && after[0] != '\0';
  size_t total = 0;
  size_t count = 0;
  char keys[kMaxListWindow][66];  // sort keys of the visible window
  if (maxItems > kMaxListWindow) {
    maxItems = kMaxListWindow;
  }
  for (fs::File entry = d.openNextFile(); entry; entry = d.openNextFile()) {
    const char* name = entry.name();
    const bool dirEntry = entry.isDirectory();
    // Subfolders only at the top level: /littlecube/video/<Show>/ ("seasons
    // one level deep" — the design's library rule).
    const bool topLevel = strcmp(dir, paths::kVideo) == 0;
    const bool wanted = dirEntry ? (topLevel && isDirName(name)) : isLcvName(name);
    if (wanted) {
      total++;
      char key[66];
      makeKey(dirEntry, name, key, sizeof(key));
      if (!paged || sortsBefore(after, key)) {
        size_t pos = count < maxItems ? count : maxItems;
        while (pos > 0 && sortsBefore(key, keys[pos - 1])) {
          pos--;
        }
        if (pos < maxItems) {
          for (size_t j = (count < maxItems ? count : maxItems - 1); j > pos; j--) {
            out[j] = out[j - 1];
            memcpy(keys[j], keys[j - 1], sizeof(keys[0]));
          }
          VideoInfo& info = out[pos];
          snprintf(info.path, sizeof(info.path), "%s/%s", dir, name);
          strncpy(info.name, name, sizeof(info.name) - 1);
          info.name[sizeof(info.name) - 1] = '\0';
          info.isDir = dirEntry;
          info.durationMs = 0;
          memcpy(keys[pos], key, sizeof(keys[0]));
          if (count < maxItems) {
            count++;
          }
        }
      }
    }
    entry.close();
  }
  d.close();

  // Durations for the visible files only — one 64-byte header read per row.
  for (size_t i = 0; i < count; i++) {
    if (!out[i].isDir) {
      LcvHeader h;
      char why[8];
      if (LcvReader::readHeader(out[i].path, h, why, sizeof(why))) {
        out[i].durationMs = h.durationMs;
      }
    }
  }
  if (totalOut != nullptr) {
    *totalOut = total;
  }
  return count;
}

bool VideoService::sibling(const char* currentPath, bool forward, char* outPath,
                           size_t outLen) {
  if (currentPath == nullptr || storage_ == nullptr) {
    return false;
  }
  const char* slash = strrchr(currentPath, '/');
  if (slash == nullptr) {
    return false;
  }
  char dir[160];
  const size_t dirLen = static_cast<size_t>(slash - currentPath);
  if (dirLen == 0 || dirLen >= sizeof(dir)) {
    return false;
  }
  memcpy(dir, currentPath, dirLen);
  dir[dirLen] = '\0';
  const char* leaf = slash + 1;

  SdCardAdapter* card = storage_->card();
  if (card == nullptr || !card->mounted()) {
    return false;
  }
  fs::File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) {
    if (d) {
      d.close();
    }
    return false;
  }
  // Sized to the repo's 160-byte path-buffer convention (VideoInfo::path,
  // VideoPlayer::path_): this leaf becomes part of a real SD path, so it
  // must not truncate at the display-name budget.
  char best[160] = "";
  for (fs::File entry = d.openNextFile(); entry; entry = d.openNextFile()) {
    if (!entry.isDirectory() && isLcvName(entry.name())) {
      const char* name = entry.name();
      const bool candidate = forward
          ? (sortsBefore(leaf, name) && (best[0] == '\0' || sortsBefore(name, best)))
          : (sortsBefore(name, leaf) && (best[0] == '\0' || sortsBefore(best, name)));
      if (candidate) {
        strncpy(best, name, sizeof(best) - 1);
        best[sizeof(best) - 1] = '\0';
      }
    }
    entry.close();
  }
  d.close();
  if (best[0] == '\0') {
    return false;
  }
  snprintf(outPath, outLen, "%s/%s", dir, best);
  return true;
}

bool VideoService::nextInFolder(const char* currentPath, char* outPath, size_t outLen) {
  return sibling(currentPath, true, outPath, outLen);
}

bool VideoService::prevInFolder(const char* currentPath, char* outPath, size_t outLen) {
  return sibling(currentPath, false, outPath, outLen);
}

void VideoService::loadResume() {
  if (resumeLoaded_ || !prefsReady_) {
    return;
  }
  resumeLoaded_ = true;
  resumeCount_ = 0;
  const size_t bytes = prefs_.getBytes(kResumeKey, resume_, sizeof(resume_));
  resumeCount_ = bytes / sizeof(ResumeRec);
  seq_ = 0;
  for (size_t i = 0; i < resumeCount_; i++) {
    if (resume_[i].seq > seq_) {
      seq_ = resume_[i].seq;
    }
  }
}

void VideoService::storeResume() {
  if (prefsReady_) {
    prefs_.putBytes(kResumeKey, resume_, resumeCount_ * sizeof(ResumeRec));
  }
}

int VideoService::findResume(uint32_t crc) const {
  for (size_t i = 0; i < resumeCount_; i++) {
    if (resume_[i].pathCrc == crc) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

uint32_t VideoService::resumeMs(const char* path) {
  loadResume();
  const int i = findResume(crc32Path(path));
  return i < 0 ? 0 : resume_[i].posMs;
}

void VideoService::savePosition(const char* path, uint32_t posMs, uint32_t durationMs) {
  loadResume();
  // Early positions are noise; near-complete counts as finished.
  if (posMs < kMinSaveMs ||
      (durationMs > 0 && posMs >= durationMs - durationMs / 20)) {
    clearPosition(path);
    return;
  }
  const uint32_t crc = crc32Path(path);
  int i = findResume(crc);
  if (i < 0) {
    if (resumeCount_ < kMaxResume) {
      i = static_cast<int>(resumeCount_++);
    } else {
      i = 0;  // evict the least recently played
      for (size_t j = 1; j < resumeCount_; j++) {
        if (resume_[j].seq < resume_[i].seq) {
          i = static_cast<int>(j);
        }
      }
    }
    resume_[i].pathCrc = crc;
  }
  resume_[i].posMs = posMs;
  resume_[i].seq = ++seq_;
  storeResume();
}

void VideoService::clearPosition(const char* path) {
  loadResume();
  const int i = findResume(crc32Path(path));
  if (i < 0) {
    return;
  }
  resume_[i] = resume_[resumeCount_ - 1];
  resumeCount_--;
  storeResume();
}

#endif  // FEATURE_VIDEO
