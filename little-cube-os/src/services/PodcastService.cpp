#include "PodcastService.h"

#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ctype.h>
#include <string.h>
#include <strings.h>  // strcasecmp

#include "../hardware/SdCardAdapter.h"
#include "../storage/AtomicFile.h"
#include "../storage/SdStorage.h"
#include "../storage/StoragePaths.h"
#include "WifiService.h"

namespace {

// Streaming buffers. Only one download runs at a time (guarded by fetchState_),
// so a single file-scope chunk buffer is safe and keeps the task stack light.
constexpr size_t kDlBufSize = 4096;
uint8_t s_dlBuf[kDlBufSize];

constexpr size_t kMaxScanBytes = 256u * 1024u;  // give up on a pathological feed
constexpr uint32_t kStallMs = 15000;            // no bytes for this long => abort
constexpr uint32_t kFlushEveryBytes = 64u * 1024u;
constexpr const char* kUserAgent = "LittleCubeOS/1.0 (+podcast)";
constexpr const char* kFeedsFile = "feeds.txt";

bool startsWithCI(const char* s, const char* prefix) {
  while (*prefix != '\0') {
    if (*s == '\0') return false;
    if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
    ++s;
    ++prefix;
  }
  return true;
}

// The saved extension for a URL whose path ends in a known audio extension.
// Returns nullptr when the path has no recognized audio extension.
const char* audioExtOf(const char* url) {
  const char* q = strpbrk(url, "?#");
  const char* end = q != nullptr ? q : url + strlen(url);
  const char* p = end;
  while (p > url && *(p - 1) != '.' && *(p - 1) != '/') {
    --p;
  }
  if (p == url || *(p - 1) != '.') {
    return nullptr;
  }
  char ext[8];
  size_t n = (size_t)(end - p);
  if (n == 0 || n >= sizeof(ext)) {
    return nullptr;
  }
  for (size_t i = 0; i < n; i++) {
    ext[i] = (char)tolower((unsigned char)p[i]);
  }
  ext[n] = '\0';
  if (strcmp(ext, "mp3") == 0) return ".mp3";
  if (strcmp(ext, "m4a") == 0 || strcmp(ext, "m4b") == 0) return ".m4a";
  if (strcmp(ext, "aac") == 0) return ".aac";
  return nullptr;
}

bool isAudioUrl(const char* url) { return audioExtOf(url) != nullptr; }

// Sanitizes a title or URL basename into a safe leaf filename: keeps
// [A-Za-z0-9-.], everything else (spaces, punctuation, UTF-8 bytes) becomes an
// underscore; runs of underscores collapse; no leading dot/underscore (so the
// file is never hidden nor an AppleDouble lookalike the library scan skips);
// trailing separators trimmed; capped so the full SD path stays well bounded.
void sanitizeLeaf(const char* in, char* out, size_t outCap) {
  size_t o = 0;
  bool lastUnderscore = false;
  const size_t cap = outCap > 49 ? 49 : outCap;  // <= 48 chars + NUL
  for (const char* p = in; *p != '\0' && o + 1 < cap; ++p) {
    unsigned char c = (unsigned char)*p;
    char ch;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '.') {
      ch = (char)c;
    } else {
      ch = '_';
    }
    if (ch == '_') {
      if (lastUnderscore || o == 0) {
        continue;  // no leading or repeated underscores
      }
      lastUnderscore = true;
    } else {
      if (o == 0 && ch == '.') {
        continue;  // no leading dot
      }
      lastUnderscore = false;
    }
    out[o++] = ch;
  }
  while (o > 0 && (out[o - 1] == '_' || out[o - 1] == '.')) {
    --o;
  }
  out[o] = '\0';
}

