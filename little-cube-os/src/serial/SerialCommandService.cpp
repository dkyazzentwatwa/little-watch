#include "SerialCommandService.h"

#include "../board_config.h"
#include "../core/AppRouter.h"
#include "../core/EventBus.h"
#include "../core/SystemState.h"
#include "../hardware/SdCardAdapter.h"
#include "../services/TimeService.h"
#include "../services/WifiService.h"
#include "CmdArgs.h"
#include "commands/FilesCommands.h"
#include "commands/NotesCommands.h"
#include "commands/StorageCommands.h"

void SerialCommandService::begin(Services* services) {
  services_ = services;
  lineLen_ = 0;
  overflowed_ = false;
}

void SerialCommandService::update() {
  while (Serial.available() > 0) {
    const int raw = Serial.read();
    if (raw < 0) {
      break;
    }
    const char c = static_cast<char>(raw);
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      line_[lineLen_] = '\0';
      if (overflowed_) {
        Serial.println("error: line too long (max 255 chars); ignored");
        if (wifiPrompt_.active) {
          wifiPrompt_.active = false;
          Serial.println("password entry cancelled");
        }
      } else if (wifiPrompt_.active) {
        // This line is a Wi-Fi password: hand it straight to the service,
        // then destroy every copy. It is never echoed or logged.
        wifiPrompt_.active = false;
        Serial.println("Connecting...");
        if (services_->wifi != nullptr) {
          services_->wifi->connectTo(wifiPrompt_.ssid, line_, wifiPrompt_.hidden);
        }
        memset(line_, 0, sizeof(line_));
        memset(wifiPrompt_.ssid, 0, sizeof(wifiPrompt_.ssid));
      } else if (multiline_.active()) {
        switch (multiline_.feedLine(line_)) {
          case MultilineBuffer::Result::Saved:
            Serial.printf("Saved: %s (%u B)\n", multiline_.targetPath(),
                          (unsigned)multiline_.bytes());
            break;
          case MultilineBuffer::Result::Cancelled:
            Serial.println("Discarded.");
            break;
          case MultilineBuffer::Result::Collecting:
          case MultilineBuffer::Result::Error:
            break;
        }
      } else if (lineLen_ > 0) {
        handleLine(line_);
      }
      lineLen_ = 0;
      overflowed_ = false;
      continue;
    }
    if (lineLen_ >= kMaxLineLen - 1) {
      overflowed_ = true;
      continue;
    }
    line_[lineLen_++] = c;
  }
}

void SerialCommandService::printHelp(const char* topic) {
  if (topic != nullptr) {
    if (strcmp(topic, "notes") == 0) {
      printNotesHelp();
      return;
    }
    if (strcmp(topic, "files") == 0) {
      printFilesHelp();
      return;
    }
    if (strcmp(topic, "storage") == 0) {
      printStorageHelp();
      return;
    }
    Serial.printf("no detailed help for '%s' yet\n", topic);
    return;
  }
  Serial.printf("%s %s — serial interface\n\n", FIRMWARE_NAME, FIRMWARE_VERSION);
  Serial.println("general:  help [topic] · status · version · uptime · reboot");
  Serial.println("          open <app> · back");
  Serial.println("notes:    list · show · new · write · append · rename · favorite ·");
  Serial.println("          pin · delete · import · export        (help notes)");
  Serial.println("files:    list · tree · cat · mkdir · copy · move · rename · delete");
  Serial.println("          (help files)");
  Serial.println("storage:  status · mount · eject · usage · index (help storage)");
  Serial.println("wifi / recordings / audio / calendar / contacts / settings:");
  Serial.println("          arriving in later milestones");
}

