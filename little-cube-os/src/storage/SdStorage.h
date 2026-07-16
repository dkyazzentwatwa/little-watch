#pragma once

#include <Arduino.h>

class SdCardAdapter;

// SD content layer above the raw card: directory-tree creation, sanitized
// path handling, and simple listing helpers. Every path arriving from
// serial or UI goes through sanitizePath() — file operations outside
// /littlecube/ and the deck interop root are refused.
class SdStorage {
 public:
  void begin(SdCardAdapter* card);

  // Creates the full /littlecube/ tree (spec §30). Safe to call whenever a
  // card mounts; only creates what is missing.
  bool ensureTree();

  // Canonicalizes and validates a user-supplied path. Returns true and
  // writes the safe absolute path into out when the path is inside an
  // allowed root; rejects "..", backslashes, control characters.
  bool sanitizePath(const char* raw, String& out) const;

  // Sanitizing wrappers used by the serial file commands. Every mutation
  // of the card from user input funnels through these.
  bool makeDir(const char* path);
  bool removeFile(const char* path);
  bool renamePath(const char* from, const char* to);
  bool copyFile(const char* from, const char* to);

  SdCardAdapter* card() { return card_; }

 private:
  SdCardAdapter* card_ = nullptr;
};
