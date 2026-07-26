#pragma once

#include <Arduino.h>
#include <FS.h>

// Multiline text entry over serial (spec §18). Control lines (own line,
// case-sensitive, never stored):
//   .END      save    |  .CANCEL  discard
//   .PREVIEW  echo    |  .CLEAR   restart
// Content streams to an SD temp file as it arrives — RAM holds one line at
// a time — and lands on the target via an atomic backup-rename on save.
// Total input is capped at kMaxBytes. Only .CANCEL deletes the temp file:
// every failure path keeps it, so a save into a missing directory (or onto a
// full card) never silently swallows what the user typed.
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
  // After Result::Error: the surviving temp file holding everything typed
  // (`files cat <path>` recovers it), or nullptr when nothing was kept.
  const char* recoveryPath() const { return tempKept_ ? tempPath_.c_str() : nullptr; }
  size_t bytes() const { return bytes_; }

  static constexpr size_t kMaxBytes = 64 * 1024;

 private:
  void abort(bool removeTemp);
  Result finish();

  fs::FS* fs_ = nullptr;
  fs::File file_;
  String tempPath_;
  String targetPath_;
  SaveMode mode_ = SaveMode::Overwrite;
  size_t bytes_ = 0;
  bool active_ = false;
  bool tempKept_ = false;  // an error left the temp file on disk
};
