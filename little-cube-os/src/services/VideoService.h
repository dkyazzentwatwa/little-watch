#pragma once

#include <Arduino.h>

#include "../feature_flags.h"

#if FEATURE_VIDEO

#include <Preferences.h>

class SdStorage;

// Video library + resume positions + folder queue. Pull-only like
// MusicService: no task, no update(deltaMs) slot — VideoApp and the serial
// family call in, nothing runs from the render path.
struct VideoInfo {
  char path[160] = "";  // absolute SD path, e.g. /littlecube/video/Show/ep01.lcv
  // 64 chars + NUL. MUST match the sort-key name budget in list() and the
  // page anchors VideoApp builds from this field — a name truncated shorter
  // than the key re-includes its row at a page boundary.
  char name[65] = "";
  uint32_t durationMs = 0;  // 0 for directories
  bool isDir = false;
};

class VideoService {
 public:
  void begin(SdStorage* storage);

  // Widest visible window a single list() call can fill.
  static constexpr size_t kMaxListWindow = 8;

  // Same shape as MusicService::list — sorted case-insensitively with an
  // exact-bytes tiebreak, totalOut = the TRUE count — with two deviations a
  // caller must know:
  //   * `after` is the internal SORT KEY of the previous page's last row
  //     ('0'/'1' dir/file prefix + up to 64 name chars — build it exactly
  //     the way makeKey() in the .cpp does), NOT a bare filename.
  //   * maxItems is clamped to kMaxListWindow.
  // Directories sort before files ("seasons" one level deep). Duration is
  // read from each visible file's 64-byte header after the window is final.
  size_t list(const char* dir, VideoInfo* out, size_t maxItems, size_t* totalOut = nullptr,
              const char* after = nullptr);

  // Next / previous .lcv sibling of currentPath in sort order. False at the
  // ends of the folder.
  bool nextInFolder(const char* currentPath, char* outPath, size_t outLen);
  bool prevInFolder(const char* currentPath, char* outPath, size_t outLen);

  // Resume store: one NVS blob of up to 32 {crc32(path), posMs, seq}
  // records, LRU-evicted by seq. Positions under 30 s are dropped;
  // >= 95 % of durationMs counts as finished and clears the record.
  uint32_t resumeMs(const char* path);  // 0 = start from the beginning
  void savePosition(const char* path, uint32_t posMs, uint32_t durationMs);
  void clearPosition(const char* path);

 private:
  struct ResumeRec {
    uint32_t pathCrc = 0;
    uint32_t posMs = 0;
    uint32_t seq = 0;
  };
  static constexpr size_t kMaxResume = 32;
  static constexpr uint32_t kMinSaveMs = 30000;

  void loadResume();
  void storeResume();
  int findResume(uint32_t crc) const;
  bool sibling(const char* currentPath, bool forward, char* outPath, size_t outLen);

  SdStorage* storage_ = nullptr;
  Preferences prefs_;
  bool prefsReady_ = false;
  bool resumeLoaded_ = false;
  ResumeRec resume_[kMaxResume];
  size_t resumeCount_ = 0;
  uint32_t seq_ = 0;
};

#endif  // FEATURE_VIDEO
