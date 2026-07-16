#include "NotesService.h"

// TODO(task-9): listing across both roots, title derivation (strip NNNN_,
// dashes -> spaces), atomic writes, LittleFS favorites/pins index.

void NotesService::begin(SdStorage* storage) {
  storage_ = storage;
}

size_t NotesService::list(NoteInfo* out, size_t maxNotes) {
  (void)out;
  (void)maxNotes;
  return 0;
}

bool NotesService::read(const char* path, String& outBody, size_t maxBytes) {
  (void)path;
  (void)maxBytes;
  outBody = "";
  return false;
}

bool NotesService::write(const char* path, const String& body) {
  (void)path;
  (void)body;
  return false;
}

bool NotesService::append(const char* path, const String& text) {
  (void)path;
  (void)text;
  return false;
}

bool NotesService::remove(const char* path) {
  (void)path;
  return false;
}

bool NotesService::rename(const char* path, const char* newTitle) {
  (void)path;
  (void)newTitle;
  return false;
}

bool NotesService::setFavorite(const char* path, bool on) {
  (void)path;
  (void)on;
  return false;
}

bool NotesService::setPinned(const char* path, bool on) {
  (void)path;
  (void)on;
  return false;
}