// Builds the destination path under kPodcastDownloads from the episode title
// (preferred) or the URL's basename, with a millis() fallback so we always have
// a name. The extension follows the source audio type; defaults to .mp3.
void deriveDownloadPath(const char* mp3Url, const char* title, char* out, size_t outCap) {
  char base[64];
  base[0] = '\0';
  if (title != nullptr && title[0] != '\0') {
    sanitizeLeaf(title, base, sizeof(base));
  }
  if (base[0] == '\0') {
    const char* q = strpbrk(mp3Url, "?#");
    const char* end = q != nullptr ? q : mp3Url + strlen(mp3Url);
    const char* slash = end;
    while (slash > mp3Url && *(slash - 1) != '/') {
      --slash;
    }
    char raw[96];
    size_t n = (size_t)(end - slash);
    if (n >= sizeof(raw)) {
      n = sizeof(raw) - 1;
    }
    memcpy(raw, slash, n);
    raw[n] = '\0';
    sanitizeLeaf(raw, base, sizeof(base));
  }
  if (base[0] == '\0') {
    snprintf(base, sizeof(base), "podcast_%lu", (unsigned long)millis());
  }

  const char* ext = audioExtOf(mp3Url);
  if (ext == nullptr) {
    ext = ".mp3";
  }
  // Drop a trailing copy of the same extension so we don't produce "x.mp3.mp3".
  const size_t blen = strlen(base);
  const size_t elen = strlen(ext);
  if (blen > elen && startsWithCI(base + (blen - elen), ext)) {
    base[blen - elen] = '\0';
    while (base[0] != '\0' && (base[strlen(base) - 1] == '_' || base[strlen(base) - 1] == '.')) {
      base[strlen(base) - 1] = '\0';
    }
  }
  if (base[0] == '\0') {
    snprintf(base, sizeof(base), "podcast_%lu", (unsigned long)millis());
  }
  snprintf(out, outCap, "%s/%s%s", paths::kPodcastDownloads, base, ext);
}

