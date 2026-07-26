#include "CalendarService.h"

#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <string.h>

#include "../hardware/SdCardAdapter.h"
#include "../storage/AtomicFile.h"
#include "../storage/SdStorage.h"
#include "../storage/StoragePaths.h"
#include "TimeService.h"

namespace {

enum class DayLoad : uint8_t { Missing, Ok, Corrupt };

// Copies a JSON string into a fixed field, dropping control characters (an
// imported file can contain anything and these end up on the AMOLED).
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

// Strict "HH:MM" 24-hour. Anything else is untimed.
bool parseHhMm(const char* s, uint16_t& outMinutes) {
  outMinutes = CalendarEvent::kUntimed;
  if (s == nullptr || strlen(s) != 5 || s[2] != ':') {
    return false;
  }
  static const int kDigitPos[] = {0, 1, 3, 4};
  for (size_t k = 0; k < 4; k++) {
    if (!isdigit(static_cast<unsigned char>(s[kDigitPos[k]]))) {
      return false;
    }
  }
  const int hh = (s[0] - '0') * 10 + (s[1] - '0');
  const int mm = (s[3] - '0') * 10 + (s[4] - '0');
  if (hh > 23 || mm > 59) {
    return false;
  }
  outMinutes = static_cast<uint16_t>(hh * 60 + mm);
  return true;
}

int daysInMonth(int year, int month) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
    return 29;
  }
  return kDays[month - 1];
}

DayLoad readDayDoc(const String& path, JsonDocument& doc) {
  if (!SD_MMC.exists(path)) {
    return DayLoad::Missing;
  }
  fs::File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    return DayLoad::Missing;
  }
  if (f.isDirectory()) {
    f.close();
    return DayLoad::Corrupt;
  }
  const size_t sz = f.size();
  if (sz == 0) {
    f.close();
    return DayLoad::Missing;  // an empty file is an empty day, not a failure
  }
  if (sz > CalendarService::kMaxDayFileBytes) {
    f.close();
    Serial.printf("[calendar] %s is %u B (max %u); skipped\n", path.c_str(), (unsigned)sz,
                  (unsigned)CalendarService::kMaxDayFileBytes);
    return DayLoad::Corrupt;
  }
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    // Never delete or rewrite it — the user's data may still be recoverable
    // by hand (spec §43: no crash on corrupt files).
    Serial.printf("[calendar] %s unreadable (%s); left untouched\n", path.c_str(), err.c_str());
    return DayLoad::Corrupt;
  }
  if (!doc.is<JsonArray>() && !doc.is<JsonObject>()) {
    Serial.printf("[calendar] %s is not an object or array; left untouched\n", path.c_str());
    return DayLoad::Corrupt;
  }
  return DayLoad::Ok;
}

// Both accepted shapes funnel through here. Returns a null array when the
// document simply has no events (that is an empty day, not an error).
JsonArray eventsOf(JsonDocument& doc) {
  if (doc.is<JsonArray>()) {
    return doc.as<JsonArray>();
  }
  return doc["events"].as<JsonArray>();  // null array when absent or wrong type
}

// Creates the events array for a freshly made document (object form).
JsonArray makeEventsArray(JsonDocument& doc, const char* isoDate) {
  doc["date"] = isoDate;
  return doc["events"].to<JsonArray>();
}

}  // namespace

void CalendarService::begin(SdStorage* storage, TimeService* time) {
  storage_ = storage;
  time_ = time;
  clearCache();
  // Deliberately no directory scan and no file I/O here: day files are read
  // lazily, and begin() must succeed with no card at all (spec §43).
}

void CalendarService::invalidate() { clearCache(); }

void CalendarService::clearCache() {
  count_ = 0;
  total_ = 0;
  loaded_ = false;
  corrupt_ = false;
  loadedDate_[0] = '\0';
}

bool CalendarService::ready() const {
  return storage_ != nullptr && storage_->card() != nullptr && storage_->card()->mounted();
}

