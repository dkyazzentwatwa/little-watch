#include "ContactsService.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <string.h>
#include <strings.h>  // strcasecmp

#include "../hardware/SdCardAdapter.h"
#include "../storage/AtomicFile.h"
#include "../storage/SdStorage.h"
#include "../storage/StoragePaths.h"

namespace {

constexpr const char* kFlagsPath = "/contacts_flags.json";
// Bounds file growth from serial input; the cache is far smaller, but the
// file itself must not be allowed to grow without limit.
constexpr size_t kMaxContacts = 512;

enum class FileLoad : uint8_t { Missing, Ok, Corrupt };

// Copies a JSON string into a fixed field, dropping control characters (an
// imported address book can contain anything and these end up on the AMOLED).
void copyField(char* dst, size_t dstSize, const char* src) {
  if (dst == nullptr || dstSize == 0) {
    return;
  }
  size_t w = 0;
  if (src != nullptr) {
    for (size_t i = 0; src[i] != '\0' && w + 1 < dstSize; i++) {
      const unsigned char c = static_cast<unsigned char>(src[i]);
      dst[w++] = (c < 0x20 || c == 0x7F) ? ' ' : src[i];
    }
  }
  dst[w] = '\0';
}

void trimInPlace(char* s) {
  if (s == nullptr) {
    return;
  }
  while (s[0] == ' ') {
    memmove(s, s + 1, strlen(s));
  }
  size_t n = strlen(s);
  while (n > 0 && s[n - 1] == ' ') {
    s[--n] = '\0';
  }
}

// tolower() on a plain char is UB for bytes >= 0x80 (char is signed on
// xtensa) — every call casts to unsigned char first.
char lowerAscii(char c) {
  return static_cast<char>(tolower(static_cast<unsigned char>(c)));
}

bool containsCaseInsensitive(const char* haystack, const char* needle) {
  if (haystack == nullptr || needle == nullptr || needle[0] == '\0') {
    return false;
  }
  const size_t hn = strlen(haystack);
  const size_t nn = strlen(needle);
  if (nn > hn) {
    return false;
  }
  for (size_t i = 0; i + nn <= hn; i++) {
    size_t j = 0;
    while (j < nn && lowerAscii(haystack[i + j]) == lowerAscii(needle[j])) {
      j++;
    }
    if (j == nn) {
      return true;
    }
  }
  return false;
}

const char* stringOr(JsonVariantConst v, const char* fallback) {
  return v.is<const char*>() ? v.as<const char*>() : fallback;
}

// A record with no usable "id" still needs a handle, so it gets a synthetic
// one. The file is never rewritten just to add ids.
void idForRecord(JsonVariantConst v, uint16_t fileIndex, char* out, size_t outSize) {
  const char* id = stringOr(v["id"], nullptr);
  if (id != nullptr && id[0] != '\0') {
    copyField(out, outSize, id);
    trimInPlace(out);
    if (out[0] != '\0') {
      return;
    }
  }
  snprintf(out, outSize, "#%u", (unsigned)(fileIndex + 1));
}

FileLoad readDoc(const String& path, JsonDocument& doc) {
  if (!SD_MMC.exists(path)) {
    return FileLoad::Missing;
  }
  fs::File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    return FileLoad::Missing;
  }
  if (f.isDirectory()) {
    f.close();
    return FileLoad::Corrupt;
  }
  const size_t sz = f.size();
  if (sz == 0) {
    f.close();
    return FileLoad::Missing;  // empty file == empty address book
  }
  if (sz > ContactsService::kMaxFileBytes) {
    f.close();
    Serial.printf("[contacts] %s is %u B (max %u); skipped\n", path.c_str(), (unsigned)sz,
                  (unsigned)ContactsService::kMaxFileBytes);
    return FileLoad::Corrupt;
  }
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[contacts] %s unreadable (%s); left untouched\n", path.c_str(), err.c_str());
    return FileLoad::Corrupt;
  }
  if (!doc.is<JsonArray>() && !doc.is<JsonObject>()) {
    Serial.printf("[contacts] %s is not an object or array; left untouched\n", path.c_str());
    return FileLoad::Corrupt;
  }
  return FileLoad::Ok;
}

