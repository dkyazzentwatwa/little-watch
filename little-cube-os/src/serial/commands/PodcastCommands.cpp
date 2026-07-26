#include "PodcastCommands.h"

#include <Arduino.h>
#include <strings.h>  // strncasecmp

#include "../../core/Services.h"
#include "../../services/PodcastService.h"
#include "../CmdArgs.h"

namespace {

// Feed ids are 0-based here (matching `fetch` defaulting to feed 0), unlike the
// 1-based notes/recordings lists — kept consistent across feeds/fetch/remove.
void printFeeds(PodcastService& pod) {
  String feeds[PodcastService::kMaxFeeds];
  const size_t n = pod.listFeeds(feeds, PodcastService::kMaxFeeds);
  if (n == 0) {
    Serial.println("no feeds — subscribe with: podcast add <rss-url>");
    return;
  }
  for (size_t i = 0; i < n; i++) {
    Serial.printf("%2u. %s\n", (unsigned)i, feeds[i].c_str());
  }
}

void printStatus(PodcastService& pod) {
  switch (pod.state()) {
    case PodcastService::State::Idle:
      Serial.println("idle");
      break;
    case PodcastService::State::FetchingFeed:
      Serial.println("fetching feed...");
      break;
    case PodcastService::State::Downloading: {
      const uint32_t d = pod.downloadedBytes();
      const uint32_t t = pod.totalBytes();
      if (t > 0) {
        Serial.printf("downloading %u%% (%lu / %lu KB)\n", pod.percent(),
                      (unsigned long)(d / 1024), (unsigned long)(t / 1024));
      } else {
        Serial.printf("downloading %lu KB\n", (unsigned long)(d / 1024));
      }
      break;
    }
    case PodcastService::State::Done:
      Serial.printf("done: %s\n", pod.message());
      break;
    case PodcastService::State::Failed:
      Serial.printf("failed: %s\n", pod.message());
      break;
  }
}

}  // namespace

void printPodcastHelp() {
  Serial.println("podcast feeds                 list subscribed feeds (ids below)");
  Serial.println("podcast add <url>             subscribe to an RSS feed");
  Serial.println("podcast remove <id>           unsubscribe");
  Serial.println("podcast fetch [id|url]        download latest episode (default feed 0)");
  Serial.println("podcast status                current download status");
  Serial.println("podcast list                  downloaded episodes on the card");
}

bool handlePodcastCommand(Services& services, const char* verb, char* args) {
  if (services.podcasts == nullptr) {
    return false;
  }
  PodcastService& pod = *services.podcasts;

  if (strcmp(verb, "feeds") == 0) {
    printFeeds(pod);
    return true;
  }

  if (strcmp(verb, "add") == 0) {
    char* cursor = args;
    const char* url = cmdargs::rest(cursor);
    if (url == nullptr) {
      Serial.println("usage: podcast add <rss-url>");
      return true;
    }
    if (pod.addFeed(url)) {
      Serial.println("subscribed");
    } else {
      Serial.println("error: could not add (need http(s) url, feed list full, or SD not writable)");
    }
    return true;
  }

  if (strcmp(verb, "remove") == 0) {
    char* cursor = args;
    const char* token = cmdargs::nextToken(cursor);
    if (token == nullptr) {
      Serial.println("usage: podcast remove <id>   (see 'podcast feeds')");
      return true;
    }
    const size_t index = strtoul(token, nullptr, 10);
    Serial.println(pod.removeFeed(index) ? "removed" : "error: no such feed (podcast feeds)");
    return true;
  }

  if (strcmp(verb, "fetch") == 0) {
    if (pod.busy()) {
      Serial.println("busy: a download is already running (podcast status)");
      return true;
    }
    char* cursor = args;
    const char* token = cmdargs::nextToken(cursor);
    bool started = false;
    if (token != nullptr &&
        (strncasecmp(token, "http://", 7) == 0 || strncasecmp(token, "https://", 8) == 0)) {
      started = pod.fetchLatestUrl(token);
      if (started) {
        Serial.println("fetching latest episode...");
      }
    } else {
      const size_t index = token != nullptr ? strtoul(token, nullptr, 10) : 0;
      started = pod.fetchLatest(index);
      if (started) {
        Serial.printf("fetching latest of feed %u... (poll 'podcast status')\n", (unsigned)index);
      }
    }
    if (!started) {
      Serial.printf("error: %s\n", pod.message());
    }
    return true;
  }

  if (strcmp(verb, "status") == 0) {
    printStatus(pod);
    return true;
  }

  if (strcmp(verb, "list") == 0) {
    PodcastService::DownloadInfo items[24];
    size_t total = 0;
    const size_t n = pod.listDownloads(items, 24, &total);
    if (n == 0) {
      Serial.println("no downloaded episodes");
      return true;
    }
    for (size_t i = 0; i < n; i++) {
      Serial.printf("%2u. %-44s %6lu KB\n", (unsigned)(i + 1), items[i].name,
                    (unsigned long)(items[i].sizeBytes / 1024));
    }
    if (total > n) {
      Serial.printf("... +%u more\n", (unsigned)(total - n));
    }
    return true;
  }

  return false;
}