void SerialCommandService::printStatus() {
  const SystemState& s = *services_->state;
  Serial.printf("%s %s\n", FIRMWARE_NAME, FIRMWARE_VERSION);
  const uint32_t upSec = millis() / 1000;
  Serial.printf("uptime: %lum %lus\n", (unsigned long)(upSec / 60), (unsigned long)(upSec % 60));
  Serial.printf("heap: %u KB free · psram: %u KB free\n", (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(ESP.getFreePsram() / 1024));
  Serial.printf("wifi: %s\n", wifiStateName(s.wifi));
  Serial.printf("sd: %s", sdCardStateName(s.sd));
  if (services_->sdCard->mounted()) {
    Serial.printf(" (%llu MB free)", (unsigned long long)(services_->sdCard->freeBytes() /
                                                          (1024 * 1024)));
  }
  Serial.println();
  if (s.batteryPresent) {
    Serial.printf("battery: %d%%%s\n", s.batteryPercent, s.charging ? " charging" : "");
  }
  Serial.printf("time: %s%s\n", s.clockHhMm, s.timeValid ? "" : " (not set)");
}

void SerialCommandService::handleLine(char* line) {
  if (services_ != nullptr && services_->events != nullptr) {
    services_->events->publish(SystemEvent::SerialCommandReceived);
  }

  char* cursor = line;
  const char* family = cmdargs::nextToken(cursor);
  if (family == nullptr) {
    return;
  }

  // --- general -----------------------------------------------------------
  if (strcmp(family, "help") == 0) {
    printHelp(cmdargs::nextToken(cursor));
    return;
  }
  if (strcmp(family, "version") == 0) {
    Serial.printf("%s %s\n", FIRMWARE_NAME, FIRMWARE_VERSION);
    return;
  }
  if (strcmp(family, "status") == 0) {
    printStatus();
    return;
  }
  if (strcmp(family, "uptime") == 0) {
    Serial.printf("%lu s\n", (unsigned long)(millis() / 1000));
    return;
  }
  if (strcmp(family, "reboot") == 0) {
    Serial.println("rebooting...");
    Serial.flush();
    delay(100);
    ESP.restart();
    return;
  }
  if (strcmp(family, "open") == 0 && services_->router != nullptr) {
    const char* name = cmdargs::rest(cursor);
    if (name != nullptr) {
      for (uint8_t i = 0; i < kAppCount; i++) {
        const AppId id = static_cast<AppId>(i);
        if (strcasecmp(name, appName(id)) == 0) {
          services_->router->open(id);
          Serial.printf("opened %s\n", appName(id));
          return;
        }
      }
    }
    Serial.println("error: unknown app (Home, Today, Clock, Weather, Calendar, Notes, Recorder, "
                   "Audio, Files, Contacts, Calculator, Settings)");
    return;
  }
  if (strcmp(family, "back") == 0 && services_->router != nullptr) {
    services_->router->back();
    Serial.printf("now: %s\n", appName(services_->router->currentId()));
    return;
  }
  if (strcmp(family, "time") == 0 && services_->time != nullptr) {
    const char* verb = cmdargs::nextToken(cursor);
    if (verb == nullptr) {
      struct tm t;
      if (services_->time->now(t)) {
        Serial.printf("%04d-%02d-%02d %02d:%02d:%02d%s\n", t.tm_year + 1900, t.tm_mon + 1,
                      t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
                      services_->time->valid() ? "" : " (not set)");
      } else {
        Serial.println("RTC unavailable");
      }
      return;
    }
    if (strcmp(verb, "set") == 0) {
      const char* date = cmdargs::nextToken(cursor);
      const char* clock = cmdargs::nextToken(cursor);
      struct tm t = {};
      int sec = 0;
      if (date == nullptr || clock == nullptr ||
          sscanf(date, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3 ||
          sscanf(clock, "%d:%d:%d", &t.tm_hour, &t.tm_min, &sec) < 2) {
        Serial.println("usage: time set YYYY-MM-DD HH:MM[:SS]");
        return;
      }
      t.tm_year -= 1900;
      t.tm_mon -= 1;
      t.tm_sec = sec;
      mktime(&t);  // normalizes and fills tm_wday
      Serial.println(services_->time->setManual(t) ? "time set" : "error: RTC write failed");
      return;
    }
    Serial.println("usage: time · time set YYYY-MM-DD HH:MM[:SS]");
    return;
  }

  // --- families ----------------------------------------------------------
  const char* verb = cmdargs::nextToken(cursor);

  if (strcmp(family, "notes") == 0) {
    if (verb == nullptr) {
      printNotesHelp();
    } else if (!handleNotesCommand(*services_, multiline_, verb, cursor)) {
      Serial.printf("error: unknown command 'notes %s' — try 'help notes'\n", verb);
    }
    return;
  }
  if (strcmp(family, "files") == 0) {
    if (verb == nullptr) {
      printFilesHelp();
    } else if (!handleFilesCommand(*services_, verb, cursor)) {
      Serial.printf("error: unknown command 'files %s' — try 'help files'\n", verb);
    }
    return;
  }
  if (strcmp(family, "storage") == 0) {
    if (verb == nullptr) {
      printStorageHelp();
    } else if (!handleStorageCommand(*services_, verb, cursor)) {
      Serial.printf("error: unknown command 'storage %s' — try 'help storage'\n", verb);
    }
    return;
  }

  if (strcmp(family, "wifi") == 0) {
    if (verb == nullptr) {
      printWifiHelp();
    } else if (!handleWifiCommand(*services_, wifiPrompt_, verb, cursor)) {
      Serial.printf("error: unknown command 'wifi %s' — try 'help wifi'\n", verb);
    }
    return;
  }

  if (strcmp(family, "recordings") == 0 ||
      strcmp(family, "audio") == 0 || strcmp(family, "volume") == 0 ||
      strcmp(family, "calendar") == 0 || strcmp(family, "contacts") == 0 ||
      strcmp(family, "settings") == 0) {
    Serial.printf("'%s' commands arrive in a later milestone — 'help' shows what works today\n",
                  family);
    return;
  }

  Serial.printf("error: unknown command '%s' — try 'help'\n", family);
}
