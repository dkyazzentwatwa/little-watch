#include "CalendarCommands.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <stdlib.h>

#include "../../core/Services.h"
#include "../../hardware/SdCardAdapter.h"
#include "../../services/CalendarService.h"
#include "../../services/TimeService.h"
#include "../../storage/SdStorage.h"
#include "../../storage/StoragePaths.h"
#include "../CmdArgs.h"

namespace {

bool requireCard(Services& services) {
  if (services.sdCard == nullptr || !services.sdCard->mounted()) {
    Serial.printf("error: SD card %s\n",
                  services.sdCard != nullptr ? sdCardStateName(services.sdCard->state())
                                             : "unavailable");
    return false;
  }
  return true;
}

// today | tomorrow | yesterday | +N | -N | YYYY-MM-DD. A null token means
// "today". Prints its own error and returns false when the clock has never
// been set and the token needs it (spec §43 first-boot state).
bool resolveDay(Services& services, const char* token, char* iso, size_t isoSize) {
  CalendarService& cal = *services.calendar;
  if (token == nullptr || strcmp(token, "today") == 0) {
    if (cal.todayIso(iso, isoSize)) {
      return true;
    }
  } else if (strcmp(token, "tomorrow") == 0) {
    if (cal.isoForOffset(1, iso, isoSize)) {
      return true;
    }
  } else if (strcmp(token, "yesterday") == 0) {
    if (cal.isoForOffset(-1, iso, isoSize)) {
      return true;
    }
  } else if (token[0] == '+' || token[0] == '-') {
    const long off = strtol(token, nullptr, 10);
    if (off < -3650 || off > 3650) {
      Serial.println("error: day offset out of range");
      return false;
    }
    if (cal.isoForOffset(static_cast<int>(off), iso, isoSize)) {
      return true;
    }
  } else {
    if (CalendarService::isValidIsoDate(token)) {
      snprintf(iso, isoSize, "%s", token);
      return true;
    }
    Serial.printf("error: '%s' is not a date (use YYYY-MM-DD, today, tomorrow, +N)\n", token);
    return false;
  }
  Serial.println("error: clock not set — give an explicit date (calendar list 2026-07-25)");
  return false;
}

void printDayHeader(CalendarService& cal) {
  char label[24];
  CalendarService::formatDayLabel(cal.loadedDate(), label, sizeof(label));
  Serial.printf("--- %s (%s) ---\n", cal.loadedDate(), label);
}

void printEvents(CalendarService& cal) {
  if (cal.eventTotal() == 0) {
    Serial.println("no events");
    return;
  }
  for (size_t i = 0; i < cal.eventCount(); i++) {
    const CalendarEvent* e = cal.event(i);
    if (e == nullptr) {
      continue;
    }
    Serial.printf("%2u. [%c] %-5s  %s\n", (unsigned)(i + 1), e->done ? 'x' : ' ',
                  e->timed() ? e->time : "--:--", e->title);
    if (e->note[0] != '\0') {
      Serial.printf("           %s\n", e->note);
    }
  }
  const size_t hidden = cal.hiddenCount();
  if (hidden > 0) {
    Serial.printf("... +%u more (%u events on this day; %u shown)\n", (unsigned)hidden,
                  (unsigned)cal.eventTotal(), (unsigned)cal.eventCount());
  }
}

// Loads the day named by the (optional) token and reports failures.
bool loadFromToken(Services& services, const char* token) {
  char iso[11];
  if (!resolveDay(services, token, iso, sizeof(iso))) {
    return false;
  }
  CalendarService& cal = *services.calendar;
  if (!cal.loadDate(iso)) {
    if (cal.dayCorrupt()) {
      Serial.printf("error: %s.json is unreadable — it was left untouched; fix or remove it\n",
                    iso);
    } else {
      Serial.printf("error: could not read %s (SD card %s)\n", iso,
                    services.sdCard != nullptr ? sdCardStateName(services.sdCard->state()) : "?");
    }
    return false;
  }
  return true;
}

// 1-based id from the last listing into a cached-day index.
bool resolveId(CalendarService& cal, const char* token, size_t& outIndex) {
  if (token == nullptr) {
    return false;
  }
  if (!cal.dayLoaded()) {
    Serial.println("error: no day loaded — run 'calendar list' first");
    return false;
  }
  char* end = nullptr;
  const long id = strtol(token, &end, 10);
  if (end == token || *end != '\0' || id < 1 ||
      static_cast<size_t>(id) > cal.eventCount()) {
    Serial.printf("error: no event %s on %s (run 'calendar list')\n", token, cal.loadedDate());
    return false;
  }
  outIndex = static_cast<size_t>(id) - 1;
  return true;
}

}  // namespace