// Pulls the url="..." (or url='...') attribute out of an <enclosure ...> tag.
bool extractUrlAttr(const char* tag, char* out, size_t outCap) {
  for (const char* p = tag; *p != '\0'; ++p) {
    const bool boundary =
        (p == tag) || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r';
    if (!boundary) {
      continue;
    }
    if (tolower((unsigned char)p[0]) != 'u' || tolower((unsigned char)p[1]) != 'r' ||
        tolower((unsigned char)p[2]) != 'l') {
      continue;
    }
    const char* q = p + 3;
    while (*q == ' ' || *q == '\t') {
      ++q;
    }
    if (*q != '=') {
      continue;
    }
    ++q;
    while (*q == ' ' || *q == '\t') {
      ++q;
    }
    const char quote = *q;
    if (quote != '"' && quote != '\'') {
      continue;
    }
    ++q;
    const char* start = q;
    while (*q != '\0' && *q != quote) {
      ++q;
    }
    size_t len = (size_t)(q - start);
    if (len == 0 || len >= outCap) {
      len = len >= outCap ? outCap - 1 : len;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return out[0] != '\0';
  }
  return false;
}

void trimInPlace(char* s) {
  char* start = s;
  while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
    ++start;
  }
  if (start != s) {
    memmove(s, start, strlen(start) + 1);
  }
  size_t len = strlen(s);
  while (len > 0 &&
         (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) {
    s[--len] = '\0';
  }
}

bool beginRequest(HTTPClient& http, WiFiClientSecure& secure, WiFiClient& plain, const char* url) {
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // feeds 302 to a CDN
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.setReuse(false);
  http.setUserAgent(kUserAgent);
  // Force HTTP/1.0: getStreamPtr() returns the RAW client, and HTTPClient only
  // de-chunks inside writeToStream(). An HTTP/1.0 request forbids a chunked
  // response, so the body is Content-Length or close-delimited (identity) — no
  // chunk-framing bytes to corrupt the RSS scan or the saved MP3.
  http.useHTTP10(true);
  if (startsWithCI(url, "https:")) {
    secure.setInsecure();  // no cert store on-device; same tradeoff as weather
    return http.begin(secure, url);
  }
  return http.begin(plain, url);
}

// Incrementally scans an RSS response for the first <enclosure> whose url is an
// audio file, capturing the last <title> seen before it (the episode title in a
// well-formed feed: channel title, then item title, then the enclosure). A
// byte-oriented state machine, so a tag or CDATA block straddling a chunk
// boundary is handled by carried state, never by buffering the whole feed.
bool scanForEnclosure(HTTPClient& http, char* urlOut, size_t urlCap, char* titleOut,
                      size_t titleCap) {
  urlOut[0] = '\0';
  titleOut[0] = '\0';
  NetworkClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    return false;
  }

  enum Mode { TEXT, TAG, CDATA };
  Mode mode = TEXT;
  char tag[1024];
  size_t tagLen = 0;
  char titleAcc[160];
  size_t titleLen = 0;
  bool inTitle = false;
  int cdataClose = 0;  // count of trailing ']' while scanning for "]]>"
  char pendingTitle[160];
  pendingTitle[0] = '\0';

  uint8_t buf[512];
  size_t scanned = 0;
  uint32_t lastDataMs = millis();

  while (scanned < kMaxScanBytes) {
    if (!http.connected() && stream->available() == 0) {
      break;
    }
    const int avail = stream->available();
    if (avail <= 0) {
      if (millis() - lastDataMs > kStallMs) {
        break;
      }
      delay(5);
      continue;
    }
    size_t toRead = (size_t)avail > sizeof(buf) ? sizeof(buf) : (size_t)avail;
    size_t n = stream->readBytes(buf, toRead);
    if (n == 0) {
      if (millis() - lastDataMs > kStallMs) {
        break;
      }
      delay(5);
      continue;
    }
    lastDataMs = millis();
    scanned += n;

    for (size_t i = 0; i < n; i++) {
      const char c = (char)buf[i];
      if (mode == TEXT) {
        if (c == '<') {
          mode = TAG;
          tagLen = 0;
        } else if (inTitle && titleLen + 1 < sizeof(titleAcc)) {
          titleAcc[titleLen++] = c;
        }
      } else if (mode == CDATA) {
        if (c == '>' && cdataClose >= 2) {
          if (inTitle && titleLen >= 2) {
            titleLen -= 2;  // strip the "]]" we appended before the ">"
          }
          cdataClose = 0;
          mode = TEXT;
        } else {
          if (c == ']') {
            cdataClose++;
          } else {
            cdataClose = 0;
          }
          if (inTitle && titleLen + 1 < sizeof(titleAcc)) {
            titleAcc[titleLen++] = c;
          }
        }
      } else {  // TAG
        if (c == '>') {
          tag[tagLen] = '\0';
          mode = TEXT;
          char* t = tag;
          while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') {
            ++t;
          }
          if (startsWithCI(t, "enclosure") &&
              (t[9] == ' ' || t[9] == '\t' || t[9] == '\n' || t[9] == '\r')) {
            char u[768];
            if (extractUrlAttr(t, u, sizeof(u)) && isAudioUrl(u)) {
              strncpy(urlOut, u, urlCap - 1);
              urlOut[urlCap - 1] = '\0';
              strncpy(titleOut, pendingTitle, titleCap - 1);
              titleOut[titleCap - 1] = '\0';
              return true;
            }
          } else if (startsWithCI(t, "/title")) {
            titleAcc[titleLen] = '\0';
            trimInPlace(titleAcc);
            if (titleAcc[0] != '\0') {
              strncpy(pendingTitle, titleAcc, sizeof(pendingTitle) - 1);
              pendingTitle[sizeof(pendingTitle) - 1] = '\0';
            }
            inTitle = false;
            titleLen = 0;
          } else if (startsWithCI(t, "title") &&
                     (t[5] == '\0' || t[5] == ' ' || t[5] == '\t' || t[5] == '/')) {
            if (tagLen == 0 || tag[tagLen - 1] != '/') {  // not <title/>
              inTitle = true;
              titleLen = 0;
            }
          }
        } else {
          if (tagLen + 1 < sizeof(tag)) {
            tag[tagLen++] = c;
          }
          if (tagLen == 8 && memcmp(tag, "![CDATA[", 8) == 0) {
            mode = CDATA;
            cdataClose = 0;
            tagLen = 0;
          }
        }
      }
    }
  }
  return false;
}

