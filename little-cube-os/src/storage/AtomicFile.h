#pragma once

#include <Arduino.h>

namespace fs {
class FS;
}

// Crash-safe writes (spec §35), same protocol the CrowPanel deck proved:
//   write <path>.tmp -> verify byte count -> flush/close ->
//   rename existing to <path>.bak -> rename .tmp to path -> delete .bak.
// Long-running writers (recordings, downloads) instead write <path>.partial
// and rename only on successful completion.
namespace AtomicFile {

// Writes data atomically. Returns false and cleans up the temp on failure.
bool writeAll(fs::FS& fs, const char* path, const uint8_t* data, size_t length);
bool writeAll(fs::FS& fs, const char* path, const String& text);

// Removes stale .tmp/.bak siblings left by an interrupted write.
void cleanupSiblings(fs::FS& fs, const char* path);

// <path>.partial helpers for long-running writes.
String partialPath(const char* path);
bool finalizePartial(fs::FS& fs, const char* path);

}  // namespace AtomicFile
