#pragma once

// SD card lifecycle states (spec §31). UI and serial must report the
// specific state, never a generic "storage error".
enum class SdCardState {
  NotPresent,
  Mounting,
  Mounted,
  ReadOnly,
  UnsupportedFilesystem,
  Corrupted,
  Full,
  RemovedUnexpectedly,
  Error,
};

const char* sdCardStateName(SdCardState state);