void printCalendarHelp() {
  Serial.println("calendar list [today|tomorrow|yesterday|+N|YYYY-MM-DD]");
  Serial.println("                                 agenda for a day (ids for the verbs below)");
  Serial.println("calendar show <id>               full details of one event");
  Serial.println("calendar next                    next upcoming event (searches 7 days)");
  Serial.println("calendar add <day> <HH:MM|-> \"<title>\" [\"<note>\"]   create an event");
  Serial.println("calendar done <id> [on|off]      mark complete / dismissed (default on)");
  Serial.println("calendar delete <id> confirm     delete an event (confirm required)");
  Serial.println("calendar export [<day>]          copy the day file into /littlecube/exports");
  Serial.println("calendar import <path> [<day>] [confirm]");
  Serial.println("                                 copy a JSON day file into the calendar");
  Serial.println("days: today (default) · tomorrow · yesterday · +N / -N · YYYY-MM-DD");
}

bool handleCalendarCommand(Services& services, const char* verb, char* args) {
  if (services.calendar == nullptr || services.storage == nullptr) {
    Serial.println("error: calendar service unavailable");
    return true;
  }
  CalendarService& cal = *services.calendar;

  if (strcmp(verb, "list") == 0) {
    if (!requireCard(services)) {
      return true;
    }
    char* cursor = args;
    if (!loadFromToken(services, cmdargs::nextToken(cursor))) {
      return true;
    }
    printDayHeader(cal);
    printEvents(cal);
    return true;
  }

  if (strcmp(verb, "show") == 0) {
    char* cursor = args;
    size_t index = 0;
    if (!resolveId(cal, cmdargs::nextToken(cursor), index)) {
      return true;
    }
    const CalendarEvent* e = cal.event(index);
    if (e == nullptr) {
      Serial.println("error: event vanished — run 'calendar list' again");
      return true;
    }
    char label[24];
    CalendarService::formatDayLabel(cal.loadedDate(), label, sizeof(label));
    Serial.printf("date:  %s (%s)\n", cal.loadedDate(), label);
    Serial.printf("time:  %s\n", e->timed() ? e->time : "(untimed)");
    Serial.printf("title: %s\n", e->title);
    Serial.printf("note:  %s\n", e->note[0] != '\0' ? e->note : "-");
    Serial.printf("done:  %s\n", e->done ? "yes" : "no");
    return true;
  }

  if (strcmp(verb, "next") == 0) {
    if (!requireCard(services)) {
      return true;
    }
    if (!cal.clockKnown()) {
      Serial.println("clock not set — 'time set YYYY-MM-DD HH:MM' first");
      return true;
    }
    CalendarEvent e;
    char iso[11];
    if (!cal.nextEvent(e, iso, sizeof(iso), 7)) {
      Serial.println("nothing scheduled in the next 7 days");
      return true;
    }
    char label[24];
    CalendarService::formatDayLabel(iso, label, sizeof(label));
    Serial.printf("%s %s  %s\n", label, e.time, e.title);
    if (e.note[0] != '\0') {
      Serial.printf("  %s\n", e.note);
    }
    return true;
  }

  if (strcmp(verb, "add") == 0) {
    char* cursor = args;
    const char* dayToken = cmdargs::nextToken(cursor);
    const char* timeToken = cmdargs::nextToken(cursor);
    const char* title = cmdargs::nextToken(cursor);
    const char* note = cmdargs::rest(cursor);
    if (dayToken == nullptr || timeToken == nullptr || title == nullptr) {
      Serial.println("usage: calendar add <day> <HH:MM|-> \"<title>\" [\"<note>\"]");
      return true;
    }
    if (!requireCard(services)) {
      return true;
    }
    if (!services.sdCard->writable()) {
      Serial.printf("error: SD card %s\n", sdCardStateName(services.sdCard->state()));
      return true;
    }
    char iso[11];
    if (!resolveDay(services, dayToken, iso, sizeof(iso))) {
      return true;
    }
    // "-" means an untimed / all-day entry.
    const char* hhmm = strcmp(timeToken, "-") == 0 ? "" : timeToken;
    if (!cal.addEvent(iso, hhmm, title, note)) {
      Serial.println("error: could not add (bad time, empty title, or unreadable day file)");
      return true;
    }
    Serial.printf("added to %s\n", iso);
    if (strcmp(iso, cal.loadedDate()) == 0) {
      printEvents(cal);
    }
    return true;
  }

  if (strcmp(verb, "done") == 0) {
    char* cursor = args;
    const char* idToken = cmdargs::nextToken(cursor);
    const char* state = cmdargs::nextToken(cursor);
    size_t index = 0;
    if (idToken == nullptr) {
      Serial.println("usage: calendar done <id> [on|off]");
      return true;
    }
    if (state != nullptr && strcmp(state, "on") != 0 && strcmp(state, "off") != 0) {
      Serial.println("usage: calendar done <id> [on|off]");
      return true;
    }
    if (!resolveId(cal, idToken, index)) {
      return true;
    }
    const bool on = state == nullptr || strcmp(state, "on") == 0;
    Serial.println(cal.setDone(index, on) ? "ok" : "error: could not update the day file");
    return true;
  }

  if (strcmp(verb, "delete") == 0) {
    char* cursor = args;
    const char* idToken = cmdargs::nextToken(cursor);
    const char* confirm = cmdargs::nextToken(cursor);
    size_t index = 0;
    if (idToken == nullptr) {
      Serial.println("usage: calendar delete <id> confirm");
      return true;
    }
    if (!resolveId(cal, idToken, index)) {
      return true;
    }
    const CalendarEvent* e = cal.event(index);
    if (confirm == nullptr || strcmp(confirm, "confirm") != 0) {
      Serial.printf("this deletes \"%s\" on %s — run: calendar delete %s confirm\n",
                    e != nullptr ? e->title : "?", cal.loadedDate(), idToken);
      return true;
    }
    Serial.println(cal.removeEvent(index) ? "deleted" : "error: delete failed");
    return true;
  }

  if (strcmp(verb, "export") == 0) {
    char* cursor = args;
    char iso[11];
    if (!resolveDay(services, cmdargs::nextToken(cursor), iso, sizeof(iso))) {
      return true;
    }
    String src;
    if (!cal.dayFilePath(iso, src)) {
      Serial.println("error: bad date");
      return true;
    }
    const String dest = String(paths::kExports) + "/" + iso + ".json";
    if (services.storage->copyFile(src.c_str(), dest.c_str())) {
      Serial.printf("exported: %s\n", dest.c_str());
    } else {
      Serial.println("error: export failed (is there anything on that day?)");
    }
    return true;
  }

  if (strcmp(verb, "import") == 0) {
    char* cursor = args;
    const char* from = cmdargs::nextToken(cursor);
    const char* t1 = cmdargs::nextToken(cursor);
    const char* t2 = cmdargs::nextToken(cursor);
    const char* dayToken = nullptr;
    bool confirmed = false;
    if (t1 != nullptr) {
      if (strcmp(t1, "confirm") == 0) {
        confirmed = true;
      } else {
        dayToken = t1;
      }
    }
    if (t2 != nullptr && strcmp(t2, "confirm") == 0) {
      confirmed = true;
    }
    if (from == nullptr) {
      Serial.println("usage: calendar import <path> [<day>] [confirm]");
      Serial.println("  copies a JSON day file into /littlecube/calendar/<day>.json");
      return true;
    }
    if (!requireCard(services)) {
      return true;
    }
    // Every path from serial is sanitized before it touches the card (§44).
    String safeFrom;
    if (!services.storage->sanitizePath(from, safeFrom)) {
      Serial.println("error: invalid source path");
      return true;
    }
    char iso[11];
    if (dayToken != nullptr) {
      if (!resolveDay(services, dayToken, iso, sizeof(iso))) {
        return true;
      }
    } else {
      // Derive the day from a <YYYY-MM-DD>.json basename.
      const char* slash = strrchr(safeFrom.c_str(), '/');
      const char* base = slash != nullptr ? slash + 1 : safeFrom.c_str();
      if (strlen(base) < 10) {
        Serial.println("error: give a day (calendar import <path> YYYY-MM-DD)");
        return true;
      }
      snprintf(iso, sizeof(iso), "%.10s", base);
      if (!CalendarService::isValidIsoDate(iso)) {
        Serial.println("error: give a day (calendar import <path> YYYY-MM-DD)");
        return true;
      }
    }
    String dest;
    if (!cal.dayFilePath(iso, dest)) {
      Serial.println("error: bad date");
      return true;
    }
    // Importing over an existing day destroys it: confirm first (spec §44).
    if (SD_MMC.exists(dest) && !confirmed) {
      Serial.printf("%s already exists and would be replaced — run:\n", dest.c_str());
      Serial.printf("  calendar import %s %s confirm\n", from, iso);
      return true;
    }
    if (!services.storage->copyFile(safeFrom.c_str(), dest.c_str())) {
      Serial.println("error: import failed (path allowed? card writable?)");
      return true;
    }
    Serial.printf("imported: %s\n", dest.c_str());
    if (cal.loadDate(iso)) {
      printDayHeader(cal);
      printEvents(cal);
    } else {
      Serial.println("warning: the imported file did not parse — it is on the card but ignored");
    }
    return true;
  }

  return false;
}