bool CalendarService::clockKnown() const { return time_ != nullptr && time_->valid(); }

bool CalendarService::isValidIsoDate(const char* iso) {
  if (iso == nullptr || strlen(iso) != 10 || iso[4] != '-' || iso[7] != '-') {
    return false;
  }
  static const int kDigitPos[] = {0, 1, 2, 3, 5, 6, 8, 9};
  for (size_t k = 0; k < 8; k++) {
    if (!isdigit(static_cast<unsigned char>(iso[kDigitPos[k]]))) {
      return false;
    }
  }
  const int y = (iso[0] - '0') * 1000 + (iso[1] - '0') * 100 + (iso[2] - '0') * 10 + (iso[3] - '0');
  const int m = (iso[5] - '0') * 10 + (iso[6] - '0');
  const int d = (iso[8] - '0') * 10 + (iso[9] - '0');
  if (y < 1970 || y > 2200) {
    return false;
  }
  const int dim = daysInMonth(y, m);
  return dim > 0 && d >= 1 && d <= dim;
}

bool CalendarService::todayIso(char* out, size_t outSize) const {
  return isoForOffset(0, out, outSize);
}

bool CalendarService::isoForOffset(int dayOffset, char* out, size_t outSize) const {
  if (out == nullptr || outSize < 11) {
    return false;
  }
  out[0] = '\0';
  if (!clockKnown()) {
    return false;  // first boot, clock never set — caller must handle it
  }
  struct tm t;
  if (!time_->now(t)) {
    return false;
  }
  // Anchor at local noon before shifting days: midnight does not exist on
  // some DST spring-forward dates, and 12:00 survives a +/-1 h shift. The
  // day arithmetic itself goes through mktime(), never +86400 seconds.
  t.tm_hour = 12;
  t.tm_min = 0;
  t.tm_sec = 0;
  t.tm_mday += dayOffset;
  t.tm_isdst = -1;
  if (mktime(&t) == static_cast<time_t>(-1)) {
    return false;
  }
  snprintf(out, outSize, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return true;
}

bool CalendarService::formatDayLabel(const char* iso, char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return false;
  }
  out[0] = '\0';
  if (!isValidIsoDate(iso)) {
    return false;
  }
  static const char* kWday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  struct tm t = {};
  t.tm_year = ((iso[0] - '0') * 1000 + (iso[1] - '0') * 100 + (iso[2] - '0') * 10 +
               (iso[3] - '0')) - 1900;
  t.tm_mon = (iso[5] - '0') * 10 + (iso[6] - '0') - 1;
  t.tm_mday = (iso[8] - '0') * 10 + (iso[9] - '0');
  t.tm_hour = 12;
  t.tm_isdst = -1;
  if (mktime(&t) == static_cast<time_t>(-1) || t.tm_wday < 0 || t.tm_wday > 6) {
    snprintf(out, outSize, "%s", iso);
    return true;
  }
  snprintf(out, outSize, "%s %d %s", kWday[t.tm_wday], t.tm_mday, kMon[t.tm_mon]);
  return true;
}

bool CalendarService::dayFilePath(const char* isoDate, String& out) const {
  out = "";
  if (storage_ == nullptr || !isValidIsoDate(isoDate)) {
    return false;
  }
  const String raw = String(paths::kCalendar) + "/" + isoDate + ".json";
  return storage_->sanitizePath(raw.c_str(), out);
}

const CalendarEvent* CalendarService::event(size_t index) const {
  return index < count_ ? &events_[index] : nullptr;
}

size_t CalendarService::pendingCount() const {
  size_t n = 0;
  for (size_t i = 0; i < count_; i++) {
    if (!events_[i].done) {
      n++;
    }
  }
  return n;
}

