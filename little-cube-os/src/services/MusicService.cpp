#include "MusicService.h"

#include <SD_MMC.h>

#include <ctype.h>
#include <string.h>

#include "../hardware/SdCardAdapter.h"
#include "../storage/SdStorage.h"
#include "../storage/StoragePaths.h"

namespace {

// Case-insensitive equality; cast to unsigned char before tolower so a byte
// >= 0x80 (UTF-8 in a filename) is never passed to <ctype.h> as a negative int.
bool ciEquals(const char* a, const char* b) {
  while (*a != '\0' && *b != '\0') {
    if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b))) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

// A total order over filenames: case-insensitive first, exact bytes as the
// tiebreak. Two names that differ only in case still compare unequal, which is
// what keeps keyset paging from skipping or repeating a row at a page edge.
int trackCompare(const char* a, const char* b) {
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
      break;  // la == lb and ca == 0 implies cb == 0: fully equal, ignoring case
    }
  }
  return strcmp(a, b);  // stable tiebreak for names differing only in case
}

// True when `a` should appear before `b` in the listed order (ascending).
bool sortsBefore(const char* a, const char* b) { return trackCompare(a, b) < 0; }

// A playable, non-hidden audio file. Skips dot-entries (.DS_Store) and the
// AppleDouble "._name.mp3" resource forks a Mac drops onto the card, which end
// in a real audio extension but are not real tracks.
bool isAudioName(const char* name) {
  if (name == nullptr || name[0] == '.') {
    return false;
  }
  const char* dot = strrchr(name, '.');
  if (dot == nullptr || dot[1] == '\0') {
    return false;
  }
  const char* ext = dot + 1;
  return ciEquals(ext, "mp3") || ciEquals(ext, "wav") || ciEquals(ext, "flac") ||
         ciEquals(ext, "aac") || ciEquals(ext, "m4a");
}

}  // namespace

void MusicService::begin(SdStorage* storage) { storage_ = storage; }

size_t MusicService::list(const char* dir, TrackInfo* out, size_t maxItems, size_t* totalOut,
                          const char* after) {
  if (totalOut != nullptr) {
    *totalOut = 0;
  }
  if (dir == nullptr || out == nullptr || maxItems == 0 || storage_ == nullptr) {
    return 0;
  }
  // mounted() includes a Full card, which is still perfectly readable.
  SdCardAdapter* card = storage_->card();
  if (card == nullptr || !card->mounted()) {
    return 0;
  }

  fs::File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) {
    if (d) {
      d.close();
    }
    return 0;  // directory absent (tree not built yet) is just an empty list
  }

  const bool paged = after != nullptr && after[0] != '\0';
  size_t total = 0;
  size_t count = 0;
  // The whole directory is walked even when only maxItems fit, because `total`
  // is what makes the "+N more" indicator honest. openNextFile() order is
  // whatever the FAT holds, so the visible window is built by sorted insertion
  // rather than by taking the first entries seen. Same shape as
  // RecorderService::list, with an ascending case-insensitive comparator.
  for (fs::File entry = d.openNextFile(); entry; entry = d.openNextFile()) {
    if (!entry.isDirectory()) {
      const char* name = entry.name();  // leaf name on esp32 core 3.x
      if (isAudioName(name)) {
        total++;
        // Everything on a later page sorts strictly after the anchor.
        if (!paged || sortsBefore(after, name)) {
          size_t pos = count < maxItems ? count : maxItems;
          while (pos > 0 && sortsBefore(name, out[pos - 1].name)) {
            pos--;
          }
          if (pos < maxItems) {
            for (size_t j = (count < maxItems ? count : maxItems - 1); j > pos; j--) {
              out[j] = out[j - 1];
            }
            TrackInfo& info = out[pos];
            snprintf(info.path, sizeof(info.path), "%s/%s", dir, name);
            strncpy(info.name, name, sizeof(info.name) - 1);
            info.name[sizeof(info.name) - 1] = '\0';
            info.sizeBytes = entry.size();
            if (count < maxItems) {
              count++;
            }
          }
        }
      }
    }
    entry.close();
  }
  d.close();

  if (totalOut != nullptr) {
    *totalOut = total;
  }
  return count;
}