// Streams the HTTP body straight to <finalPath>.partial in fixed chunks, then
// leaves the .partial for the caller to finalize. Fills a failure message and
// removes the .partial on any error (card full/removed, stall, short read).
bool downloadToPartial(HTTPClient& http, PodcastService* self, const char* finalPath, char* msg,
                       size_t msgCap) {
  const int len0 = http.getSize();  // -1 when the server sends no Content-Length
  self->reportDownloadTotal(len0 > 0 ? (uint32_t)len0 : 0);

  // Soft free-space preflight against the cached figure (no FAT walk here).
  SdCardAdapter* card = self->storage() != nullptr ? self->storage()->card() : nullptr;
  if (card == nullptr || !card->writable()) {
    snprintf(msg, msgCap, "SD not writable");
    return false;
  }
  if (len0 > 0) {
    const uint64_t freeB = card->freeBytes();
    if (freeB > 0 && (uint64_t)len0 + (256u * 1024u) > freeB) {
      snprintf(msg, msgCap, "not enough space (need %ld KB)", (long)(len0 / 1024));
      return false;
    }
  }

  const String partial = AtomicFile::partialPath(finalPath);
  SD_MMC.remove(partial);  // drop any stale partial from an earlier attempt
  fs::File f = SD_MMC.open(partial.c_str(), FILE_WRITE);
  if (!f) {
    snprintf(msg, msgCap, "cannot open download file");
    return false;
  }

  NetworkClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    f.close();
    SD_MMC.remove(partial);
    snprintf(msg, msgCap, "no data stream");
    return false;
  }
  int remaining = len0;  // >0 known, -1 unknown, hits 0 when a known length ends
  uint32_t lastFlush = 0;
  uint32_t lastDataMs = millis();
  bool failed = false;
  const char* why = "";

  while (remaining != 0 && (http.connected() || stream->available() > 0)) {
    if (!card->writable()) {
      failed = true;
      why = "card removed";
      break;
    }
    const int avail = stream->available();
    if (avail <= 0) {
      if (!http.connected()) {
        break;
      }
      if (millis() - lastDataMs > kStallMs) {
        failed = true;
        why = "download stalled";
        break;
      }
      delay(5);
      continue;
    }
    size_t toRead = (size_t)avail > kDlBufSize ? kDlBufSize : (size_t)avail;
    size_t c = stream->readBytes(s_dlBuf, toRead);
    if (c == 0) {
      if (millis() - lastDataMs > kStallMs) {
        failed = true;
        why = "download stalled";
        break;
      }
      continue;
    }
    if (f.write(s_dlBuf, c) != c) {
      failed = true;
      why = "write failed (card full?)";
      break;
    }
    self->setDownloadedBytes(self->downloadedBytes() + (uint32_t)c);
    if (remaining > 0) {
      remaining -= (int)c;
    }
    lastDataMs = millis();
    if (self->downloadedBytes() - lastFlush >= kFlushEveryBytes) {
      f.flush();
      lastFlush = self->downloadedBytes();
    }
  }

  f.flush();
  f.close();

  if (!failed && remaining > 0) {
    failed = true;
    why = "incomplete download";
  }
  if (!failed && self->downloadedBytes() == 0) {
    failed = true;
    why = "empty response";
  }
  if (failed) {
    SD_MMC.remove(partial);
    snprintf(msg, msgCap, "%s", why);
    return false;
  }
  return true;
}

}  // namespace

