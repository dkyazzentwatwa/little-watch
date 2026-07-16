#pragma once

#include <Arduino.h>
#include <FS.h>

// Multiline text entry over serial (spec §18). Control lines (own line,
// case-sensitive, never stored):
//   .END      save    |  .CANCEL  discard
//   .PREVIEW  echo    |  .CLEAR   restart
// Content streams to an SD temp file as it arrives — RAM holds one line at
// a time — and lands on the target via an atomic backup-rename on save.
// Total input is capped at kMaxBytes.
class MultilineBuffer {
 public:
  enum class Result {
    Collecting,
    Saved,
    Cancelled,
    Error,
  };
  enum class SaveMode {
    Overwrite,  // temp replaces the target atomically
    Append,     // temp content is appended to the target
  };

  bool start(fs::FS& fs, const char* tempPath, const char* targetPath, SaveMode mode);
  bool active() const { return active_; }
  Result feedLine(const char* line);
  const char* targetPath() const { return targetPath_.c_str(); }
  size_t bytes() const { return bytes_; }

  static constexpr size_t kMaxBytes = 64 * 1024;

 private:
  void abort();
  Result finish();

  fs::FS* fs_ = nullptr;
  fs::File file_;
  String tempPath_;
  String targetPath_;
  SaveMode mode_ = SaveMode::Overwrite;
  size_t bytes_ = 0;
  bool active_ = false;
};