// On-disk position of a contact, or -1. Records without a usable name are
// skipped exactly as they are on load, so ids stay consistent everywhere.
int indexOfId(JsonArray arr, const char* id) {
  if (arr.isNull() || id == nullptr) {
    return -1;
  }
  uint16_t fileIndex = 0;
  for (JsonVariant v : arr) {
    const uint16_t here = fileIndex++;
    if (!v.is<JsonObject>()) {
      continue;
    }
    const char* name = stringOr(v["name"], nullptr);
    if (name == nullptr || name[0] == '\0') {
      continue;
    }
    char recordId[sizeof(ContactInfo::id)];
    idForRecord(v, here, recordId, sizeof(recordId));
    if (strcmp(recordId, id) == 0) {
      return static_cast<int>(here);
    }
  }
  return -1;
}

JsonArray contactsOf(JsonDocument& doc) {
  if (doc.is<JsonArray>()) {
    return doc.as<JsonArray>();
  }
  return doc["contacts"].as<JsonArray>();  // null array when absent or wrong type
}

}  // namespace

void ContactsService::begin(SdStorage* storage) {
  storage_ = storage;
  invalidate();
  // Repair the flags index if a write was interrupted, before reading it.
  AtomicFile::cleanupSiblings(LittleFS, kFlagsPath);
  loadFlags();
  // No SD access here: begin() must succeed with no card (spec §43). The
  // kernel calls reload() from the SdMounted hook.
}

void ContactsService::invalidate() {
  count_ = 0;
  total_ = 0;
  loaded_ = false;
  corrupt_ = false;
}

bool ContactsService::ready() const {
  return storage_ != nullptr && storage_->card() != nullptr && storage_->card()->mounted();
}

bool ContactsService::filePath(String& out) const {
  out = "";
  if (storage_ == nullptr) {
    return false;
  }
  const String raw = String(paths::kContacts) + "/contacts.json";
  return storage_->sanitizePath(raw.c_str(), out);
}

// --- favourites (LittleFS index, never on the card) -------------------------

