#pragma once

#include <Arduino.h>

class SdStorage;

// E-reader backend (plain-text books). Books are clean .txt/.md files in
// /littlecube/documents. A book can be MEGABYTES, so this service NEVER holds
// a whole book: list() scans the directory, and readPage() streams exactly one
// screen-page at a time by byte offset. Resume positions (one byte offset per
// book) live in a small LittleFS JSON index — never inside the book, and never
// on the SD card, so a swapped card cannot corrupt them.
//
// This service holds NO open file handle across calls — readPage() opens and
// closes within the call — so it never blocks a safe-eject and needs no
// quiesce hook.
struct BookInfo {
  char path[128] = "";
  char title[64] = "";
  uint32_t sizeBytes = 0;
  uint32_t resumeOffset = 0;  // 0 when the book has never been opened
};

class BookService {
 public:
  void begin(SdStorage* storage);

  // Re-reads the resume-position index from LittleFS. The book list is scanned
  // on demand by list(), so there is no cached list to refresh; this exists for
  // API symmetry and for callers that changed the index out of band.
  void reload();

  // Fills up to maxBooks entries and returns how many were written. `totalOut`,
  // when given, receives how many books exist on the card — the caller needs it
  // to admit it is showing a window ("+N more"), not the whole card.
  size_t list(BookInfo* out, size_t maxBooks, size_t* totalOut = nullptr);

  // Size of a book in bytes (0 if unreadable / card gone). Cheap: one open.
  // Used for the progress indicator and to clamp a stale resume offset.
  uint32_t fileSize(const char* path);

  // Streams one screen-page of text from `path` beginning at byte `startOffset`.
  // Lays out up to `maxLines` lines `charsPerLine` characters wide using the
  // EXACT greedy-wrap rule of widgets::textBlock, writes the sanitized ASCII
  // page text (NUL-terminated, <= outCap-1 bytes) into `out`, reports in
  // `nextOffset` the byte offset at which the following page begins, and sets
  // `atEnd` true when this page reaches end of file.
  //
  // Returns the number of bytes written to `out`. A return of 0 means either a
  // clean end (out is empty and `atEnd` is true) or a read failure (out is
  // empty and `atEnd` is false) — the caller distinguishes via `atEnd`.
  //
  // Peak RAM is one bounded buffer (the caller's `out`); the whole book is
  // never resident. `charsPerLine`/`maxLines` come from the caller's font size
  // and screen geometry and MUST match what it later passes to textBlock, or
  // the drawn page and the computed nextOffset will disagree.
  size_t readPage(const char* path, uint32_t startOffset, uint16_t charsPerLine,
                  uint16_t maxLines, char* out, size_t outCap, uint32_t& nextOffset,
                  bool& atEnd);

  // Resume position (byte offset) per book, persisted to LittleFS.
  uint32_t position(const char* path) const;
  void setPosition(const char* path, uint32_t offset);
  void clearPosition(const char* path);

  // Derives a human title from a filename: strips directory and extension,
  // maps '-'/'_' to spaces, capitalizes the first letter.
  static void titleFromPath(const char* path, char* out, size_t outSize);

 private:
  void loadPositions();
  bool savePositions();
  int findPosition(const char* path) const;

  SdStorage* storage_ = nullptr;

  // Remembered resume positions. Capped like NotesService's flags index; when
  // full, the oldest entry is evicted (a private cache with no on-screen list,
  // so losing an old position merely resumes that book at page one).
  static constexpr size_t kMaxRemembered = 24;
  String posPaths_[kMaxRemembered];
  uint32_t posOffsets_[kMaxRemembered] = {0};
  size_t posCount_ = 0;
};
