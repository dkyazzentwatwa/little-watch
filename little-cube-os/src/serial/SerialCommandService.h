#pragma once

#include <Arduino.h>

#include "../core/Services.h"

// USB serial command interface (spec §16-§18): the cube's keyboard. Line
// based, non-blocking, bounded buffers; commands dispatch to shared
// services and never touch hardware or the filesystem directly. Passwords
// are never echoed or logged.
class SerialCommandService {
 public:
  void begin(Services* services);
  void update();

 private:
  void handleLine(char* line);

  Services* services_ = nullptr;

  static constexpr size_t kMaxLineLen = 256;
  char line_[kMaxLineLen];
  size_t lineLen_ = 0;
  bool overflowed_ = false;
};
