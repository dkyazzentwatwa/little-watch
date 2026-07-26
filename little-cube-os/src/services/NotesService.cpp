#include "NotesService.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD_MMC.h>

#include "../hardware/SdCardAdapter.h"
#include "../storage/AtomicFile.h"
#include "../storage/SdStorage.h"
#include "../storage/StoragePaths.h"

namespace {

constexpr const char* kFlagsPath = "/notes_flags.json";

bool isNoteFile(const char* name) {
  const String n(name);
  if (n.endsWith(".tmp") || n.endsWith(".bak") || n.endsWith(".partial")) {
    return false;
  }
  return n.endsWith(".txt") || n.endsWith(".md");
}

const char* baseName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash != nullptr ? slash + 1 : path;
}

}  // namespace

void NotesService::begin(SdStorage* storage) {
  storage_ = storage;
  // Repair the flags index if a write was interrupted, before reading it.
  AtomicFile::cleanupSiblings(LittleFS, kFlagsPath);
  loadFlags();
}

// Walks one notes directory and restores any note whose only surviving copy
// is a <name>.bak left by a write that lost power between the two renames.
// Such a file is invisible to every listing (isNoteFile filters .bak out), so
// without this pass the note is simply gone.
void NotesService::recoverInterrupted() {
  if (storage_ == nullptr || storage_->card() == nullptr || !storage_->card()->writable()) {
    return;
  }
  const char* roots[] = {paths::kNotesText, paths::kDeckNotes};
  for (const char* root : roots) {
    fs::File dir = SD_MMC.open(root);
    if (!dir || !dir.isDirectory()) {
      if (dir) {
        dir.close();
      }
      continue;
    }
    for (fs::File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      if (entry.isDirectory()) {
        continue;
      }
      const String name(entry.name());
      String full = String(root) + "/" + baseName(entry.name());
      entry.close();
      if (name.endsWith(".tmp")) {
        SD_MMC.remove(full);  // a temp file is never the real content
        continue;
      }
      if (!name.endsWith(".bak")) {
        continue;
      }
      String target = full.substring(0, full.length() - 4);
      if (!SD_MMC.exists(target)) {
        if (SD_MMC.rename(full, target)) {
          Serial.printf("[notes] recovered interrupted write: %s\n", target.c_str());
        }
      } else {
        SD_MMC.remove(full);
      }
    }
    dir.close();
  }
}

void NotesService::titleFromPath(const char* path, char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return;
  }
  const char* name = baseName(path);
  String title(name);

  const int dot = title.lastIndexOf('.');
  if (dot > 0) {
    title.remove(dot);
  }
  // Deck convention: strip a leading NNNN_ sequence prefix.
  if (title.length() > 5 && title.charAt(4) == '_' && isDigit(title.charAt(0)) &&
      isDigit(title.charAt(1)) && isDigit(title.charAt(2)) && isDigit(title.charAt(3))) {
    title.remove(0, 5);
  }
  title.replace('-', ' ');
  title.replace('_', ' ');
  if (title.length() > 0) {
    title.setCharAt(0, toupper(title.charAt(0)));
  }

  strncpy(out, title.c_str(), outSize - 1);
  out[outSize - 1] = '\0';
}