// Runs on its own short-lived task; touches only the staging members and flips
// fetchState_ last. RSS scan -> derive name -> skip-if-exists -> stream to
// .partial -> finalize. Never touches the UI or the main loop's state.
void podcastFetchTask(void* arg) {
  PodcastService* self = static_cast<PodcastService*>(arg);

  // Phase 1: fetch the feed and find the first audio enclosure.
  self->mp3Url_[0] = '\0';
  self->episodeTitle_[0] = '\0';
  {
    WiFiClientSecure secure;
    WiFiClient plain;
    HTTPClient http;
    bool got = false;
    if (beginRequest(http, secure, plain, self->pendingUrl_)) {
      const int code = http.GET();
      if (code == 200) {
        got = scanForEnclosure(http, self->mp3Url_, sizeof(self->mp3Url_), self->episodeTitle_,
                               sizeof(self->episodeTitle_));
      } else {
        snprintf(self->message_, sizeof(self->message_), "feed HTTP %d", code);
      }
      http.end();
    } else {
      snprintf(self->message_, sizeof(self->message_), "could not reach feed");
    }
    if (!got || self->mp3Url_[0] == '\0') {
      if (self->message_[0] == '\0') {
        snprintf(self->message_, sizeof(self->message_), "no episode found in feed");
      }
      self->fetchState_ = 4;
      vTaskDelete(nullptr);
      return;
    }
  }

  deriveDownloadPath(self->mp3Url_, self->episodeTitle_, self->finalPath_,
                     sizeof(self->finalPath_));
  const char* leaf = strrchr(self->finalPath_, '/');
  leaf = leaf != nullptr ? leaf + 1 : self->finalPath_;

  if (SD_MMC.exists(self->finalPath_)) {
    strncpy(self->lastFinalName_, leaf, sizeof(self->lastFinalName_) - 1);
    self->lastFinalName_[sizeof(self->lastFinalName_) - 1] = '\0';
    snprintf(self->message_, sizeof(self->message_), "already have %s", self->lastFinalName_);
    self->fetchState_ = 3;  // nothing to do, but a success from the user's view
    vTaskDelete(nullptr);
    return;
  }

  // Phase 2: stream the episode to <name>.partial.
  self->fetchState_ = 2;
  {
    WiFiClientSecure secure;
    WiFiClient plain;
    HTTPClient http;
    if (!beginRequest(http, secure, plain, self->mp3Url_)) {
      snprintf(self->message_, sizeof(self->message_), "could not reach episode");
      self->fetchState_ = 4;
      vTaskDelete(nullptr);
      return;
    }
    const int code = http.GET();
    if (code != 200 && code != 206) {
      snprintf(self->message_, sizeof(self->message_), "episode HTTP %d", code);
      http.end();
      self->fetchState_ = 4;
      vTaskDelete(nullptr);
      return;
    }
    char why[80];
    why[0] = '\0';
    const bool ok = downloadToPartial(http, self, self->finalPath_, why, sizeof(why));
    http.end();
    if (!ok) {
      snprintf(self->message_, sizeof(self->message_), "%s", why[0] != '\0' ? why : "download failed");
      self->fetchState_ = 4;
      vTaskDelete(nullptr);
      return;
    }
  }

  // Crash-safe rename: a clean file appears atomically, or not at all.
  if (!AtomicFile::finalizePartial(SD_MMC, self->finalPath_)) {
    SD_MMC.remove(AtomicFile::partialPath(self->finalPath_));
    snprintf(self->message_, sizeof(self->message_), "could not save file");
    self->fetchState_ = 4;
    vTaskDelete(nullptr);
    return;
  }

  strncpy(self->lastFinalName_, leaf, sizeof(self->lastFinalName_) - 1);
  self->lastFinalName_[sizeof(self->lastFinalName_) - 1] = '\0';
  snprintf(self->message_, sizeof(self->message_), "saved %s", self->lastFinalName_);
  self->fetchState_ = 3;
  vTaskDelete(nullptr);
}

void PodcastService::begin(SdStorage* storage, WifiService* wifi, EventBus* events) {
  storage_ = storage;
  wifi_ = wifi;
  events_ = events;
}

void PodcastService::update(uint32_t deltaMs) {
  (void)deltaMs;
  // Reap the task's terminal state, exactly like OpenMeteoWeatherService. Fast
  // and non-blocking: no filesystem work here (the card's free-space figure
  // self-corrects on the kernel's periodic refresh within 30 s).
  if (fetchState_ == 3) {
    fetchState_ = 0;
    lastOutcome_ = 1;
    Serial.printf("[podcast] %s\n", message_);
  } else if (fetchState_ == 4) {
    fetchState_ = 0;
    lastOutcome_ = 2;
    Serial.printf("[podcast] failed: %s\n", message_);
  }
}

