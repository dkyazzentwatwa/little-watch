#include "ContactsCommands.h"

#include <Arduino.h>

#include "../../core/Services.h"
#include "../../hardware/SdCardAdapter.h"
#include "../../services/ContactsService.h"
#include "../../storage/SdStorage.h"
#include "../../storage/StoragePaths.h"
#include "../CmdArgs.h"

namespace {

// Search results are the only listing that needs its own storage (list()
// reads the service cache in place). 12 rows = ~1.5 KB of .bss; the true
// match count is always printed, so results are never silently truncated.
constexpr size_t kMaxSearchResults = 12;
ContactInfo s_search[kMaxSearchResults];

bool requireCard(Services& services) {
  if (services.sdCard == nullptr || !services.sdCard->mounted()) {
    Serial.printf("error: SD card %s\n",
                  services.sdCard != nullptr ? sdCardStateName(services.sdCard->state())
                                             : "unavailable");
    return false;
  }
  return true;
}

bool requireWritable(Services& services) {
  if (!requireCard(services)) {
    return false;
  }
  if (!services.sdCard->writable()) {
    Serial.printf("error: SD card %s\n", sdCardStateName(services.sdCard->state()));
    return false;
  }
  return true;
}

// Explains why the address book is empty rather than just showing nothing.
bool reportLoad(Services& services, ContactsService& contacts) {
  if (contacts.corrupt()) {
    Serial.println("error: /littlecube/contacts/contacts.json is unreadable — it was left");
    Serial.println("       untouched; fix or remove it, then run 'contacts list' again");
    return false;
  }
  if (!contacts.loaded()) {
    Serial.printf("error: could not read contacts (SD card %s)\n",
                  services.sdCard != nullptr ? sdCardStateName(services.sdCard->state()) : "?");
    return false;
  }
  return true;
}

void printRow(const ContactInfo& c) {
  Serial.printf("%-6s %s%s\n", c.id, c.favorite ? "* " : "  ", c.name);
  if (c.phone[0] != '\0' || c.email[0] != '\0') {
    Serial.printf("       %s%s%s\n", c.phone, (c.phone[0] != '\0' && c.email[0] != '\0') ? "  ·  "
                                                                                        : "",
                  c.email);
  }
  if (c.hasVoice) {
    Serial.println("       [voice note]");
  }
}

}  // namespace

void printContactsHelp() {
  Serial.println("contacts list                       list contacts (favourites first)");
  Serial.println("contacts search <text>              case-insensitive match on name");
  Serial.println("contacts show <id>                  full record");
  Serial.println("contacts add \"<name>\" [\"<phone>\"] [\"<email>\"] [\"<note>\"]");
  Serial.println("contacts set <id> <field> <value>   field: name|phone|email|note|voice");
  Serial.println("contacts favorite <id> on|off       star / unstar (kept off the SD card)");
  Serial.println("contacts delete <id> confirm        delete a contact (confirm required)");
  Serial.println("contacts export                     copy contacts.json into /littlecube/exports");
  Serial.println("contacts import <path> confirm      replace the whole address book");
}

