#pragma once

#include <Arduino.h>

// Multiline text entry over serial (spec §18). Control lines (own line,
// case-sensitive, never stored in the note):
//   .END      save   |  .CANCEL   discard
//   .PREVIEW  echo    |  .CLEAR    restart
// Input is bounded in RAM; large notes stream to an SD temp file and are
// atomically renamed on save.
class MultilineBuffer {
 public:
  enum class Result {
    Collecting,
    Saved,
    Cancelled,
  };

  void start(const char* targetPath);
  bool active() const { return active_; }
  Result feedLine(const char* line);

 private:
  bool active_ = false;
  String targetPath_;
  String buffer_;
};