int ContactsService::findFavorite(const char* id) const {
  if (id == nullptr) {
    return -1;
  }
  for (size_t i = 0; i < favoriteCount_; i++) {
    if (favorites_[i].equals(id)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool ContactsService::isFavorite(const char* id) const { return findFavorite(id) >= 0; }

void ContactsService::loadFlags() {
  favoriteCount_ = 0;
  fs::File f = LittleFS.open(kFlagsPath, FILE_READ);
  if (!f) {
    return;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[contacts] flags index unreadable (%s); starting clean\n", err.c_str());
    return;
  }
  for (JsonVariant v : doc["fav"].as<JsonArray>()) {
    if (favoriteCount_ < kMaxFavorites && v.is<const char*>()) {
      favorites_[favoriteCount_++] = v.as<const char*>();
    }
  }
}

bool ContactsService::saveFlags() {
  JsonDocument doc;
  JsonArray fav = doc["fav"].to<JsonArray>();
  for (size_t i = 0; i < favoriteCount_; i++) {
    fav.add(favorites_[i]);
  }
  String body;
  serializeJson(doc, body);
  return AtomicFile::writeAll(LittleFS, kFlagsPath, body);
}

bool ContactsService::setFavorite(const char* id, bool on) {
  if (id == nullptr || id[0] == '\0') {
    return false;
  }
  const int at = findFavorite(id);
  if (on && at < 0) {
    if (favoriteCount_ >= kMaxFavorites) {
      return false;
    }
    favorites_[favoriteCount_++] = id;
  } else if (!on && at >= 0) {
    for (size_t i = static_cast<size_t>(at); i + 1 < favoriteCount_; i++) {
      favorites_[i] = favorites_[i + 1];
    }
    favoriteCount_--;
  }
  if (!saveFlags()) {
    return false;
  }
  for (size_t i = 0; i < count_; i++) {
    if (strcmp(cache_[i].id, id) == 0) {
      cache_[i].favorite = on;
    }
  }
  sortCache();
  return true;
}

// --- cache ------------------------------------------------------------------

void ContactsService::sortCache() {
  // Favourites float to the top, then case-insensitive name order. Insertion
  // sort: kMaxCached is 32.
  for (size_t i = 1; i < count_; i++) {
    const ContactInfo key = cache_[i];
    size_t j = i;
    while (j > 0) {
      const ContactInfo& prev = cache_[j - 1];
      bool after = false;
      if (prev.favorite != key.favorite) {
        after = !prev.favorite;  // key is a favourite, prev is not
      } else {
        after = strcasecmp(prev.name, key.name) > 0;
      }
      if (!after) {
        break;
      }
      cache_[j] = prev;
      j--;
    }
    cache_[j] = key;
  }
}

bool ContactsService::reload() {
  invalidate();
  String path;
  if (!ready() || !filePath(path)) {
    return false;  // no card: empty, honestly-unloaded state
  }

  JsonDocument doc;
  const FileLoad st = readDoc(path, doc);
  if (st == FileLoad::Corrupt) {
    corrupt_ = true;
    return false;
  }
  loaded_ = true;
  if (st == FileLoad::Missing) {
    return true;
  }

  JsonArray arr = contactsOf(doc);
  if (arr.isNull()) {
    return true;
  }

  uint16_t fileIndex = 0;
  for (JsonVariant v : arr) {
    const uint16_t here = fileIndex++;
    if (!v.is<JsonObject>()) {
      continue;
    }
    const char* name = stringOr(v["name"], nullptr);
    if (name == nullptr || name[0] == '\0') {
      continue;
    }
    total_++;
    if (count_ >= kMaxCached) {
      continue;  // counted, not cached — hiddenCount() reports these
    }
    ContactInfo& c = cache_[count_];
    c = ContactInfo();
    c.fileIndex = here;
    idForRecord(v, here, c.id, sizeof(c.id));
    copyField(c.name, sizeof(c.name), name);
    copyField(c.phone, sizeof(c.phone), stringOr(v["phone"], ""));
    copyField(c.email, sizeof(c.email), stringOr(v["email"], ""));
    c.hasNote = stringOr(v["note"], "")[0] != '\0';
    c.hasVoice = stringOr(v["voice"], "")[0] != '\0';
    c.favorite = isFavorite(c.id);
    count_++;
  }
  sortCache();
  return true;
}

const ContactInfo* ContactsService::at(size_t index) const {
  return index < count_ ? &cache_[index] : nullptr;
}

size_t ContactsService::list(ContactInfo* out, size_t maxItems, size_t* totalOut) const {
  if (totalOut != nullptr) {
    *totalOut = total_;  // the TRUE count, so the UI can say "+N more"
  }
  if (out == nullptr || maxItems == 0) {
    return 0;
  }
  const size_t n = count_ < maxItems ? count_ : maxItems;
  for (size_t i = 0; i < n; i++) {
    out[i] = cache_[i];
  }
  return n;
}

// --- file-backed reads ------------------------------------------------------

size_t ContactsService::search(const char* query, ContactInfo* out, size_t maxItems,
                               size_t* totalOut) {
  if (totalOut != nullptr) {
    *totalOut = 0;
  }
  if (query == nullptr || query[0] == '\0') {
    return 0;
  }
  char needle[40];
  copyField(needle, sizeof(needle), query);
  trimInPlace(needle);
  if (needle[0] == '\0') {
    return 0;
  }

  String path;
  if (!ready() || !filePath(path)) {
    return 0;
  }
  JsonDocument doc;
  if (readDoc(path, doc) != FileLoad::Ok) {
    return 0;
  }
  JsonArray arr = contactsOf(doc);
  if (arr.isNull()) {
    return 0;
  }

  size_t written = 0;
  size_t matches = 0;
  uint16_t fileIndex = 0;
  for (JsonVariant v : arr) {
    const uint16_t here = fileIndex++;
    if (!v.is<JsonObject>()) {
      continue;
    }
    const char* name = stringOr(v["name"], nullptr);
    if (name == nullptr || name[0] == '\0') {
      continue;
    }
    if (!containsCaseInsensitive(name, needle)) {
      continue;
    }
    matches++;
    if (out == nullptr || written >= maxItems) {
      continue;  // still counted: totalOut stays truthful
    }
    ContactInfo& c = out[written];
    c = ContactInfo();
    c.fileIndex = here;
    idForRecord(v, here, c.id, sizeof(c.id));
    copyField(c.name, sizeof(c.name), name);
    copyField(c.phone, sizeof(c.phone), stringOr(v["phone"], ""));
    copyField(c.email, sizeof(c.email), stringOr(v["email"], ""));
    c.hasNote = stringOr(v["note"], "")[0] != '\0';
    c.hasVoice = stringOr(v["voice"], "")[0] != '\0';
    c.favorite = isFavorite(c.id);
    written++;
  }
  if (totalOut != nullptr) {
    *totalOut = matches;
  }
  return written;
}

bool ContactsService::get(const char* id, ContactDetail& out) {
  out = ContactDetail();
  if (id == nullptr || id[0] == '\0') {
    return false;
  }
  String path;
  if (!ready() || !filePath(path)) {
    return false;
  }
  JsonDocument doc;
  if (readDoc(path, doc) != FileLoad::Ok) {
    return false;
  }
  JsonArray arr = contactsOf(doc);
  const int at = indexOfId(arr, id);
  if (at < 0) {
    return false;
  }
  JsonVariantConst v = arr[static_cast<size_t>(at)];
  idForRecord(v, static_cast<uint16_t>(at), out.id, sizeof(out.id));
  copyField(out.name, sizeof(out.name), stringOr(v["name"], ""));
  copyField(out.phone, sizeof(out.phone), stringOr(v["phone"], ""));
  copyField(out.email, sizeof(out.email), stringOr(v["email"], ""));
  copyField(out.note, sizeof(out.note), stringOr(v["note"], ""));
  copyField(out.voice, sizeof(out.voice), stringOr(v["voice"], ""));
  out.favorite = isFavorite(out.id);
  return true;
}

// --- mutation ---------------------------------------------------------------

bool ContactsService::add(const char* name, const char* phone, const char* email,
                          const char* note, char* outId, size_t outIdSize) {
  if (outId != nullptr && outIdSize > 0) {
    outId[0] = '\0';
  }
  char safeName[sizeof(ContactInfo::name)];
  copyField(safeName, sizeof(safeName), name);
  trimInPlace(safeName);
  if (safeName[0] == '\0') {
    return false;
  }
  char safePhone[sizeof(ContactInfo::phone)];
  char safeEmail[sizeof(ContactInfo::email)];
  char safeNote[sizeof(ContactDetail::note)];
  copyField(safePhone, sizeof(safePhone), phone != nullptr ? phone : "");
  copyField(safeEmail, sizeof(safeEmail), email != nullptr ? email : "");
  copyField(safeNote, sizeof(safeNote), note != nullptr ? note : "");
  trimInPlace(safePhone);
  trimInPlace(safeEmail);
  trimInPlace(safeNote);

  String path;
  if (!ready() || !filePath(path) || !storage_->card()->writable()) {
    return false;
  }

  JsonDocument doc;
  const FileLoad st = readDoc(path, doc);
  if (st == FileLoad::Corrupt) {
    Serial.printf("[contacts] refusing to write over unreadable %s\n", path.c_str());
    return false;
  }
  JsonArray arr;
  if (st == FileLoad::Missing) {
    doc.clear();
    arr = doc["contacts"].to<JsonArray>();
  } else {
    arr = contactsOf(doc);
    if (arr.isNull()) {
      if (!doc.is<JsonObject>()) {
        return false;
      }
      arr = doc["contacts"].to<JsonArray>();
    }
  }
  if (arr.size() >= kMaxContacts) {
    Serial.printf("[contacts] address book is full (%u)\n", (unsigned)kMaxContacts);
    return false;
  }

  // Generate "cNNN" one past the highest existing numeric id, so ids stay
  // unique even after deletions.
  unsigned highest = 0;
  for (JsonVariant v : arr) {
    const char* existing = stringOr(v["id"], "");
    if (existing[0] != 'c') {
      continue;
    }
    bool digits = existing[1] != '\0';
    for (const char* p = existing + 1; *p != '\0'; p++) {
      if (!isdigit(static_cast<unsigned char>(*p))) {
        digits = false;
        break;
      }
    }
    if (digits) {
      const unsigned n = strtoul(existing + 1, nullptr, 10);
      if (n > highest) {
        highest = n;
      }
    }
  }
  char newId[sizeof(ContactInfo::id)];
  snprintf(newId, sizeof(newId), "c%03u", highest + 1);

  JsonObject o = arr.add<JsonObject>();
  if (o.isNull()) {
    return false;
  }
  o["id"] = newId;
  o["name"] = safeName;
  o["phone"] = safePhone;
  o["email"] = safeEmail;
  o["note"] = safeNote;
  o["voice"] = "";

  if (!SD_MMC.exists(paths::kContacts)) {
    SD_MMC.mkdir(paths::kContacts);
  }
  String body;
  serializeJson(doc, body);
  if (!AtomicFile::writeAll(SD_MMC, path.c_str(), body)) {
    return false;
  }
  if (outId != nullptr && outIdSize > 0) {
    snprintf(outId, outIdSize, "%s", newId);
  }
  reload();
  return true;
}

bool ContactsService::update(const char* id, const char* field, const char* value) {
  if (id == nullptr || id[0] == '\0' || field == nullptr) {
    return false;
  }
  const bool isName = strcmp(field, "name") == 0;
  const bool isPhone = strcmp(field, "phone") == 0;
  const bool isEmail = strcmp(field, "email") == 0;
  const bool isNote = strcmp(field, "note") == 0;
  const bool isVoice = strcmp(field, "voice") == 0;
  if (!isName && !isPhone && !isEmail && !isNote && !isVoice) {
    return false;
  }

  char safe[sizeof(ContactDetail::note)];
  size_t limit = sizeof(safe);
  if (isName) {
    limit = sizeof(ContactInfo::name);
  } else if (isPhone) {
    limit = sizeof(ContactInfo::phone);
  } else if (isEmail) {
    limit = sizeof(ContactInfo::email);
  } else if (isVoice) {
    limit = sizeof(ContactDetail::voice);
  }
  copyField(safe, limit, value != nullptr ? value : "");
  trimInPlace(safe);
  if (isName && safe[0] == '\0') {
    return false;  // a contact without a name is invisible to every listing
  }

  String path;
  if (!ready() || !filePath(path) || !storage_->card()->writable()) {
    return false;
  }
  // A voice path is user input: it must be inside the allowed roots.
  if (isVoice && safe[0] != '\0') {
    String sanitized;
    if (!storage_->sanitizePath(safe, sanitized)) {
      return false;
    }
    snprintf(safe, sizeof(ContactDetail::voice), "%s", sanitized.c_str());
  }

  JsonDocument doc;
  if (readDoc(path, doc) != FileLoad::Ok) {
    return false;
  }
  JsonArray arr = contactsOf(doc);
  const int at = indexOfId(arr, id);
  if (at < 0) {
    return false;
  }
  JsonObject o = arr[static_cast<size_t>(at)].as<JsonObject>();
  if (o.isNull()) {
    return false;
  }
  // Use a literal key: ArduinoJson may store the key by reference, and
  // `field` points into the caller's (serial line) buffer.
  const char* key = isName    ? "name"
                    : isPhone ? "phone"
                    : isEmail ? "email"
                    : isNote  ? "note"
                              : "voice";
  o[key] = safe;
  String body;
  serializeJson(doc, body);
  if (!AtomicFile::writeAll(SD_MMC, path.c_str(), body)) {
    return false;
  }
  reload();
  return true;
}

bool ContactsService::remove(const char* id) {
  if (id == nullptr || id[0] == '\0') {
    return false;
  }
  String path;
  if (!ready() || !filePath(path)) {
    return false;
  }
  if (!storage_->card()->canDelete()) {
    return false;
  }
  JsonDocument doc;
  if (readDoc(path, doc) != FileLoad::Ok) {
    return false;
  }
  JsonArray arr = contactsOf(doc);
  const int at = indexOfId(arr, id);
  if (at < 0) {
    return false;
  }
  arr.remove(static_cast<size_t>(at));
  String body;
  serializeJson(doc, body);
  if (!AtomicFile::writeAll(SD_MMC, path.c_str(), body)) {
    return false;
  }
  if (findFavorite(id) >= 0) {
    setFavorite(id, false);
  }
  reload();
  return true;
}
