#include "AtomicFile.h"

#include <FS.h>

// TODO(task-8): full implementation with byte-count verification and
// rollback from .bak on failure.

namespace AtomicFile {

bool writeAll(fs::FS& fs, const char* path, const uint8_t* data, size_t length) {
  (void)fs;
  (void)path;
  (void)data;
  (void)length;
  return false;
}

bool writeAll(fs::FS& fs, const char* path, const String& text) {
  return writeAll(fs, path, reinterpret_cast<const uint8_t*>(text.c_str()), text.length());
}

void cleanupSiblings(fs::FS& fs, const char* path) {
  (void)fs;
  (void)path;
}

String partialPath(const char* path) {
  String result(path);
  result += ".partial";
  return result;
}

bool finalizePartial(fs::FS& fs, const char* path) {
  (void)fs;
  (void)path;
  return false;
}

}  // namespace AtomicFile