PodcastService::State PodcastService::state() const {
  switch (fetchState_) {
    case 1: return State::FetchingFeed;
    case 2: return State::Downloading;
    case 3: return State::Done;
    case 4: return State::Failed;
    default:
      if (lastOutcome_ == 1) return State::Done;
      if (lastOutcome_ == 2) return State::Failed;
      return State::Idle;
  }
}

uint8_t PodcastService::percent() const {
  const uint32_t t = totalBytes_;
  const uint32_t d = downloadedBytes_;
  if (t == 0) {
    return 0;  // unknown length — the command shows raw KB instead
  }
  if (d >= t) {
    return 100;
  }
  return (uint8_t)(((uint64_t)d * 100) / t);
}

String PodcastService::feedsFilePath() const {
  return String(paths::kPodcastFeeds) + "/" + kFeedsFile;
}

size_t PodcastService::readAllFeeds(String* out) {
  if (storage_ == nullptr) {
    return 0;
  }
  SdCardAdapter* card = storage_->card();
  if (card == nullptr || !card->mounted()) {
    return 0;
  }
  fs::File f = SD_MMC.open(feedsFilePath().c_str(), FILE_READ);
  if (!f) {
    return 0;
  }
  size_t count = 0;
  while (f.available() > 0 && count < kMaxFeeds) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.charAt(0) == '#') {
      continue;
    }
    if (out != nullptr) {
      out[count] = line;
    }
    count++;
  }
  f.close();
  return count;
}

bool PodcastService::writeFeeds(String* feeds, size_t count) {
  if (storage_ == nullptr) {
    return false;
  }
  SdCardAdapter* card = storage_->card();
  if (card == nullptr || !card->writable()) {
    return false;
  }
  String body;
  for (size_t i = 0; i < count; i++) {
    body += feeds[i];
    body += '\n';
  }
  const String path = feedsFilePath();
  return AtomicFile::writeAll(SD_MMC, path.c_str(), body);
}

size_t PodcastService::listFeeds(String* out, size_t maxItems) {
  if (out == nullptr || maxItems == 0) {
    return 0;
  }
  String all[kMaxFeeds];
  const size_t count = readAllFeeds(all);
  const size_t n = count < maxItems ? count : maxItems;
  for (size_t i = 0; i < n; i++) {
    out[i] = all[i];
  }
  return n;
}

size_t PodcastService::feedCount() { return readAllFeeds(nullptr); }

bool PodcastService::addFeed(const char* url) {
  if (url == nullptr) {
    return false;
  }
  const size_t len = strlen(url);
  if (len < 8 || len >= kMaxUrlLen) {
    return false;
  }
  if (!startsWithCI(url, "http://") && !startsWithCI(url, "https://")) {
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    const unsigned char c = (unsigned char)url[i];
    if (c < 0x20 || c == 0x7F || c == ' ') {
      return false;  // no control chars or spaces in a URL
    }
  }

  String feeds[kMaxFeeds];
  size_t count = readAllFeeds(feeds);
  for (size_t i = 0; i < count; i++) {
    if (feeds[i] == url) {
      return true;  // already subscribed — idempotent
    }
  }
  if (count >= kMaxFeeds) {
    return false;
  }
  feeds[count++] = String(url);
  return writeFeeds(feeds, count);
}

bool PodcastService::removeFeed(size_t index) {
  String feeds[kMaxFeeds];
  const size_t count = readAllFeeds(feeds);
  if (index >= count) {
    return false;
  }
  for (size_t i = index; i + 1 < count; i++) {
    feeds[i] = feeds[i + 1];
  }
  return writeFeeds(feeds, count - 1);
}

