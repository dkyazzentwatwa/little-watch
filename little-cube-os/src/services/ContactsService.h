#pragma once

#include <Arduino.h>

class SdStorage;

// Contacts (spec §26): reference-first. List, search, details, favorites,
// associate a voice note. No dialling, no messaging — the cube never implies
// it can call anyone. Editing happens over serial or by importing a file.
//
// ---------------------------------------------------------------------------
// ON-DISK SCHEMA — /littlecube/contacts/contacts.json (one file, all contacts)
// ---------------------------------------------------------------------------
//   {
//     "contacts": [
//       {
//         "id":    "c001",
//         "name":  "Ada Lovelace",
//         "phone": "+44 20 7946 0000",
//         "email": "ada@example.org",
//         "note":  "met at the analytical engine demo",
//         "voice": "/littlecube/notes/audio/ada.wav"
//       }
//     ]
//   }
//
//   id     stable handle used by every method here and by the serial family.
//          Any string; only the first 11 characters are significant. A record
//          with no id gets the synthetic id "#<n>" (n = 1-based file position)
//          so it is still addressable — the file is NOT rewritten to add one.
//          Ids this service generates look like "c001", "c002", ...
//   name   required and non-empty; a record without one is skipped on load.
//   phone / email / note / voice   all optional.
//   voice  absolute SD path of an associated recording; validated through
//          SdStorage::sanitizePath() on write and ignored if it escapes the
//          allowed roots.
//
//   A bare top-level array is ALSO accepted on read. A rewrite preserves
//   whichever top-level form the file had; a file this service creates uses
//   the object form, and unknown keys on a record survive a rewrite.
//
//   FAVOURITES ARE NOT IN THIS FILE. They live in a LittleFS index,
//   /contacts_flags.json ({"fav":["c001", ...]}), exactly like NotesService's
//   flags file, so the SD data stays a clean, portable address book.
//
//   A contacts.json that fails to parse is logged and skipped. It is never
//   deleted and never overwritten: corrupt() latches and every mutating
//   method refuses until the file is fixed or removed by hand (spec §43).
//
// ---------------------------------------------------------------------------
// THREADING / BLOCKING + RAM
// ---------------------------------------------------------------------------
//   Pull-only: no update(deltaMs), no task. reload(), search(), get() and all
//   mutations do SD I/O — call them from App::onOpen()/refresh(), never from
//   render(). list(), total(), isFavorite() serve RAM and are render-safe.
//
//   The cache holds at most kMaxCached contacts (130 B each => 4160 B; the
//   whole service object measures ~4.6 KB of .bss including the favourites
//   index). total() always reports the true on-disk count, so a UI must render
//   a "+N more" affordance rather than pretending the list is complete.
//   search() reads the FILE, not the cache, so contacts past the cache
//   window are still findable.

// Cached list row. 130 bytes. Detail-only fields (note, voice) are not here
// on purpose — fetch them with get() when a detail view opens.
struct ContactInfo {
  char id[12] = "";
  char name[40] = "";
  char phone[24] = "";
  char email[48] = "";
  uint16_t fileIndex = 0;  // position in the on-disk array
  bool favorite = false;
  bool hasNote = false;
  bool hasVoice = false;
};

// Full record. ~382 bytes — caller-owned, never stored in an array here.
struct ContactDetail {
  char id[12] = "";
  char name[40] = "";
  char phone[24] = "";
  char email[48] = "";
  char note[160] = "";
  char voice[96] = "";
  bool favorite = false;
};

class ContactsService {
 public:
  static constexpr size_t kMaxCached = 32;
  static constexpr size_t kMaxFavorites = 24;
  // Refuse to parse a contacts file bigger than this so malformed or hostile
  // input cannot exhaust the heap (spec §43).
  static constexpr size_t kMaxFileBytes = 32768;

  void begin(SdStorage* storage);

  // Re-reads contacts.json into the cache. Safe with no card (leaves an empty
  // unloaded state and returns false). Call on SdMounted and after an import.
  bool reload();
  void invalidate();  // drop the cache without touching the card

  bool loaded() const { return loaded_; }
  bool corrupt() const { return corrupt_; }
  size_t total() const { return total_; }   // true on-disk count
  size_t cached() const { return count_; }  // <= kMaxCached
  size_t hiddenCount() const { return total_ > count_ ? total_ - count_ : 0; }

  // Zero-copy read of a cached row (favourites first, then name order).
  // Returns null past cached(). Prefer this over list() when you only need to
  // iterate — a second ContactInfo array costs another 4 KB.
  const ContactInfo* at(size_t index) const;

  // Copies up to maxItems cached rows (favourites first, then name order).
  // *totalOut receives the TRUE total, which may exceed maxItems.
  size_t list(ContactInfo* out, size_t maxItems, size_t* totalOut) const;

  // Case-insensitive substring match on name, scanning the whole FILE so
  // results are not limited to the cache. *totalOut receives the true number
  // of matches. An empty/null query matches nothing. Does SD I/O.
  size_t search(const char* query, ContactInfo* out, size_t maxItems, size_t* totalOut);

  // Full record by id. Does SD I/O.
  bool get(const char* id, ContactDetail& out);

  // Creates a contact and writes back the generated id. name is required.
  bool add(const char* name, const char* phone, const char* email, const char* note,
           char* outId, size_t outIdSize);
  // field is one of: name, phone, email, note, voice. A null/empty value
  // clears the field (except name, which must stay non-empty). "voice" is
  // run through SdStorage::sanitizePath().
  bool update(const char* id, const char* field, const char* value);
  bool remove(const char* id);

  bool setFavorite(const char* id, bool on);
  bool isFavorite(const char* id) const;

  // Absolute path of contacts.json — public so the serial import/export
  // verbs can name it. Goes through sanitizePath().
  bool filePath(String& out) const;

 private:
  bool ready() const;
  void loadFlags();
  bool saveFlags();
  int findFavorite(const char* id) const;
  void sortCache();

  SdStorage* storage_ = nullptr;

  ContactInfo cache_[kMaxCached];
  size_t count_ = 0;
  size_t total_ = 0;
  bool loaded_ = false;
  bool corrupt_ = false;

  String favorites_[kMaxFavorites];
  size_t favoriteCount_ = 0;
};
