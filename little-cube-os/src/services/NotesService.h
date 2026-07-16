#pragma once

#include <Arduino.h>

class SdStorage;

// Notes: reading and writing .txt/.md from the cube tree
// (/littlecube/notes/text) and the CrowPanel deck tree
// (/cypher-puter/desk/notes — clean bodies, no frontmatter, .tmp/.bak
// siblings tolerated). Favorites/pins live in a small LittleFS index, never
// inside note bodies.
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

  // Fills out up to maxNotes entries; returns the count found.
  size_t list(NoteInfo* out, size_t maxNotes);

  bool read(const char* path, String& outBody, size_t maxBytes);
  bool write(const char* path, const String& body);
  bool append(const char* path, const String& text);
  bool remove(const char* path);
  bool rename(const char* path, const char* newTitle);

  bool setFavorite(const char* path, bool on);
  bool setPinned(const char* path, bool on);

 private:
  SdStorage* storage_ = nullptr;
};
