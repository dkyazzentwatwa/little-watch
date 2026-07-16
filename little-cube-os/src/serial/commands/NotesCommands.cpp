#include "NotesCommands.h"

#include <Arduino.h>
#include <SD_MMC.h>

#include "../../core/Services.h"
#include "../../hardware/SdCardAdapter.h"
#include "../../services/NotesService.h"
#include "../../services/TimeService.h"
#include "../../storage/SdStorage.h"
#include "../../storage/StoragePaths.h"
#include "../CmdArgs.h"
#include "../MultilineBuffer.h"

namespace {

constexpr const char* kTempPath = "/littlecube/cache/.serial-input.tmp";
constexpr size_t kMaxListed = 48;

NoteInfo s_list[kMaxListed];
size_t s_listCount = 0;
bool s_listValid = false;

void refreshList(Services& services) {
  s_listCount = services.notes->list(s_list, kMaxListed);
  s_listValid = true;
}

// Accepts a 1-based id from the last listing or a path.
bool resolveNote(Services& services, const char* token, String& outPath) {
  if (token == nullptr) {
    return false;
  }
  bool digits = token[0] != '\0';
  for (const char* p = token; *p != '\0'; p++) {
    if (!isDigit(*p)) {
      digits = false;
      break;
    }
  }
  if (digits) {
    if (!s_listValid) {
      refreshList(services);
    }
    const size_t id = strtoul(token, nullptr, 10);
    if (id < 1 || id > s_listCount) {
      Serial.printf("error: no note %s (run 'notes list')\n", token);
      return false;
    }
    outPath = s_list[id - 1].path;
    return true;
  }
  if (!services.storage->sanitizePath(token, outPath)) {
    Serial.println("error: invalid path");
    return false;
  }
  return true;
}

bool requireCard(Services& services) {
  if (!services.sdCard->mounted()) {
    Serial.printf("error: SD card %s\n", sdCardStateName(services.sdCard->state()));
    return false;
  }
  return true;
}

void startMultiline(Services& services, MultilineBuffer& multiline, const String& target,
                    MultilineBuffer::SaveMode mode) {
  if (!services.sdCard->writable()) {
    Serial.printf("error: SD card %s\n", sdCardStateName(services.sdCard->state()));
    return;
  }
  if (!multiline.start(SD_MMC, kTempPath, target.c_str(), mode)) {
    Serial.println("error: could not open temp file for input");
    return;
  }
  Serial.println();
  Serial.println("Enter text.");
  Serial.println("Type .END on a line by itself to save.");
  Serial.println("Type .CANCEL to discard. (.PREVIEW / .CLEAR also work)");
  Serial.println();
}

String noteTargetFromName(Services& services, const char* name) {
  String target;
  if (strchr(name, '/') != nullptr) {
    if (!services.storage->sanitizePath(name, target)) {
      return String();
    }
  } else {
    target = String(paths::kNotesText) + "/" + name;
  }
  if (!target.endsWith(".md") && !target.endsWith(".txt")) {
    target += ".md";
  }
  return target;
}

}  // namespace

void printNotesHelp() {
  Serial.println("notes list                    list notes (ids for the commands below)");
  Serial.println("notes show <id|path>          print a note");
  Serial.println("notes new                     create a timestamped note (multiline)");
  Serial.println("notes write <filename>        write/overwrite a note (multiline)");
  Serial.println("notes append <id|path>        append to a note (multiline)");
  Serial.println("notes rename <id> \"<title>\"   rename via slugified title");
  Serial.println("notes favorite <id> on|off    star / unstar");
  Serial.println("notes pin <id> on|off         pin to the top of lists");
  Serial.println("notes delete <id> confirm     delete (confirm required)");
  Serial.println("notes export <id>             copy into /littlecube/exports");
  Serial.println("notes import <path>           copy a file into the notes folder");
}

