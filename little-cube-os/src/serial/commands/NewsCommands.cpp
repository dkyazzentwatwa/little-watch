#include "NewsCommands.h"

#include <Arduino.h>

#include "../../core/Services.h"
#include "../../services/NewsService.h"
#include "../CmdArgs.h"

void printNewsHelp() {
  Serial.println("news list            list fetched/cached headlines (ids for 'show')");
  Serial.println("news show <n>        print one headline: title, summary, link");
  Serial.println("news refresh         fetch the latest headlines (needs Wi-Fi)");
}

bool handleNewsCommand(Services& services, const char* verb, char* args) {
  NewsService* news = services.news;
  if (news == nullptr) {
    Serial.println("error: news service unavailable");
    return true;
  }

  if (strcmp(verb, "list") == 0) {
    const size_t n = news->count();
    if (n == 0) {
      Serial.println(news->fetching() ? "fetching..." : "no headlines yet (try 'news refresh')");
      return true;
    }
    char fresh[48];
    news->freshness(fresh, sizeof(fresh));
    Serial.printf("headlines (%u) - %s\n", static_cast<unsigned>(n), fresh);
    for (size_t i = 0; i < n; i++) {
      const Headline& h = news->headline(i);
      Serial.printf("%2u. %s\n", static_cast<unsigned>(i + 1), h.title);
    }
    return true;
  }

  if (strcmp(verb, "show") == 0) {
    char* cursor = args;
    const char* token = cmdargs::nextToken(cursor);
    if (token == nullptr) {
      Serial.println("usage: news show <n>  (n from 'news list')");
      return true;
    }
    const size_t n = news->count();
    const unsigned long id = strtoul(token, nullptr, 10);
    if (id < 1 || id > n) {
      Serial.printf("error: no headline %s (run 'news list')\n", token);
      return true;
    }
    const Headline& h = news->headline(static_cast<size_t>(id - 1));
    Serial.printf("--- headline %lu of %u ---\n", id, static_cast<unsigned>(n));
    Serial.printf("title:   %s\n", h.title);
    Serial.printf("summary: %s\n", h.summary[0] != '\0' ? h.summary : "(none)");
    Serial.printf("link:    %s\n", h.url[0] != '\0' ? h.url : "(no article link)");
    Serial.println("--- end ---");
    return true;
  }

  if (strcmp(verb, "refresh") == 0) {
    if (news->refresh()) {
      Serial.println("fetching latest headlines...");
    } else {
      Serial.println(news->fetching() ? "already fetching"
                                       : "error: offline (connect Wi-Fi first)");
    }
    return true;
  }

  return false;
}
