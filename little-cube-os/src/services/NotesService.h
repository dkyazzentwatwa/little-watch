#pragma once

#include <Arduino.h>

class SdStorage;

// Notes (spec §15): the cube is a note *reader* and capture device. Text
// notes are clean .txt/.md files in /littlecube/notes/text plus the
// CrowPanel deck tree /cypher-puter/desk/notes{,/daily,/scraps,/<notebook>}
// (one level deep, .tmp/.bak siblings ignored) so a swapped card
// round-trips with the writer deck. Favorites/pins live in a small
// LittleFS JSON index — never inside note bodies.
struct NoteInfo {
  char path[128] = "";
  char title[64] = "";
  size_t sizeBytes = 0;
  bool favorite = false;
  bool pinned = false;
  bool fromDeck = false;
};

class NotesService {
 public:
  void begin(SdStorage* storage);

  // Restores notes whose only copy is a leftover .bak from an interrupted
  // write, and reaps stray .tmp files. Run once per card mount.
  void recoverInterrupted();

  // Fills out up to maxNotes entries (pinned first) and returns how many were
  // written. `totalOut`, when given, receives how many notes exist — the
  // caller needs that to admit it is showing a window, not the whole card.
  size_t list(NoteInfo* out, size_t maxNotes, size_t* totalOut = nullptr);

  bool read(const char* path, String& outBody, size_t maxBytes, bool& truncated);
  bool write(const char* path, const String& body);
  bool append(const char* path, const String& text);
  bool remove(const char* path);

  // Renames by title: slugified filename (deck convention) in the same
  // directory, same extension, -2/-3... on collision. Returns the new path.
  bool rename(const char* path, const char* newTitle, String& outNewPath);

  bool setFavorite(const char* path, bool on);
  bool setPinned(const char* path, bool on);
  bool isFavorite(const char* path) const;
  bool isPinned(const char* path) const;

  // First-boot nicety: drops a welcome note when the cube tree has none.
  void seedWelcomeIfEmpty();

  // Derives a human title from a filename: strips the extension and any
  // NNNN_ deck prefix, maps -/_ to spaces.
  static void titleFromPath(const char* path, char* out, size_t outSize);

 private:
  void loadFlags();
  bool saveFlags();
  int findFlag(const String* arr, size_t count, const char* path) const;
  // Moves a flagged note's entry to newPath, or drops it when newPath is
  // null. Returns true when something changed (the caller then persists).
  bool retargetFlags(const char* oldPath, const char* newPath);

  SdStorage* storage_ = nullptr;

  static constexpr size_t kMaxFlagged = 24;
  String favorites_[kMaxFlagged];
  size_t favoriteCount_ = 0;
  String pinned_[kMaxFlagged];
  size_t pinnedCount_ = 0;
};
