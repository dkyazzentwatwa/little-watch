#include "NewsService.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include <time.h>

#include "../core/EventBus.h"
#include "../storage/AtomicFile.h"
#include "WifiService.h"

namespace {

// LittleFS root path, exactly like weather's kCachePath. Deliberately NOT under
// the SD /littlecube/cache tree: that tree only exists on the SD card, whereas
// the news cache must survive a missing card (news is a Wi-Fi feature, SD is
// not required to boot), and AtomicFile does not create parent directories — a
// subdir path on LittleFS would silently fail to write.
constexpr const char* kCachePath = "/news_cache.json";
// BBC News top stories, RSS 2.0, no key. Chosen over the HN Algolia API
// because RSS <description> carries a real summary per story — HN only has
// titles. ~26 KB for ~30 items; kMaxHeadlines caps what we keep.
constexpr const char* kFeedUrl = "https://feeds.bbci.co.uk/news/rss.xml";

constexpr uint32_t kAutoRefreshMs = 30UL * 60UL * 1000UL;  // gentle 30-min refresh
constexpr uint32_t kFirstRetryMs = 60UL * 1000UL;          // first retry after a failure
constexpr uint32_t kMaxRetryMs = kAutoRefreshMs;           // backoff ceiling

// Decode the inner text of one RSS element into dst: unwraps an optional CDATA
// wrapper, strips any embedded markup tags, decodes the five named XML
// entities plus numeric &#NN;/&#xNN; references, and coalesces every
// non-printable-ASCII result into a single '?'. The built-in 6x8 GFX font is
// ASCII, so raw UTF-8 ("Café", curly quotes) would render as garbage glyphs;
// this keeps text legible without shipping a Unicode font. Runs on the fetch
// task, off the UI loop.
void decodeRssText(const char* src, size_t len, char* dst, size_t cap) {
  if (dst == nullptr || cap == 0) {
    return;
  }
  // <![CDATA[ ... ]]> — content is literal, but still gets tag-strip/sanitize
  // below, which is harmless for BBC's plain-text payloads and defensive
  // against feeds that nest HTML inside CDATA.
  if (len >= 12 && strncmp(src, "<![CDATA[", 9) == 0 &&
      strncmp(src + len - 3, "]]>", 3) == 0) {
    src += 9;
    len -= 12;
  }
  size_t o = 0;
  bool lastBad = false;
  for (size_t i = 0; i < len && o < cap - 1;) {
    const unsigned char c = static_cast<unsigned char>(src[i]);
    if (c == '<') {  // skip a markup tag wholesale
      while (i < len && src[i] != '>') {
        i++;
      }
      i++;  // past '>'
      continue;
    }
    char out = 0;
    if (c == '&') {
      // Entity reference: find the ';' within a short window.
      size_t semi = i + 1;
      const size_t limit = i + 10 < len ? i + 10 : len;
      while (semi < limit && src[semi] != ';') {
        semi++;
      }
      if (semi < limit) {
        const char* e = src + i + 1;
        const size_t elen = semi - i - 1;
        if (elen == 3 && strncmp(e, "amp", 3) == 0) {
          out = '&';
        } else if (elen == 2 && strncmp(e, "lt", 2) == 0) {
          out = '<';
        } else if (elen == 2 && strncmp(e, "gt", 2) == 0) {
          out = '>';
        } else if (elen == 4 && strncmp(e, "quot", 4) == 0) {
          out = '"';
        } else if (elen == 4 && strncmp(e, "apos", 4) == 0) {
          out = '\'';
        } else if (elen >= 2 && e[0] == '#') {
          const long v = (e[1] == 'x' || e[1] == 'X') ? strtol(e + 2, nullptr, 16)
                                                      : strtol(e + 1, nullptr, 10);
          out = (v >= 0x20 && v <= 0x7E) ? static_cast<char>(v) : 0;
        }
        i = semi + 1;
      } else {
        i++;  // stray '&' with no terminator — treat as unmappable
      }
    } else {
      out = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : 0;
      i++;
    }
    if (out != 0) {
      dst[o++] = out;
      lastBad = false;
    } else if (!lastBad) {
      dst[o++] = '?';
      lastBad = true;
    }
  }
  dst[o] = '\0';
}

// Extract <tag>...</tag> from body[from..to) into dst (decoded/sanitized).
// Returns false (and empties dst) when the tag is absent from the span.
bool extractTag(const String& body, int from, int to, const char* openTag,
                const char* closeTag, char* dst, size_t cap) {
  if (dst != nullptr && cap > 0) {
    dst[0] = '\0';
  }
  const int open = body.indexOf(openTag, from);
  if (open < 0 || open >= to) {
    return false;
  }
  const int start = open + static_cast<int>(strlen(openTag));
  const int close = body.indexOf(closeTag, start);
  if (close < 0 || close > to) {
    return false;
  }
  decodeRssText(body.c_str() + start, static_cast<size_t>(close - start), dst, cap);
  return true;
}

bool httpGetBody(const char* url, String& body) {
  WiFiClientSecure client;
  client.setInsecure();  // no cert store on-device; documented tradeoff (matches weather)
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    return false;
  }
  http.useHTTP10(true);  // identity encoding, not chunked
  const int code = http.GET();
  bool ok = false;
  if (code == 200) {
    // Buffer the whole body (~26 KB) before scanning; getString() drains to
    // the connection close, so the scan always sees a complete document.
    body = http.getString();
    ok = body.length() > 0;
  } else {
    Serial.printf("[news] http %d\n", code);
  }
  http.end();
  return ok;
}

}  // namespace