bool handleContactsCommand(Services& services, const char* verb, char* args) {
  if (services.contacts == nullptr || services.storage == nullptr) {
    Serial.println("error: contacts service unavailable");
    return true;
  }
  ContactsService& contacts = *services.contacts;

  if (strcmp(verb, "list") == 0) {
    if (!requireCard(services)) {
      return true;
    }
    contacts.reload();
    if (!reportLoad(services, contacts)) {
      return true;
    }
    if (contacts.total() == 0) {
      Serial.println("no contacts (contacts add \"Name\" \"phone\")");
      return true;
    }
    for (size_t i = 0; i < contacts.cached(); i++) {
      const ContactInfo* c = contacts.at(i);
      if (c != nullptr) {
        printRow(*c);
      }
    }
    if (contacts.hiddenCount() > 0) {
      Serial.printf("... +%u more (%u contacts; %u shown — use 'contacts search')\n",
                    (unsigned)contacts.hiddenCount(), (unsigned)contacts.total(),
                    (unsigned)contacts.cached());
    } else {
      Serial.printf("%u contact%s\n", (unsigned)contacts.total(),
                    contacts.total() == 1 ? "" : "s");
    }
    return true;
  }

  if (strcmp(verb, "search") == 0) {
    char* cursor = args;
    const char* query = cmdargs::rest(cursor);
    if (query == nullptr) {
      Serial.println("usage: contacts search <text>");
      return true;
    }
    if (!requireCard(services)) {
      return true;
    }
    size_t matches = 0;
    const size_t shown = contacts.search(query, s_search, kMaxSearchResults, &matches);
    if (matches == 0) {
      Serial.printf("no contact matches \"%s\"\n", query);
      return true;
    }
    for (size_t i = 0; i < shown; i++) {
      printRow(s_search[i]);
    }
    if (matches > shown) {
      Serial.printf("... +%u more match%s — narrow the search\n", (unsigned)(matches - shown),
                    (matches - shown) == 1 ? "" : "es");
    }
    return true;
  }

  if (strcmp(verb, "show") == 0) {
    char* cursor = args;
    const char* id = cmdargs::nextToken(cursor);
    if (id == nullptr) {
      Serial.println("usage: contacts show <id>   (ids come from 'contacts list')");
      return true;
    }
    if (!requireCard(services)) {
      return true;
    }
    ContactDetail d;
    if (!contacts.get(id, d)) {
      Serial.printf("error: no contact '%s' (run 'contacts list')\n", id);
      return true;
    }
    Serial.printf("id:    %s%s\n", d.id, d.favorite ? "  [favourite]" : "");
    Serial.printf("name:  %s\n", d.name);
    Serial.printf("phone: %s\n", d.phone[0] != '\0' ? d.phone : "-");
    Serial.printf("email: %s\n", d.email[0] != '\0' ? d.email : "-");
    Serial.printf("note:  %s\n", d.note[0] != '\0' ? d.note : "-");
    Serial.printf("voice: %s\n", d.voice[0] != '\0' ? d.voice : "-");
    return true;
  }

  if (strcmp(verb, "add") == 0) {
    char* cursor = args;
    const char* name = cmdargs::nextToken(cursor);
    const char* phone = cmdargs::nextToken(cursor);
    const char* email = cmdargs::nextToken(cursor);
    const char* note = cmdargs::rest(cursor);
    if (name == nullptr) {
      Serial.println("usage: contacts add \"<name>\" [\"<phone>\"] [\"<email>\"] [\"<note>\"]");
      return true;
    }
    if (!requireWritable(services)) {
      return true;
    }
    char newId[12];
    if (!contacts.add(name, phone, email, note, newId, sizeof(newId))) {
      Serial.println("error: could not add (empty name, book full, or unreadable file)");
      return true;
    }
    Serial.printf("added %s: %s\n", newId, name);
    return true;
  }

  if (strcmp(verb, "set") == 0) {
    char* cursor = args;
    const char* id = cmdargs::nextToken(cursor);
    const char* field = cmdargs::nextToken(cursor);
    const char* value = cmdargs::rest(cursor);
    if (id == nullptr || field == nullptr) {
      Serial.println("usage: contacts set <id> <name|phone|email|note|voice> <value>");
      return true;
    }
    if (!requireWritable(services)) {
      return true;
    }
    if (!contacts.update(id, field, value)) {
      Serial.println("error: could not set (unknown id or field, empty name, or bad voice path)");
      return true;
    }
    Serial.println("ok");
    return true;
  }

  if (strcmp(verb, "favorite") == 0) {
    char* cursor = args;
    const char* id = cmdargs::nextToken(cursor);
    const char* state = cmdargs::nextToken(cursor);
    if (id == nullptr || state == nullptr ||
        (strcmp(state, "on") != 0 && strcmp(state, "off") != 0)) {
      Serial.println("usage: contacts favorite <id> on|off");
      return true;
    }
    const bool on = strcmp(state, "on") == 0;
    // Starring something that does not exist would leave a dead id in the
    // index forever. Unstarring is always allowed, so a stale one can go.
    if (on) {
      if (!requireCard(services)) {
        return true;
      }
      ContactDetail d;
      if (!contacts.get(id, d)) {
        Serial.printf("error: no contact '%s' (run 'contacts list')\n", id);
        return true;
      }
    }
    if (!contacts.setFavorite(id, on)) {
      Serial.printf("error: could not update (at most %u favourites, and LittleFS must be"
                    " writable)\n",
                    (unsigned)ContactsService::kMaxFavorites);
      return true;
    }
    Serial.println("ok");
    return true;
  }

  if (strcmp(verb, "delete") == 0) {
    char* cursor = args;
    const char* id = cmdargs::nextToken(cursor);
    const char* confirm = cmdargs::nextToken(cursor);
    if (id == nullptr) {
      Serial.println("usage: contacts delete <id> confirm");
      return true;
    }
    if (!requireCard(services)) {
      return true;
    }
    ContactDetail d;
    if (!contacts.get(id, d)) {
      Serial.printf("error: no contact '%s' (run 'contacts list')\n", id);
      return true;
    }
    if (confirm == nullptr || strcmp(confirm, "confirm") != 0) {
      Serial.printf("this deletes %s (%s) — run: contacts delete %s confirm\n", d.name, d.id, id);
      return true;
    }
    Serial.println(contacts.remove(id) ? "deleted" : "error: delete failed");
    return true;
  }

  if (strcmp(verb, "export") == 0) {
    if (!requireCard(services)) {
      return true;
    }
    String src;
    if (!contacts.filePath(src)) {
      Serial.println("error: contacts path unavailable");
      return true;
    }
    const String dest = String(paths::kExports) + "/contacts.json";
    if (services.storage->copyFile(src.c_str(), dest.c_str())) {
      Serial.printf("exported: %s\n", dest.c_str());
    } else {
      Serial.println("error: export failed (is there an address book yet?)");
    }
    return true;
  }

  if (strcmp(verb, "import") == 0) {
    char* cursor = args;
    const char* from = cmdargs::nextToken(cursor);
    const char* confirm = cmdargs::nextToken(cursor);
    if (from == nullptr) {
      Serial.println("usage: contacts import <path> confirm");
      Serial.println("  REPLACES /littlecube/contacts/contacts.json with <path>");
      return true;
    }
    if (!requireWritable(services)) {
      return true;
    }
    // Every path from serial is sanitized before it touches the card (§44).
    String safeFrom;
    if (!services.storage->sanitizePath(from, safeFrom)) {
      Serial.println("error: invalid source path");
      return true;
    }
    String dest;
    if (!contacts.filePath(dest)) {
      Serial.println("error: contacts path unavailable");
      return true;
    }
    if (confirm == nullptr || strcmp(confirm, "confirm") != 0) {
      Serial.printf("this replaces your whole address book (%u contacts) — run:\n",
                    (unsigned)contacts.total());
      Serial.printf("  contacts import %s confirm\n", from);
      return true;
    }
    if (!services.storage->copyFile(safeFrom.c_str(), dest.c_str())) {
      Serial.println("error: import failed (path allowed? card writable?)");
      return true;
    }
    if (!contacts.reload()) {
      Serial.println("imported, but the file did not parse — the previous book is gone;");
      Serial.println("restore it from /littlecube/exports if you exported one");
      return true;
    }
    Serial.printf("imported: %u contact%s\n", (unsigned)contacts.total(),
                  contacts.total() == 1 ? "" : "s");
    return true;
  }

  return false;
}
