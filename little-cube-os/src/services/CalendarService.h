#pragma once

#include <Arduino.h>
#include <time.h>

class SdStorage;
class TimeService;

// Calendar (spec §14): agenda viewing over editing. The cube owns the data;
// there is no sync. Full titles arrive over USB serial or as imported files.
//
// ---------------------------------------------------------------------------
// ON-DISK SCHEMA — /littlecube/calendar/<YYYY-MM-DD>.json, one file per day
// ---------------------------------------------------------------------------
//   {
//     "date": "2026-07-25",
//     "events": [
//       { "time": "09:30", "title": "Standup", "note": "daily",  "done": false },
//       { "time": "",      "title": "Dad's birthday",            "done": false }
//     ]
//   }
//
//   date    optional, informational; the FILENAME is authoritative.
//   events  array of objects. Missing/!array => the day is treated as empty.
//   time    "HH:MM" 24-hour LOCAL wall time. "" or absent = untimed/all-day.
//           Anything that is not a valid HH:MM is read as untimed.
//   title   required and non-empty; an event without one is skipped on load.
//   note    optional free text.
//   done    optional bool, default false (dismissed / completed).
//
//   A bare top-level array (`[ {...}, {...} ]`) is ALSO accepted on read, so a
//   hand-written or imported file works. A rewrite preserves whichever
//   top-level form the file already had; files this service creates use the
//   object form. Unknown keys on an event survive a rewrite untouched.
//
//   Day files are read lazily — begin() touches nothing and never scans the
//   directory. A file that fails to parse is logged and skipped; it is never
//   deleted and never overwritten (dayCorrupt() latches and every mutation on
//   that day is refused, so bad JSON cannot cost the user their data).
//
// ---------------------------------------------------------------------------
// THREADING / BLOCKING
// ---------------------------------------------------------------------------
//   Pull-only. There is no update(deltaMs) and no task. EVERY method below
//   that names a date does SD I/O — call them from App::onOpen()/refresh(),
//   never from render(). The cached day is pure RAM: loadedDate(),
//   eventCount(), eventTotal(), event(), pendingCount() are render-safe.
//
// Untimed events sort AFTER all timed ones for the day (they have no slot).

// One agenda entry. 124 bytes; kMaxDayEvents of them is ~3.0 KB of .bss.
struct CalendarEvent {
  char time[6] = "";     // "HH:MM" or "" when untimed
  char title[48] = "";
  char note[64] = "";
  uint16_t minutes = 0;  // minutes since local midnight, or kUntimed
  uint16_t fileIndex = 0;  // position in the on-disk array (mutations use it)
  bool done = false;

  static constexpr uint16_t kUntimed = 0xFFFF;
  bool timed() const { return minutes != kUntimed; }
};

class CalendarService {
 public:
  // Cache bound for one day. A day file may legally hold more; eventTotal()
  // always reports the true on-disk count so the UI can render "+N more".
  static constexpr size_t kMaxDayEvents = 24;
  // Refuse to parse a day file bigger than this (malformed/hostile input must
  // not exhaust the heap — spec §43).
  static constexpr size_t kMaxDayFileBytes = 16384;

  void begin(SdStorage* storage, TimeService* time);

  // Drops the cached day. Call when the card mounts or disappears; the next
  // load re-reads from the (possibly different) card.
  void invalidate();

  // --- dates -------------------------------------------------------------
  // Both return false when the clock has never been set (TimeService::valid()
  // is false — a reachable first-boot state, spec §43). Callers must handle
  // it: the UI shows "clock not set", serial asks for an explicit date.
  bool clockKnown() const;
  bool todayIso(char* out, size_t outSize) const;
  // dayOffset is in DAYS and is applied through mktime(), so it is correct
  // across DST transitions and month/year ends. Never add 86400 seconds.
  bool isoForOffset(int dayOffset, char* out, size_t outSize) const;

  // Strict "YYYY-MM-DD" with a real calendar date (leap years included).
  static bool isValidIsoDate(const char* iso);
  // "Mon 25 Jul" style label for a date string; out needs >= 16 bytes.
  static bool formatDayLabel(const char* iso, char* out, size_t outSize);

  // --- loading (does SD I/O) ---------------------------------------------
  bool loadDay(int dayOffsetFromToday);  // 0 = today, 1 = tomorrow, -1 = yesterday
  bool loadDate(const char* isoDate);
  bool reload();  // re-read the currently loaded day

  // --- cached day (render-safe, no I/O) ----------------------------------
  bool dayLoaded() const { return loaded_; }
  bool dayCorrupt() const { return corrupt_; }
  const char* loadedDate() const { return loadedDate_; }  // "" when none
  size_t eventCount() const { return count_; }            // <= kMaxDayEvents
  size_t eventTotal() const { return total_; }            // true on-disk count
  size_t hiddenCount() const { return total_ > count_ ? total_ - count_ : 0; }
  size_t pendingCount() const;  // not-done events in the cached window
  const CalendarEvent* event(size_t index) const;
  // Bulk copy for a UI that keeps its own array. Writes the true total (which
  // may exceed maxItems) into totalOut when it is non-null.
  size_t copyEvents(CalendarEvent* out, size_t maxItems, size_t* totalOut) const;

  // --- mutation (does SD I/O) --------------------------------------------
  // isoDate may be null to mean "the loaded day". Events beyond the cache
  // window are preserved: every mutation re-reads the whole file, edits it,
  // and writes it back atomically.
  bool addEvent(const char* isoDate, const char* hhmm, const char* title, const char* note);
  // index is into the CACHED day (0..eventCount()-1); it is mapped back to
  // the on-disk position internally.
  bool setDone(size_t index, bool done);
  bool removeEvent(size_t index);

  // --- Today app ---------------------------------------------------------
  // First not-done TIMED event at or after the current local time, searching
  // forward up to searchDays day files. Untimed entries are skipped (they
  // have no "next"). Returns false when the clock is unset, the card is gone,
  // or nothing was found. Does SD I/O but does NOT disturb the loaded day.
  bool nextEvent(CalendarEvent& out, char* outIsoDate, size_t outIsoSize,
                 uint8_t searchDays = 7);
  // Cheap "does this day have anything" probe used by month/agenda views.
  // Returns false (and 0s) when the card is absent or the file is corrupt.
  bool dayCounts(const char* isoDate, size_t* totalOut, size_t* pendingOut);

  // Absolute path of a day file. Public so the serial import/export verbs can
  // name it. Runs through SdStorage::sanitizePath().
  bool dayFilePath(const char* isoDate, String& out) const;

 private:
  bool ready() const;
  void clearCache();

  SdStorage* storage_ = nullptr;
  TimeService* time_ = nullptr;

  CalendarEvent events_[kMaxDayEvents];
  char loadedDate_[11] = "";
  size_t count_ = 0;
  size_t total_ = 0;
  bool loaded_ = false;
  bool corrupt_ = false;
};