void newsFetchTask(void* arg) {
  NewsService* self = static_cast<NewsService*>(arg);

  String body;
  if (!httpGetBody(kFeedUrl, body)) {
    self->fetchState_ = 3;
    vTaskDelete(nullptr);
    return;
  }

  // Scan <item> blocks in document order — no XML library on the build, and
  // RSS 2.0 is regular enough that a bounded tag scan is dependable. Channel
  // metadata (its own <title>/<description>) sits before the first <item>, so
  // scoping every lookup to one item's span never picks it up.
  size_t n = 0;
  int pos = 0;
  while (n < NewsService::kMaxHeadlines) {
    const int itemStart = body.indexOf("<item>", pos);
    if (itemStart < 0) {
      break;
    }
    const int itemEnd = body.indexOf("</item>", itemStart);
    if (itemEnd < 0) {
      break;
    }
    pos = itemEnd + 7;

    Headline& h = self->staging_[n];
    h = Headline();
    extractTag(body, itemStart, itemEnd, "<title>", "</title>", h.title, sizeof(h.title));
    if (h.title[0] == '\0') {
      continue;  // skip a malformed / titleless item rather than show a blank
    }
    extractTag(body, itemStart, itemEnd, "<description>", "</description>", h.summary,
               sizeof(h.summary));
    extractTag(body, itemStart, itemEnd, "<link>", "</link>", h.url, sizeof(h.url));
    n++;
  }

  self->stagingCount_ = n;
  // A 200 with zero usable items is a failure, not an "empty feed" truth —
  // back off rather than blanking the cached set.
  self->fetchState_ = n > 0 ? 2 : 3;
  if (n == 0) {
    Serial.printf("[news] no items parsed (%u bytes)\n", (unsigned)body.length());
  }
  vTaskDelete(nullptr);
}

void NewsService::attach(WifiService* wifi, EventBus* events) {
  wifi_ = wifi;
  events_ = events;
}

void NewsService::begin() {
  // Repair the cache file if a write was interrupted, before reading it.
  AtomicFile::cleanupSiblings(LittleFS, kCachePath);
  loadCache();
}

const Headline& NewsService::headline(size_t i) const {
  static const Headline kEmpty;
  return i < count_ ? headlines_[i] : kEmpty;
}

bool NewsService::refresh() {
  if (fetchState_ == 1) {
    return false;  // already running
  }
  if (wifi_ == nullptr || !wifi_->internet()) {
    Serial.println("[news] no internet; keeping cached headlines");
    return false;
  }
  fetchState_ = 1;
  // 16 KB: a touch more than weather's 12 KB because the parse walks ~20
  // items. The body String and mbedTLS buffers live on the heap, not here, so
  // this is call-frame + local headroom. Not hardware-verified.
  if (xTaskCreate(newsFetchTask, "news", 16384, this, 1, nullptr) != pdPASS) {
    // Leaving fetchState_ at 1 would wedge news for the rest of the boot,
    // because refresh() reads that as "a fetch is already running".
    fetchState_ = 0;
    Serial.println("[news] fetch task could not start (low memory)");
    return false;
  }
  return true;
}