size_t CalendarService::copyEvents(CalendarEvent* out, size_t maxItems, size_t* totalOut) const {
  if (totalOut != nullptr) {
    *totalOut = total_;  // the TRUE count, so the UI can say "+N more"
  }
  if (out == nullptr || maxItems == 0) {
    return 0;
  }
  const size_t n = count_ < maxItems ? count_ : maxItems;
  for (size_t i = 0; i < n; i++) {
    out[i] = events_[i];
  }
  return n;
}

bool CalendarService::loadDay(int dayOffsetFromToday) {
  char iso[11];
  if (!isoForOffset(dayOffsetFromToday, iso, sizeof(iso))) {
    clearCache();
    return false;
  }
  return loadDate(iso);
}

bool CalendarService::reload() {
  return loadedDate_[0] != '\0' && loadDate(loadedDate_);
}

bool CalendarService::loadDate(const char* isoDate) {
  // Copy first: callers legitimately pass loadedDate_ itself (reload()), and
  // clearCache() is about to blank it.
  char iso[11];
  if (!isValidIsoDate(isoDate)) {
    clearCache();
    return false;
  }
  snprintf(iso, sizeof(iso), "%s", isoDate);
  clearCache();
  snprintf(loadedDate_, sizeof(loadedDate_), "%s", iso);

  String path;
  if (!ready() || !dayFilePath(iso, path)) {
    // No card: an empty, honestly-unloaded day. Boot and navigation still work.
    return false;
  }

  JsonDocument doc;
  const DayLoad st = readDayDoc(path, doc);
  if (st == DayLoad::Corrupt) {
    corrupt_ = true;
    return false;
  }
  loaded_ = true;
  if (st == DayLoad::Missing) {
    return true;  // no file yet == a day with no events
  }

  JsonArray arr = eventsOf(doc);
  if (arr.isNull()) {
    return true;
  }

  uint16_t fileIndex = 0;
  for (JsonVariant v : arr) {
    const uint16_t here = fileIndex++;
    if (!v.is<JsonObject>()) {
      continue;  // junk element: skip, but it still occupies its file slot
    }
    const char* title = v["title"].is<const char*>() ? v["title"].as<const char*>() : nullptr;
    if (title == nullptr || title[0] == '\0') {
      continue;
    }
    total_++;
    if (count_ >= kMaxDayEvents) {
      continue;  // counted but not cached — hiddenCount() reports these
    }
    CalendarEvent& e = events_[count_];
    e = CalendarEvent();
    e.fileIndex = here;
    copyField(e.title, sizeof(e.title), title);
    copyField(e.note, sizeof(e.note), v["note"].is<const char*>() ? v["note"].as<const char*>()
                                                                 : "");
    const char* hhmm = v["time"].is<const char*>() ? v["time"].as<const char*>() : "";
    if (parseHhMm(hhmm, e.minutes)) {
      snprintf(e.time, sizeof(e.time), "%s", hhmm);
    } else {
      e.time[0] = '\0';
      e.minutes = CalendarEvent::kUntimed;
    }
    e.done = v["done"].is<bool>() ? v["done"].as<bool>() : false;
    count_++;
  }

  // Insertion sort by time; kUntimed sorts last, ties keep file order. The
  // cached window is what the agenda renders, so it must read chronologically
  // even when the file was hand-written out of order.
  for (size_t i = 1; i < count_; i++) {
    CalendarEvent key = events_[i];
    size_t j = i;
    while (j > 0 && events_[j - 1].minutes > key.minutes) {
      events_[j] = events_[j - 1];
      j--;
    }
    events_[j] = key;
  }
  return true;
}

