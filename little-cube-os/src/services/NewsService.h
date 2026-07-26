#pragma once

#include <Arduino.h>

class WifiService;
class EventBus;

// One HTTPS pull of the BBC News RSS feed (no key) on a short-lived FreeRTOS
// task — the same shape as OpenMeteoWeatherService. The task does the TLS GET
// + a lightweight RSS <item> scan into a staging array that the update() tick
// consumes, so the UI loop never blocks. The last good pull is cached to
// LittleFS and served (age-labelled) when offline. A permanently-failing fetch
// backs off instead of respawning the TLS task every tick. HTTPS uses
// WiFiClientSecure::setInsecure() — no cert store on-device, the documented
// tradeoff, identical to weather.
struct Headline {
  char title[128] = "";
  // RSS <description>, tag-stripped and ASCII-sanitized. 256 is headroom over
  // the feed itself: BBC descriptions measure ~50-172 chars, so nothing the
  // source publishes is ever truncated here.
  char summary[256] = "";
  char url[160] = "";  // empty when an item carries no <link>
};

class NewsService {
 public:
  static constexpr size_t kMaxHeadlines = 20;

  // Mirrors weather's attach: WifiService for internet() gating, EventBus to
  // announce a fresh set. No SettingsService — the feed needs no location.
  void attach(WifiService* wifi, EventBus* events);
  void begin();
  void update(uint32_t deltaMs);

  // Kicks an asynchronous refresh; returns false when offline or busy.
  bool refresh();

  size_t count() const { return count_; }
  const Headline& headline(size_t i) const;
  bool fetching() const { return fetchState_ == 1; }

  // Bumps once per successfully applied set, so a foreground app can redraw the
  // instant new headlines land instead of waiting on the ~1 Hz status tick.
  uint32_t generation() const { return generation_; }

  // Honest, self-describing freshness (spec §11/§13 style): "no news yet",
  // "cached (before restart)", "updated Nm ago", "updated Nh ago".
  void freshness(char* out, size_t cap) const;

 private:
  friend void newsFetchTask(void* arg);

  void loadCache();
  void saveCache();
  void backOff();

  WifiService* wifi_ = nullptr;
  EventBus* events_ = nullptr;

  Headline headlines_[kMaxHeadlines];
  size_t count_ = 0;
  uint32_t fetchedAtEpoch_ = 0;
  uint32_t fetchedAtUptimeMs_ = 0;  // 0 = cached from disk (before this boot)
  uint32_t generation_ = 0;

  // Staging written by the fetch task, consumed on the main loop. The task
  // fully populates it before setting fetchState_ = 2, and the loop reads it
  // only after seeing 2 — the same single-writer handoff weather relies on.
  Headline staging_[kMaxHeadlines];
  volatile size_t stagingCount_ = 0;
  volatile int fetchState_ = 0;  // 0 idle, 1 running, 2 success, 3 failed

  uint32_t sinceFetchMs_ = 0;
  uint32_t retryDelayMs_ = 0;  // 0 = no failure pending; else the current backoff
  bool everFetched_ = false;
};