bool handleNotesCommand(Services& services, MultilineBuffer& multiline, const char* verb,
                        char* args) {
  if (services.notes == nullptr || services.storage == nullptr) {
    return false;
  }

  if (strcmp(verb, "list") == 0) {
    if (!requireCard(services)) {
      return true;
    }
    refreshList(services);
    if (s_listCount == 0) {
      Serial.println("no notes found");
      return true;
    }
    for (size_t i = 0; i < s_listCount; i++) {
      const NoteInfo& n = s_list[i];
      Serial.printf("%2u. %s%s%s  (%u B)  %s\n", (unsigned)(i + 1), n.pinned ? "[pin] " : "",
                    n.favorite ? "[*] " : "", n.title, (unsigned)n.sizeBytes, n.path);
    }
    return true;
  }

  if (strcmp(verb, "show") == 0) {
    char* cursor = args;
    const char* token = cmdargs::nextToken(cursor);
    String path;
    if (token == nullptr || !resolveNote(services, token, path)) {
      if (token == nullptr) {
        Serial.println("usage: notes show <id|path>");
      }
      return true;
    }
    String body;
    bool truncated = false;
    if (!services.notes->read(path.c_str(), body, 16384, truncated)) {
      Serial.println("error: could not read note");
      return true;
    }
    Serial.printf("--- %s ---\n", path.c_str());
    Serial.print(body);
    if (!body.endsWith("\n")) {
      Serial.println();
    }
    if (truncated) {
      Serial.println("... (truncated at 16 KB)");
    }
    Serial.println("--- end ---");
    return true;
  }

  if (strcmp(verb, "new") == 0) {
    if (!requireCard(services)) {
      return true;
    }
    char name[48];
    struct tm now;
    if (services.time != nullptr && services.time->now(now)) {
      snprintf(name, sizeof(name), "note-%04d%02d%02d-%02d%02d%02d.md", now.tm_year + 1900,
               now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec);
    } else {
      snprintf(name, sizeof(name), "note-%lu.md", (unsigned long)millis());
    }
    const String target = String(paths::kNotesText) + "/" + name;
    Serial.printf("new note: %s\n", target.c_str());
    startMultiline(services, multiline, target, MultilineBuffer::SaveMode::Overwrite);
    return true;
  }

  if (strcmp(verb, "write") == 0) {
    char* cursor = args;
    const char* name = cmdargs::nextToken(cursor);
    if (name == nullptr) {
      Serial.println("usage: notes write <filename>");
      return true;
    }
    if (!requireCard(services)) {
      return true;
    }
    const String target = noteTargetFromName(services, name);
    if (target.length() == 0) {
      Serial.println("error: invalid filename");
      return true;
    }
    startMultiline(services, multiline, target, MultilineBuffer::SaveMode::Overwrite);
    return true;
  }

  if (strcmp(verb, "append") == 0) {
    char* cursor = args;
    const char* token = cmdargs::nextToken(cursor);
    String path;
    if (token == nullptr || !resolveNote(services, token, path)) {
      if (token == nullptr) {
        Serial.println("usage: notes append <id|path>");
      }
      return true;
    }
    startMultiline(services, multiline, path, MultilineBuffer::SaveMode::Append);
    return true;
  }

  if (strcmp(verb, "rename") == 0) {
    char* cursor = args;
    const char* token = cmdargs::nextToken(cursor);
    const char* title = cmdargs::rest(cursor);
    String path;
    if (token == nullptr || title == nullptr) {
      Serial.println("usage: notes rename <id> \"<new title>\"");
      return true;
    }
    if (!resolveNote(services, token, path)) {
      return true;
    }
    String newPath;
    if (services.notes->rename(path.c_str(), title, newPath)) {
      Serial.printf("renamed: %s\n", newPath.c_str());
      s_listValid = false;
    } else {
      Serial.println("error: rename failed");
    }
    return true;
  }

  if (strcmp(verb, "favorite") == 0 || strcmp(verb, "pin") == 0) {
    char* cursor = args;
    const char* token = cmdargs::nextToken(cursor);
    const char* state = cmdargs::nextToken(cursor);
    String path;
    if (token == nullptr || state == nullptr ||
        (strcmp(state, "on") != 0 && strcmp(state, "off") != 0)) {
      Serial.printf("usage: notes %s <id> on|off\n", verb);
      return true;
    }
    if (!resolveNote(services, token, path)) {
      return true;
    }
    const bool on = strcmp(state, "on") == 0;
    const bool ok = strcmp(verb, "favorite") == 0
                        ? services.notes->setFavorite(path.c_str(), on)
                        : services.notes->setPinned(path.c_str(), on);
    Serial.println(ok ? "ok" : "error: could not update");
    s_listValid = false;
    return true;
  }

  if (strcmp(verb, "delete") == 0) {
    char* cursor = args;
    const char* token = cmdargs::nextToken(cursor);
    const char* confirm = cmdargs::nextToken(cursor);
    String path;
    if (token == nullptr) {
      Serial.println("usage: notes delete <id|path> confirm");
      return true;
    }
    if (!resolveNote(services, token, path)) {
      return true;
    }
    if (confirm == nullptr || strcmp(confirm, "confirm") != 0) {
      Serial.printf("this deletes %s — run: notes delete %s confirm\n", path.c_str(), token);
      return true;
    }
    Serial.println(services.notes->remove(path.c_str()) ? "deleted" : "error: delete failed");
    s_listValid = false;
    return true;
  }

  if (strcmp(verb, "export") == 0) {
    char* cursor = args;
    const char* token = cmdargs::nextToken(cursor);
    String path;
    if (token == nullptr || !resolveNote(services, token, path)) {
      if (token == nullptr) {
        Serial.println("usage: notes export <id|path>");
      }
      return true;
    }
    const char* slash = strrchr(path.c_str(), '/');
    const String dest = String(paths::kExports) + (slash != nullptr ? slash : "/export.md");
    if (services.storage->copyFile(path.c_str(), dest.c_str())) {
      Serial.printf("exported: %s\n", dest.c_str());
    } else {
      Serial.println("error: export failed");
    }
    return true;
  }

  if (strcmp(verb, "import") == 0) {
    char* cursor = args;
    const char* from = cmdargs::nextToken(cursor);
    if (from == nullptr) {
      Serial.println("usage: notes import <path>  (copies into /littlecube/notes/text)");
      return true;
    }
    const char* slash = strrchr(from, '/');
    const String dest = String(paths::kNotesText) + (slash != nullptr ? slash : "/imported.md");
    if (services.storage->copyFile(from, dest.c_str())) {
      Serial.printf("imported: %s\n", dest.c_str());
      s_listValid = false;
    } else {
      Serial.println("error: import failed (path allowed? card writable?)");
    }
    return true;
  }

  return false;
}