bool CalendarService::dayCounts(const char* isoDate, size_t* totalOut, size_t* pendingOut) {
  if (totalOut != nullptr) {
    *totalOut = 0;
  }
  if (pendingOut != nullptr) {
    *pendingOut = 0;
  }
  String path;
  if (!ready() || !dayFilePath(isoDate, path)) {
    return false;
  }
  JsonDocument doc;
  const DayLoad st = readDayDoc(path, doc);
  if (st == DayLoad::Corrupt) {
    return false;
  }
  if (st == DayLoad::Missing) {
    return true;
  }
  JsonArray arr = eventsOf(doc);
  if (arr.isNull()) {
    return true;
  }
  for (JsonVariant v : arr) {
    if (!v.is<JsonObject>()) {
      continue;
    }
    const char* title = v["title"].is<const char*>() ? v["title"].as<const char*>() : nullptr;
    if (title == nullptr || title[0] == '\0') {
      continue;
    }
    if (totalOut != nullptr) {
      (*totalOut)++;
    }
    if (pendingOut != nullptr && !(v["done"].is<bool>() && v["done"].as<bool>())) {
      (*pendingOut)++;
    }
  }
  return true;
}

bool CalendarService::addEvent(const char* isoDate, const char* hhmm, const char* title,
                               const char* note) {
  const char* iso = (isoDate != nullptr && isoDate[0] != '\0') ? isoDate : loadedDate_;
  if (title == nullptr) {
    return false;
  }
  // Bound the input before it can reach the heap (spec §43/§44).
  char safeTitle[sizeof(CalendarEvent::title)];
  copyField(safeTitle, sizeof(safeTitle), title);
  while (safeTitle[0] == ' ') {
    memmove(safeTitle, safeTitle + 1, strlen(safeTitle));
  }
  size_t tlen = strlen(safeTitle);
  while (tlen > 0 && safeTitle[tlen - 1] == ' ') {
    safeTitle[--tlen] = '\0';
  }
  if (tlen == 0) {
    return false;
  }
  char safeNote[sizeof(CalendarEvent::note)];
  copyField(safeNote, sizeof(safeNote), note != nullptr ? note : "");

  uint16_t minutes = CalendarEvent::kUntimed;
  char safeTime[6] = "";
  if (hhmm != nullptr && hhmm[0] != '\0') {
    if (!parseHhMm(hhmm, minutes)) {
      return false;  // a bad time is a caller error, not an untimed event
    }
    snprintf(safeTime, sizeof(safeTime), "%s", hhmm);
  }

  String path;
  if (!ready() || !dayFilePath(iso, path)) {
    return false;
  }
  if (!storage_->card()->writable()) {
    return false;
  }

  JsonDocument doc;
  const DayLoad st = readDayDoc(path, doc);
  if (st == DayLoad::Corrupt) {
    Serial.printf("[calendar] refusing to write over unreadable %s\n", path.c_str());
    return false;
  }
  JsonArray arr;
  if (st == DayLoad::Missing) {
    doc.clear();
    arr = makeEventsArray(doc, iso);
  } else {
    arr = eventsOf(doc);
    if (arr.isNull()) {
      if (!doc.is<JsonObject>()) {
        return false;
      }
      arr = doc["events"].to<JsonArray>();
    }
  }
  JsonObject o = arr.add<JsonObject>();
  if (o.isNull()) {
    return false;
  }
  o["time"] = safeTime;
  o["title"] = safeTitle;
  o["note"] = safeNote;
  o["done"] = false;

  // The calendar directory may not exist yet on a fresh card.
  if (!SD_MMC.exists(paths::kCalendar)) {
    SD_MMC.mkdir(paths::kCalendar);
  }
  String body;
  serializeJson(doc, body);
  if (!AtomicFile::writeAll(SD_MMC, path.c_str(), body)) {
    return false;
  }
  if (strcmp(iso, loadedDate_) == 0) {
    reload();
  }
  return true;
}

bool CalendarService::setDone(size_t index, bool done) {
  if (index >= count_ || loadedDate_[0] == '\0') {
    return false;
  }
  const uint16_t target = events_[index].fileIndex;
  String path;
  if (!ready() || !dayFilePath(loadedDate_, path) || !storage_->card()->writable()) {
    return false;
  }
  JsonDocument doc;
  if (readDayDoc(path, doc) != DayLoad::Ok) {
    return false;
  }
  JsonArray arr = eventsOf(doc);
  if (arr.isNull() || target >= arr.size()) {
    return false;
  }
  JsonObject o = arr[target].as<JsonObject>();
  if (o.isNull()) {
    return false;
  }
  o["done"] = done;
  String body;
  serializeJson(doc, body);
  if (!AtomicFile::writeAll(SD_MMC, path.c_str(), body)) {
    return false;
  }
  return reload();
}