// Failure backoff, doubling from a minute up to the normal refresh period, so a
// persistently unreachable feed (captive portal, DNS block) does not spawn a
// TLS task every tick. Mirrors weather's backOff().
void NewsService::backOff() {
  sinceFetchMs_ = 0;
  if (retryDelayMs_ == 0) {
    retryDelayMs_ = kFirstRetryMs;
  } else if (retryDelayMs_ < kMaxRetryMs / 2) {
    retryDelayMs_ *= 2;
  } else {
    retryDelayMs_ = kMaxRetryMs;
  }
}

void NewsService::update(uint32_t deltaMs) {
  sinceFetchMs_ += deltaMs;

  if (fetchState_ == 2) {
    fetchState_ = 0;
    const size_t n = stagingCount_ < kMaxHeadlines ? stagingCount_ : kMaxHeadlines;
    for (size_t i = 0; i < n; i++) {
      headlines_[i] = staging_[i];
    }
    count_ = n;
    everFetched_ = true;
    sinceFetchMs_ = 0;
    retryDelayMs_ = 0;
    fetchedAtUptimeMs_ = millis();
    const time_t nowEpoch = time(nullptr);
    fetchedAtEpoch_ = nowEpoch > 1700000000 ? static_cast<uint32_t>(nowEpoch) : 0;
    generation_++;
    saveCache();
    Serial.printf("[news] %u headlines\n", static_cast<unsigned>(count_));
    if (events_ != nullptr) {
      events_->publish(SystemEvent::NewsUpdated);
    }
  } else if (fetchState_ == 3) {
    fetchState_ = 0;
    backOff();
  }

  // Auto-refresh whenever the internet is up: fire immediately the first time
  // online (dueMs 0), then every kAutoRefreshMs, or after the current backoff.
  const uint32_t dueMs =
      retryDelayMs_ > 0 ? retryDelayMs_ : (everFetched_ ? kAutoRefreshMs : 0);
  if (wifi_ != nullptr && wifi_->internet() && fetchState_ == 0 && sinceFetchMs_ >= dueMs) {
    if (!refresh()) {
      // Refused (offline race, or the task would not start). Back off like a
      // failure so the reason is not re-printed every tick.
      backOff();
    }
  }
}

void NewsService::loadCache() {
  fs::File f = LittleFS.open(kCachePath, FILE_READ);
  if (!f) {
    return;
  }
  JsonDocument doc;
  const bool ok = !deserializeJson(doc, f);
  f.close();
  if (!ok) {
    return;
  }
  size_t n = 0;
  for (JsonVariantConst o : doc["h"].as<JsonArrayConst>()) {
    if (n >= kMaxHeadlines) {
      break;
    }
    Headline& h = headlines_[n];
    h = Headline();
    // Cached text was already ASCII-sanitized before writing, so copy directly.
    // A pre-summary cache (the HN era) simply loads with empty summaries.
    strncpy(h.title, o["t"] | "", sizeof(h.title) - 1);
    strncpy(h.summary, o["s"] | "", sizeof(h.summary) - 1);
    strncpy(h.url, o["u"] | "", sizeof(h.url) - 1);
    n++;
  }
  count_ = n;
  fetchedAtEpoch_ = doc["at"] | 0;
  fetchedAtUptimeMs_ = 0;  // readers treat 0 as "cached from disk"
  if (count_ > 0) {
    Serial.printf("[news] loaded %u cached headlines\n", static_cast<unsigned>(count_));
  }
}

void NewsService::saveCache() {
  JsonDocument doc;
  doc["at"] = fetchedAtEpoch_;
  JsonArray arr = doc["h"].to<JsonArray>();
  for (size_t i = 0; i < count_; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["t"] = headlines_[i].title;  // char[] -> copied into the document
    o["s"] = headlines_[i].summary;
    o["u"] = headlines_[i].url;
  }
  String body;
  serializeJson(doc, body);
  AtomicFile::writeAll(LittleFS, kCachePath, body);
}

void NewsService::freshness(char* out, size_t cap) const {
  if (out == nullptr || cap == 0) {
    return;
  }
  if (count_ == 0) {
    snprintf(out, cap, "no news yet");
    return;
  }
  if (fetchedAtUptimeMs_ == 0) {
    snprintf(out, cap, "cached (before restart)");
    return;
  }
  const uint32_t ageMin = (millis() - fetchedAtUptimeMs_) / 60000UL;
  if (ageMin < 1) {
    snprintf(out, cap, "updated just now");
  } else if (ageMin < 60) {
    snprintf(out, cap, "updated %lum ago", static_cast<unsigned long>(ageMin));
  } else {
    snprintf(out, cap, "updated %luh ago", static_cast<unsigned long>(ageMin / 60));
  }
}
