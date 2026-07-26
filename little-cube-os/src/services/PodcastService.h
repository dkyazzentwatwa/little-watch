#pragma once

#include <Arduino.h>

class SdStorage;
class WifiService;
class EventBus;
class HTTPClient;

// Podcast downloader (spec §24, Podcasts). Subscribe to RSS feeds and pull the
// latest episode's audio onto the SD card so it plays through the existing
// Audio -> Podcasts screen, which scans paths::kPodcastDownloads.
//
// Shape copied from OpenMeteoWeatherService: the network work (RSS fetch, then
// MP3 download) runs on one short-lived FreeRTOS task that only writes into a
// staging area guarded by fetchState_; update() on the main loop reaps the
// result. The main loop never blocks on the network or the card.
//
// Nothing here is streamed through RAM: the RSS body is scanned incrementally
// for the first <enclosure> and thrown away chunk by chunk; the episode is
// streamed straight to <name>.mp3.partial in fixed chunks and renamed only on a
// clean finish (AtomicFile::finalizePartial), so a half-download never looks
// like a real episode (the .partial extension is ignored by the library scan).
//
// One download at a time. Everything degrades: a missing/Full card or no
// internet is reported, never a crash.
class PodcastService {
 public:
  enum class State : uint8_t { Idle, FetchingFeed, Downloading, Done, Failed };

  static constexpr size_t kMaxFeeds = 16;
  static constexpr size_t kMaxUrlLen = 300;  // subscribed feed URL cap

  struct DownloadInfo {
    char name[80] = "";
    size_t sizeBytes = 0;
  };

  void begin(SdStorage* storage, WifiService* wifi, EventBus* events);
  void update(uint32_t deltaMs);

  // --- feed subscriptions (persisted to kPodcastFeeds/feeds.txt) -----------
  // add: validates the URL (http/https, no spaces/control chars), de-dupes,
  // caps at kMaxFeeds, writes atomically. Needs a writable card.
  bool addFeed(const char* url);
  bool removeFeed(size_t index);  // 0-based, as listed by listFeeds
  // Fills out[0..n) with subscribed URLs; returns how many (<= maxItems).
  size_t listFeeds(String* out, size_t maxItems);
  size_t feedCount();

  // --- the pipeline --------------------------------------------------------
  // Kicks RSS-fetch-then-download for feed `index` (default the first feed) or
  // for an arbitrary feed URL. Returns false if refused (busy, no internet, no
  // card, bad index) — message() then explains. One download at a time.
  bool fetchLatest(size_t feedIndex = 0);
  bool fetchLatestUrl(const char* feedUrl);

  // --- downloaded episodes on the card ------------------------------------
  size_t listDownloads(DownloadInfo* out, size_t maxItems, size_t* totalOut = nullptr);

  // --- status --------------------------------------------------------------
  State state() const;
  bool busy() const { return fetchState_ == 1 || fetchState_ == 2; }
  uint8_t percent() const;
  uint32_t downloadedBytes() const { return downloadedBytes_; }
  uint32_t totalBytes() const { return totalBytes_; }
  const char* message() const { return message_; }
  const char* lastFile() const { return lastFinalName_; }

  // Progress hooks used by the file-scope download streamer as it writes.
  void reportDownloadTotal(uint32_t bytes) { totalBytes_ = bytes; }
  void setDownloadedBytes(uint32_t bytes) { downloadedBytes_ = bytes; }
  SdStorage* storage() const { return storage_; }

 private:
  friend void podcastFetchTask(void* arg);

  bool kick(const char* feedUrl);
  size_t readAllFeeds(String* out);      // up to kMaxFeeds; returns count
  bool writeFeeds(String* feeds, size_t count);
  String feedsFilePath() const;

  SdStorage* storage_ = nullptr;
  WifiService* wifi_ = nullptr;
  EventBus* events_ = nullptr;  // stored for the required signature; no podcast
                                // SystemEvent exists to publish (see the report)

  // Written by the fetch task, consumed on the main loop. fetchState_ is the
  // release flag flipped last, exactly like the weather service.
  volatile uint8_t fetchState_ = 0;  // 0 idle, 1 RSS, 2 downloading, 3 done, 4 failed
  volatile uint32_t downloadedBytes_ = 0;
  volatile uint32_t totalBytes_ = 0;
  uint8_t lastOutcome_ = 0;  // 0 none, 1 done, 2 failed — persists after reaping

  char message_[96] = "";
  char pendingUrl_[kMaxUrlLen] = "";  // feed URL handed to the task
  char mp3Url_[768] = "";             // enclosure URL (tracking prefixes get long)
  char episodeTitle_[160] = "";
  char finalPath_[200] = "";
  char lastFinalName_[80] = "";
};