bool CalendarService::removeEvent(size_t index) {
  if (index >= count_ || loadedDate_[0] == '\0') {
    return false;
  }
  const uint16_t target = events_[index].fileIndex;
  String path;
  if (!ready() || !dayFilePath(loadedDate_, path)) {
    return false;
  }
  if (!storage_->card()->canDelete()) {
    return false;
  }
  JsonDocument doc;
  if (readDayDoc(path, doc) != DayLoad::Ok) {
    return false;
  }
  JsonArray arr = eventsOf(doc);
  if (arr.isNull() || target >= arr.size()) {
    return false;
  }
  arr.remove(target);
  String body;
  serializeJson(doc, body);
  if (!AtomicFile::writeAll(SD_MMC, path.c_str(), body)) {
    return false;
  }
  return reload();
}

bool CalendarService::nextEvent(CalendarEvent& out, char* outIsoDate, size_t outIsoSize,
                                uint8_t searchDays) {
  if (outIsoDate != nullptr && outIsoSize > 0) {
    outIsoDate[0] = '\0';
  }
  if (!clockKnown() || !ready()) {
    return false;
  }
  struct tm t;
  if (!time_->now(t)) {
    return false;
  }
  const uint16_t nowMinutes = static_cast<uint16_t>(t.tm_hour * 60 + t.tm_min);
  if (searchDays == 0) {
    searchDays = 1;
  }
  if (searchDays > 31) {
    searchDays = 31;
  }

  for (uint8_t d = 0; d < searchDays; d++) {
    char iso[11];
    if (!isoForOffset(static_cast<int>(d), iso, sizeof(iso))) {
      return false;
    }
    String path;
    if (!dayFilePath(iso, path)) {
      continue;
    }
    JsonDocument doc;
    const DayLoad st = readDayDoc(path, doc);
    if (st != DayLoad::Ok) {
      continue;  // missing or corrupt: keep looking, never crash
    }
    JsonArray arr = eventsOf(doc);
    if (arr.isNull()) {
      continue;
    }

    bool found = false;
    CalendarEvent best;
    uint16_t fileIndex = 0;
    for (JsonVariant v : arr) {
      const uint16_t here = fileIndex++;
      if (!v.is<JsonObject>()) {
        continue;
      }
      const char* title = v["title"].is<const char*>() ? v["title"].as<const char*>() : nullptr;
      if (title == nullptr || title[0] == '\0') {
        continue;
      }
      if (v["done"].is<bool>() && v["done"].as<bool>()) {
        continue;
      }
      uint16_t minutes = CalendarEvent::kUntimed;
      const char* hhmm = v["time"].is<const char*>() ? v["time"].as<const char*>() : "";
      if (!parseHhMm(hhmm, minutes)) {
        continue;  // untimed entries have no "next"
      }
      if (d == 0 && minutes < nowMinutes) {
        continue;
      }
      if (found && minutes >= best.minutes) {
        continue;
      }
      best = CalendarEvent();
      best.fileIndex = here;
      best.minutes = minutes;
      snprintf(best.time, sizeof(best.time), "%s", hhmm);
      copyField(best.title, sizeof(best.title), title);
      copyField(best.note, sizeof(best.note),
                v["note"].is<const char*>() ? v["note"].as<const char*>() : "");
      best.done = false;
      found = true;
    }
    if (found) {
      out = best;
      if (outIsoDate != nullptr && outIsoSize > 0) {
        snprintf(outIsoDate, outIsoSize, "%s", iso);
      }
      return true;
    }
  }
  return false;
}