int NotesService::findFlag(const String* arr, size_t count, const char* path) const {
  for (size_t i = 0; i < count; i++) {
    if (arr[i].equals(path)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool NotesService::isFavorite(const char* path) const {
  return findFlag(favorites_, favoriteCount_, path) >= 0;
}

bool NotesService::isPinned(const char* path) const {
  return findFlag(pinned_, pinnedCount_, path) >= 0;
}

void NotesService::loadFlags() {
  favoriteCount_ = 0;
  pinnedCount_ = 0;
  fs::File f = LittleFS.open(kFlagsPath, FILE_READ);
  if (!f) {
    return;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    // Be blunt: the file is left on disk but nothing here can read it, so the
    // first favourite or pin the user sets replaces it wholesale.
    Serial.printf(
        "[notes] %s is corrupt (%s) — all favourites and pins are gone, and this file will be "
        "overwritten by the next favourite/pin change\n",
        kFlagsPath, err.c_str());
    return;
  }
  for (JsonVariant v : doc["fav"].as<JsonArray>()) {
    if (favoriteCount_ < kMaxFlagged && v.is<const char*>()) {
      favorites_[favoriteCount_++] = v.as<const char*>();
    }
  }
  for (JsonVariant v : doc["pin"].as<JsonArray>()) {
    if (pinnedCount_ < kMaxFlagged && v.is<const char*>()) {
      pinned_[pinnedCount_++] = v.as<const char*>();
    }
  }
}

bool NotesService::saveFlags() {
  JsonDocument doc;
  JsonArray fav = doc["fav"].to<JsonArray>();
  for (size_t i = 0; i < favoriteCount_; i++) {
    fav.add(favorites_[i]);
  }
  JsonArray pin = doc["pin"].to<JsonArray>();
  for (size_t i = 0; i < pinnedCount_; i++) {
    pin.add(pinned_[i]);
  }
  String body;
  serializeJson(doc, body);
  return AtomicFile::writeAll(LittleFS, kFlagsPath, body);
}

// The index is keyed by path, so a rename or a delete that does not touch it
// leaves an entry pointing at a file that no longer exists. Those entries are
// invisible but permanent: they consume one of the kMaxFlagged slots forever,
// and a later note that happens to land on the freed filename inherits the
// flag. Callers persist afterwards, so a rename costs one flags write, not two.
bool NotesService::retargetFlags(const char* oldPath, const char* newPath) {
  bool changed = false;
  for (int pass = 0; pass < 2; pass++) {
    String* arr = pass == 0 ? favorites_ : pinned_;
    size_t& n = pass == 0 ? favoriteCount_ : pinnedCount_;
    const int at = findFlag(arr, n, oldPath);
    if (at < 0) {
      continue;
    }
    if (newPath != nullptr && newPath[0] != '\0') {
      arr[at] = newPath;
    } else {
      for (size_t i = static_cast<size_t>(at); i + 1 < n; i++) {
        arr[i] = arr[i + 1];
      }
      n--;
    }
    changed = true;
  }
  return changed;
}

bool NotesService::setFavorite(const char* path, bool on) {
  const int at = findFlag(favorites_, favoriteCount_, path);
  if (on && at < 0) {
    if (favoriteCount_ >= kMaxFlagged) {
      return false;
    }
    favorites_[favoriteCount_++] = path;
  } else if (!on && at >= 0) {
    for (size_t i = at; i + 1 < favoriteCount_; i++) {
      favorites_[i] = favorites_[i + 1];
    }
    favoriteCount_--;
  }
  return saveFlags();
}

bool NotesService::setPinned(const char* path, bool on) {
  const int at = findFlag(pinned_, pinnedCount_, path);
  if (on && at < 0) {
    if (pinnedCount_ >= kMaxFlagged) {
      return false;
    }
    pinned_[pinnedCount_++] = path;
  } else if (!on && at >= 0) {
    for (size_t i = at; i + 1 < pinnedCount_; i++) {
      pinned_[i] = pinned_[i + 1];
    }
    pinnedCount_--;
  }
  return saveFlags();
}

size_t NotesService::list(NoteInfo* out, size_t maxNotes, size_t* totalOut) {
  if (totalOut != nullptr) {
    *totalOut = 0;
  }
  if (out == nullptr || maxNotes == 0 || storage_ == nullptr ||
      !storage_->card()->mounted()) {
    return 0;
  }
  size_t count = 0;
  size_t total = 0;

  // Collector shared by both roots. Deck subfolders are one level deep.
  auto scanDir = [&](const char* dirPath, bool fromDeck, bool recurseOnce,
                     auto&& self) -> void {
    fs::File dir = SD_MMC.open(dirPath);
    if (!dir || !dir.isDirectory()) {
      if (dir) {
        dir.close();
      }
      return;
    }
    for (fs::File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      const String full = String(dirPath) + "/" + entry.name();
      if (entry.isDirectory()) {
        if (recurseOnce) {
          self(full.c_str(), fromDeck, false, self);
        }
      } else if (isNoteFile(entry.name())) {
        // Counted even past the window: the walk is cheap next to the reads,
        // and without the real total the UI cannot say how much it is hiding.
        total++;
        if (count < maxNotes) {
          NoteInfo& info = out[count];
          strncpy(info.path, full.c_str(), sizeof(info.path) - 1);
          info.path[sizeof(info.path) - 1] = '\0';
          titleFromPath(info.path, info.title, sizeof(info.title));
          info.sizeBytes = entry.size();
          info.favorite = isFavorite(info.path);
          info.pinned = isPinned(info.path);
          info.fromDeck = fromDeck;
          count++;
        }
      }
      entry.close();
    }
    dir.close();
  };

  scanDir(paths::kNotesText, false, false, scanDir);
  scanDir(paths::kDeckNotes, true, true, scanDir);

  // Pinned notes float to the front (stable-ish selection sort; list sizes
  // are small).
  size_t front = 0;
  for (size_t i = 0; i < count; i++) {
    if (out[i].pinned) {
      const NoteInfo tmp = out[i];
      for (size_t j = i; j > front; j--) {
        out[j] = out[j - 1];
      }
      out[front++] = tmp;
    }
  }
  if (totalOut != nullptr) {
    *totalOut = total;
  }
  return count;
}

bool NotesService::read(const char* path, String& outBody, size_t maxBytes, bool& truncated) {
  outBody = "";
  truncated = false;
  String safe;
  if (storage_ == nullptr || !storage_->sanitizePath(path, safe)) {
    return false;
  }
  fs::File f = SD_MMC.open(safe, FILE_READ);
  if (!f || f.isDirectory()) {
    return false;
  }
  const size_t total = f.size();
  const size_t toRead = total > maxBytes ? maxBytes : total;
  outBody.reserve(toRead + 1);
  char chunk[257];
  size_t remaining = toRead;
  while (remaining > 0) {
    const size_t n = f.readBytes(chunk, remaining > 256 ? 256 : remaining);
    if (n == 0) {
      break;
    }
    chunk[n] = '\0';
    outBody += chunk;
    remaining -= n;
  }
  f.close();
  truncated = total > maxBytes;
  return true;
}

bool NotesService::write(const char* path, const String& body) {
  String safe;
  if (storage_ == nullptr || !storage_->sanitizePath(path, safe)) {
    return false;
  }
  if (!storage_->card()->writable()) {
    return false;
  }
  return AtomicFile::writeAll(SD_MMC, safe.c_str(), body);
}

bool NotesService::append(const char* path, const String& text) {
  String safe;
  if (storage_ == nullptr || !storage_->sanitizePath(path, safe)) {
    return false;
  }
  if (!storage_->card()->writable()) {
    return false;
  }
  fs::File f = SD_MMC.open(safe, FILE_APPEND);
  if (!f) {
    return false;
  }
  const size_t written = f.print(text);
  f.close();
  return written == text.length();
}

bool NotesService::rename(const char* path, const char* newTitle, String& outNewPath) {
  outNewPath = "";
  String safe;
  if (storage_ == nullptr || !storage_->sanitizePath(path, safe) || newTitle == nullptr) {
    return false;
  }

  // Slugify per deck convention: lowercase alphanumerics, dashes between.
  String slug;
  bool pendingDash = false;
  for (size_t i = 0; newTitle[i] != '\0' && slug.length() < 48; i++) {
    const char c = newTitle[i];
    if (isalnum(c)) {
      if (pendingDash && slug.length() > 0) {
        slug += '-';
      }
      pendingDash = false;
      slug += static_cast<char>(tolower(c));
    } else {
      pendingDash = true;
    }
  }
  if (slug.length() == 0) {
    return false;
  }

  const int slash = safe.lastIndexOf('/');
  const String dir = safe.substring(0, slash);
  const int dot = safe.lastIndexOf('.');
  const String ext = dot > slash ? safe.substring(dot) : String(".md");

  String candidate = dir + "/" + slug + ext;
  for (int n = 2; SD_MMC.exists(candidate) && n < 10; n++) {
    candidate = dir + "/" + slug + "-" + String(n) + ext;
  }
  if (SD_MMC.exists(candidate)) {
    return false;
  }
  if (!SD_MMC.rename(safe, candidate)) {
    return false;
  }
  outNewPath = candidate;

  // A flag may have been stored under the path as listed or its sanitized
  // form; move whichever is there so the favourite survives the rename.
  bool moved = retargetFlags(safe.c_str(), candidate.c_str());
  if (!safe.equals(path)) {
    moved = retargetFlags(path, candidate.c_str()) || moved;
  }
  if (moved && !saveFlags()) {
    Serial.println("[notes] renamed, but the favourite/pin index could not be saved");
  }
  return true;
}

bool NotesService::remove(const char* path) {
  String safe;
  if (storage_ == nullptr || !storage_->sanitizePath(path, safe)) {
    return false;
  }
  AtomicFile::cleanupSiblings(SD_MMC, safe.c_str());
  if (!SD_MMC.remove(safe)) {
    return false;
  }
  bool dropped = retargetFlags(safe.c_str(), nullptr);
  if (!safe.equals(path)) {
    dropped = retargetFlags(path, nullptr) || dropped;
  }
  if (dropped && !saveFlags()) {
    Serial.println("[notes] deleted, but the favourite/pin index could not be saved");
  }
  return true;
}

void NotesService::seedWelcomeIfEmpty() {
  if (storage_ == nullptr || !storage_->card()->writable()) {
    return;
  }
  fs::File dir = SD_MMC.open(paths::kNotesText);
  if (!dir || !dir.isDirectory()) {
    return;
  }
  fs::File first = dir.openNextFile();
  const bool empty = !first;
  if (first) {
    first.close();
  }
  dir.close();
  if (!empty) {
    return;
  }
  const String welcome =
      "# Welcome to Little Cube\n\n"
      "This cube reads notes from the SD card.\n\n"
      "- Plug into USB and use `notes write <name>.md` to type new notes\n"
      "- Swipe up/down to scroll, left/right for next/previous\n"
      "- Notes also sync with the writer deck by swapping the card\n";
  write("/littlecube/notes/text/welcome.md", welcome);
  Serial.println("[notes] seeded welcome note");
}
