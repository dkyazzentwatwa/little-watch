#include "FilesCommands.h"

#include <Arduino.h>
#include <SD_MMC.h>

#include "../../core/Services.h"
#include "../../hardware/SdCardAdapter.h"
#include "../../storage/SdStorage.h"
#include "../../storage/StoragePaths.h"
#include "../CmdArgs.h"

namespace {

bool requireCard(Services& services) {
  if (!services.sdCard->mounted()) {
    Serial.printf("error: SD card %s\n", sdCardStateName(services.sdCard->state()));
    return false;
  }
  return true;
}

bool sanitized(Services& services, const char* raw, String& out, const char* fallback) {
  const char* candidate = raw != nullptr ? raw : fallback;
  if (candidate == nullptr || !services.storage->sanitizePath(candidate, out)) {
    Serial.println("error: invalid path (must stay inside /littlecube or /cypher-puter/desk)");
    return false;
  }
  return true;
}

void listOne(fs::File& entry, const char* parent) {
  if (entry.isDirectory()) {
    Serial.printf("  %s/\n", entry.name());
  } else {
    Serial.printf("  %-32s %8u B\n", entry.name(), (unsigned)entry.size());
  }
  (void)parent;
}

void printTree(const char* path, uint8_t depth, uint8_t maxDepth) {
  fs::File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) {
    return;
  }
  for (fs::File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    for (uint8_t i = 0; i < depth; i++) {
      Serial.print("  ");
    }
    if (entry.isDirectory()) {
      Serial.printf("%s/\n", entry.name());
      if (depth + 1 < maxDepth) {
        const String child = String(path) + "/" + entry.name();
        printTree(child.c_str(), depth + 1, maxDepth);
      }
    } else {
      Serial.printf("%s (%u B)\n", entry.name(), (unsigned)entry.size());
    }
    entry.close();
  }
  dir.close();
}

}  // namespace

void printFilesHelp() {
  Serial.println("files list [path]             list a directory (default /littlecube)");
  Serial.println("files tree [path]             directory tree, 3 levels deep");
  Serial.println("files cat <path>              print a text file (8 KB cap)");
  Serial.println("files mkdir <path>            create a directory");
  Serial.println("files copy <src> <dst>        copy a file");
  Serial.println("files move <src> <dst>        move / rename a file");
  Serial.println("files rename <src> <newname>  rename within the same folder");
  Serial.println("files delete <path> confirm   delete a file or empty folder");
}

bool handleFilesCommand(Services& services, const char* verb, char* args) {
  if (services.storage == nullptr || services.sdCard == nullptr) {
    return false;
  }

  if (strcmp(verb, "list") == 0) {
    if (!requireCard(services)) {
      return true;
    }
    char* cursor = args;
    String path;
    if (!sanitized(services, cmdargs::nextToken(cursor), path, paths::kRoot)) {
      return true;
    }
    fs::File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) {
      Serial.println("error: not a directory");
      return true;
    }
    Serial.printf("%s:\n", path.c_str());
    size_t count = 0;
    for (fs::File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      listOne(entry, path.c_str());
      entry.close();
      count++;
    }
    dir.close();
    if (count == 0) {
      Serial.println("  (empty)");
    }
    return true;
  }

  if (strcmp(verb, "tree") == 0) {
    if (!requireCard(services)) {
      return true;
    }
    char* cursor = args;
    String path;
    if (!sanitized(services, cmdargs::nextToken(cursor), path, paths::kRoot)) {
      return true;
    }
    Serial.printf("%s:\n", path.c_str());
    printTree(path.c_str(), 0, 3);
    return true;
  }

  if (strcmp(verb, "cat") == 0) {
    if (!requireCard(services)) {
      return true;
    }
    char* cursor = args;
    String path;
    if (!sanitized(services, cmdargs::nextToken(cursor), path, nullptr)) {
      return true;
    }
    fs::File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
      Serial.println("error: not a readable file");
      return true;
    }
    size_t shown = 0;
    char chunk[129];
    while (f.available() > 0 && shown < 8192) {
      const size_t n = f.readBytes(chunk, sizeof(chunk) - 1);
      if (n == 0) {
        break;
      }
      chunk[n] = '\0';
      Serial.print(chunk);
      shown += n;
    }
    if (f.available() > 0) {
      Serial.println("\n... (truncated at 8 KB)");
    } else {
      Serial.println();
    }
    f.close();
    return true;
  }

  if (strcmp(verb, "mkdir") == 0) {
    char* cursor = args;
    const char* path = cmdargs::nextToken(cursor);
    if (path == nullptr) {
      Serial.println("usage: files mkdir <path>");
      return true;
    }
    Serial.println(services.storage->makeDir(path) ? "ok" : "error: mkdir failed");
    return true;
  }

  if (strcmp(verb, "copy") == 0 || strcmp(verb, "move") == 0) {
    char* cursor = args;
    const char* src = cmdargs::nextToken(cursor);
    const char* dst = cmdargs::nextToken(cursor);
    if (src == nullptr || dst == nullptr) {
      Serial.printf("usage: files %s <src> <dst>\n", verb);
      return true;
    }
    bool ok;
    if (strcmp(verb, "copy") == 0) {
      ok = services.storage->copyFile(src, dst);
    } else {
      ok = services.storage->renamePath(src, dst);
    }
    Serial.println(ok ? "ok" : "error: operation failed");
    return true;
  }

  if (strcmp(verb, "rename") == 0) {
    char* cursor = args;
    const char* src = cmdargs::nextToken(cursor);
    const char* newName = cmdargs::nextToken(cursor);
    if (src == nullptr || newName == nullptr || strchr(newName, '/') != nullptr) {
      Serial.println("usage: files rename <src> <newname>   (newname has no slashes)");
      return true;
    }
    String safeSrc;
    if (!sanitized(services, src, safeSrc, nullptr)) {
      return true;
    }
    const int slash = safeSrc.lastIndexOf('/');
    const String dst = safeSrc.substring(0, slash + 1) + newName;
    Serial.println(services.storage->renamePath(safeSrc.c_str(), dst.c_str())
                       ? "ok"
                       : "error: rename failed");
    return true;
  }

  if (strcmp(verb, "delete") == 0) {
    char* cursor = args;
    const char* path = cmdargs::nextToken(cursor);
    const char* confirm = cmdargs::nextToken(cursor);
    if (path == nullptr) {
      Serial.println("usage: files delete <path> confirm");
      return true;
    }
    if (confirm == nullptr || strcmp(confirm, "confirm") != 0) {
      Serial.printf("this deletes %s — run: files delete %s confirm\n", path, path);
      return true;
    }
    Serial.println(services.storage->removeFile(path) ? "deleted"
                                                      : "error: delete failed (missing? non-empty dir?)");
    return true;
  }

  return false;
}
