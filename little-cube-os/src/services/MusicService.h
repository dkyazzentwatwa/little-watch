#pragma once

#include <Arduino.h>

class SdStorage;

// Local audio library (spec §24, Music + Podcasts). A pull-only directory
// scanner over the SD card — no task, no update(deltaMs) slot, no cached
// state. AudioApp calls list() on open / refresh / page-turn; nothing here
// ever runs from the render path or the kernel loop.
//
// The decode + playback engine lives in AudioAdapter (requestPlayMusicFile);
// this service only answers "what tracks are on the card, in order?".
struct TrackInfo {
  char path[160] = "";  // absolute SD path, e.g. /littlecube/music/song.mp3
  char name[64] = "";   // leaf filename shown in the UI
  size_t sizeBytes = 0;
};

class MusicService {
 public:
  // Stores the storage pointer only; touches the card lazily, per call.
  void begin(SdStorage* storage);

  // Fills out[] with up to maxItems tracks from `dir`, sorted case-insensitively
  // by filename (a stable total order — case is the tiebreak — so keyset paging
  // never skips or duplicates a row). Returns how many were written.
  //
  // `dir` is a directory constant from StoragePaths.h (kMusic /
  // kPodcastDownloads). Only .mp3/.wav/.flac/.aac/.m4a are counted; hidden
  // and AppleDouble (._*) entries are ignored.
  //
  // `totalOut`, when given, receives the TRUE number of playable tracks in the
  // directory (not the number returned) so the UI's "+N more" stays honest.
  //
  // `after` is keyset paging: pass the name of the previous page's last row to
  // get the next page. Tolerates a missing, unmounted, or Full card (Full is
  // still readable) — returns 0, never crashes.
  size_t list(const char* dir, TrackInfo* out, size_t maxItems, size_t* totalOut = nullptr,
              const char* after = nullptr);

 private:
  SdStorage* storage_ = nullptr;
};