bool PodcastService::fetchLatest(size_t feedIndex) {
  String feeds[kMaxFeeds];
  const size_t count = readAllFeeds(feeds);
  if (count == 0) {
    snprintf(message_, sizeof(message_), "no feeds subscribed");
    return false;
  }
  if (feedIndex >= count) {
    snprintf(message_, sizeof(message_), "no feed %u (have %u)", (unsigned)feedIndex,
             (unsigned)count);
    return false;
  }
  return kick(feeds[feedIndex].c_str());
}

bool PodcastService::fetchLatestUrl(const char* feedUrl) {
  if (feedUrl == nullptr || (!startsWithCI(feedUrl, "http://") && !startsWithCI(feedUrl, "https://"))) {
    snprintf(message_, sizeof(message_), "bad feed url");
    return false;
  }
  return kick(feedUrl);
}

bool PodcastService::kick(const char* feedUrl) {
  if (fetchState_ == 1 || fetchState_ == 2) {
    snprintf(message_, sizeof(message_), "already busy");
    return false;  // one at a time
  }
  if (feedUrl == nullptr || strlen(feedUrl) >= sizeof(pendingUrl_)) {
    snprintf(message_, sizeof(message_), "feed url too long");
    return false;
  }
  if (wifi_ == nullptr || !wifi_->internet()) {
    snprintf(message_, sizeof(message_), "no internet");
    return false;
  }
  SdCardAdapter* card = storage_ != nullptr ? storage_->card() : nullptr;
  if (card == nullptr || !card->writable()) {
    snprintf(message_, sizeof(message_), "SD card not writable");
    return false;
  }

  strncpy(pendingUrl_, feedUrl, sizeof(pendingUrl_) - 1);
  pendingUrl_[sizeof(pendingUrl_) - 1] = '\0';
  downloadedBytes_ = 0;
  totalBytes_ = 0;
  message_[0] = '\0';
  lastFinalName_[0] = '\0';

  fetchState_ = 1;
  // A generous stack: TLS handshake + the RSS scan/parse locals. If it cannot
  // start, drop back to idle so a later fetch is not wrongly seen as "busy".
  if (xTaskCreate(podcastFetchTask, "podcast", 20480, this, 1, nullptr) != pdPASS) {
    fetchState_ = 0;
    snprintf(message_, sizeof(message_), "could not start (low memory)");
    return false;
  }
  return true;
}

size_t PodcastService::listDownloads(DownloadInfo* out, size_t maxItems, size_t* totalOut) {
  if (totalOut != nullptr) {
    *totalOut = 0;
  }
  if (out == nullptr || maxItems == 0 || storage_ == nullptr) {
    return 0;
  }
  SdCardAdapter* card = storage_->card();
  if (card == nullptr || !card->mounted()) {
    return 0;
  }
  fs::File d = SD_MMC.open(paths::kPodcastDownloads);
  if (!d || !d.isDirectory()) {
    if (d) {
      d.close();
    }
    return 0;
  }
  size_t total = 0;
  size_t count = 0;
  for (fs::File e = d.openNextFile(); e; e = d.openNextFile()) {
    if (!e.isDirectory()) {
      const char* name = e.name();
      // Same acceptance as the library scan: a real audio leaf, not a hidden
      // dot-file, AppleDouble fork, or a .partial still in flight.
      if (name != nullptr && name[0] != '.') {
        const char* ext = audioExtOf(name);
        const char* dot = strrchr(name, '.');
        // Mirror the library scan (MusicService) so this listing matches what
        // the Audio -> Podcasts screen shows; .partial has no audio ext.
        const bool other = dot != nullptr &&
                           (strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".flac") == 0);
        if (ext != nullptr || other) {
          total++;
          if (count < maxItems) {
            strncpy(out[count].name, name, sizeof(out[count].name) - 1);
            out[count].name[sizeof(out[count].name) - 1] = '\0';
            out[count].sizeBytes = e.size();
            count++;
          }
        }
      }
    }
    e.close();
  }
  d.close();
  if (totalOut != nullptr) {
    *totalOut = total;
  }
  return count;
}
