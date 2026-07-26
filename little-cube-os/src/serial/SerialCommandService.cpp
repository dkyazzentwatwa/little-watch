#include "SerialCommandService.h"

#include "../board_config.h"
#include "../core/AppRouter.h"
#include "../core/EventBus.h"
#include "../core/SystemState.h"
#include "../hardware/InputAdapter.h"
#include "../hardware/SdCardAdapter.h"
#include "../services/SettingsService.h"
#include "../services/TimeService.h"
#include "../services/WifiService.h"
#include "CmdArgs.h"
#include "commands/AssistantCommands.h"
#include "commands/AudioCommands.h"
#include "commands/CalendarCommands.h"
#include "commands/ContactsCommands.h"
#include "commands/FilesCommands.h"
#include "commands/NewsCommands.h"
#include "commands/NotesCommands.h"
#include "commands/PodcastCommands.h"
#include "commands/RecordingsCommands.h"
#include "commands/SettingsCommands.h"
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
          // The truncated line is the first 255 chars of a password — wipe it
          // exactly like the accepted path does.
          wifiPrompt_.active = false;
          memset(line_, 0, sizeof(line_));
          memset(wifiPrompt_.ssid, 0, sizeof(wifiPrompt_.ssid));
          Serial.println("password entry cancelled");
        }
        if (assistantKeyPrompt_.active) {
          // Same rule for a truncated API key: wipe, never keep a fragment.
          assistantKeyPrompt_.active = false;
          memset(line_, 0, sizeof(line_));
          Serial.println("key entry cancelled");
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
      } else if (assistantKeyPrompt_.active) {
        // This line is the OpenAI API key: store it in NVS, then destroy the
        // only plaintext copy. It is never echoed or logged.
        assistantKeyPrompt_.active = false;
        if (line_[0] == '\0') {
          Serial.println("key entry cancelled");
        } else if (services_->settings != nullptr) {
          services_->settings->setOpenaiKey(String(line_));
          Serial.println("key stored (NVS)");
        }
        memset(line_, 0, sizeof(line_));
      } else if (multiline_.active()) {
        switch (multiline_.feedLine(line_)) {
          case MultilineBuffer::Result::Saved:
            Serial.printf("Saved: %s (%u B)\n", multiline_.targetPath(),
                          (unsigned)multiline_.bytes());
            break;
          case MultilineBuffer::Result::Cancelled:
            Serial.println("Discarded.");
            break;
          case MultilineBuffer::Result::Error: {
            // The buffer prints the specific cause; add where the text went,
            // so a failed save is never silent and never loses the content.
            const char* recovery = multiline_.recoveryPath();
            if (recovery != nullptr) {
              // The next capture reuses (and clears) that temp path.
              Serial.printf("Not saved. Your text is still in %s — copy it out before the"
                            " next write.\n",
                            recovery);
            } else {
              Serial.println("Not saved.");
            }
            break;
          }
          case MultilineBuffer::Result::Collecting:
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
    if (strcmp(topic, "wifi") == 0) {
      printWifiHelp();
      return;
    }
    if (strcmp(topic, "recordings") == 0) {
      printRecordingsHelp();
      return;
    }
    if (strcmp(topic, "assistant") == 0) {
      printAssistantHelp();
      return;
    }
    if (strcmp(topic, "audio") == 0) {
      printAudioHelp();
      return;
    }
    if (strcmp(topic, "calendar") == 0) {
      printCalendarHelp();
      return;
    }
    if (strcmp(topic, "contacts") == 0) {
      printContactsHelp();
      return;
    }
    if (strcmp(topic, "settings") == 0) {
      printSettingsHelp();
      return;
    }
    if (strcmp(topic, "news") == 0) {
      printNewsHelp();
      return;
    }
    if (strcmp(topic, "podcast") == 0) {
      printPodcastHelp();
      return;
    }
    Serial.printf("no detailed help for '%s' yet\n", topic);
    return;
  }
  Serial.printf("%s %s — USB serial commands\n", FIRMWARE_NAME, FIRMWARE_VERSION);
  Serial.println("");
  Serial.println("HOW:  type   <group> <command> [text]   then press Enter");
  Serial.println("      e.g.   status        wifi scan       notes list      audio play 1");
  Serial.println("MORE: type   help <group>                 e.g.   help audio");
  Serial.println("");
  Serial.println("  SYSTEM     status · version · uptime · reboot · open <app> · back");
  Serial.println("  TIME       time · time set YYYY-MM-DD HH:MM");
  Serial.println("  WEATHER    set it:  settings set weather.city <name>   (needs Wi-Fi)");
  Serial.println("  NEWS       list · show <n> · refresh   (BBC News)");
  Serial.println("  PODCAST    feeds · add <url> · fetch · status · list");
  Serial.println("  NOTES      list · show · new · write · append · rename · pin · delete");
  Serial.println("  FILES      list · tree · cat · mkdir · copy · move · rename · delete");
  Serial.println("  STORAGE    status · mount · eject · usage");
  Serial.println("  WI-FI      scan · connect · status · disconnect · forget · offline");
  Serial.println("  AUDIO      list · play <n> · pause · resume · stop · next · previous");
  Serial.println("  RECORDINGS list · start · stop · gain · normalize · gate · delete");
  Serial.println("  ASSISTANT  status · key · ask <q> · voice · transcribe · reset");
  Serial.println("  CALENDAR   list · show · next · add · done · delete");
  Serial.println("  CONTACTS   list · search · show · add · set · favorite · delete");
  Serial.println("  SETTINGS   list · get <key> · set <key> <value> · bedtime");
  Serial.println("  SCREEN     input debug <on|off>");
  Serial.println("");
  Serial.println("Volume: 'audio volume <0-100>'.  Detailed help + examples: help <group>.");
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
  if (strcmp(family, "input") == 0 && services_->input != nullptr) {
    const char* verb = cmdargs::nextToken(cursor);
    const char* value = cmdargs::nextToken(cursor);
    if (verb != nullptr && strcmp(verb, "debug") == 0 && value != nullptr) {
      services_->input->setDebugLog(strcmp(value, "on") == 0);
      Serial.printf("input debug %s\n", services_->input->debugLog() ? "on" : "off");
      return;
    }
    if (verb != nullptr && strcmp(verb, "status") == 0) {
      // INT edges prove the FT3168 interrupt wiring; polling stays I2C-silent
      // while idle only once an edge has been seen (tap the glass to confirm).
      Serial.printf("touch: %s, INT edges: %lu (%s), debug %s\n",
                    services_->input->touchReady() ? "ready" : "unavailable",
                    (unsigned long)services_->input->touchIntEdges(),
                    services_->input->touchIntSeen() ? "INT-gated polling"
                                                     : "fallback timed polling",
                    services_->input->debugLog() ? "on" : "off");
      return;
    }
    Serial.println("usage: input debug <on|off> | input status");
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

  if (strcmp(family, "recordings") == 0) {
    if (verb == nullptr) {
      printRecordingsHelp();
    } else if (!handleRecordingsCommand(*services_, verb, cursor)) {
      Serial.printf("error: unknown command 'recordings %s'\n", verb);
    }
    return;
  }
  if (strcmp(family, "assistant") == 0) {
    if (verb == nullptr) {
      printAssistantHelp();
    } else if (!handleAssistantCommand(*services_, assistantKeyPrompt_, verb, cursor)) {
      Serial.printf("error: unknown command 'assistant %s'\n", verb);
    }
    return;
  }
  if (strcmp(family, "audio") == 0) {
    if (verb == nullptr) {
      printAudioHelp();
    } else if (!handleAudioCommand(*services_, verb, cursor)) {
      Serial.printf("error: unknown command 'audio %s'\n", verb);
    }
    return;
  }
  if (strcmp(family, "volume") == 0) {
    // `volume <0-100>` — the verb IS the value here.
    char volLine[16];
    snprintf(volLine, sizeof(volLine), "%s", verb != nullptr ? verb : "");
    char* volCursor = volLine;
    handleVolumeCommand(*services_, volCursor);
    return;
  }

  if (strcmp(family, "calendar") == 0) {
    if (verb == nullptr) {
      printCalendarHelp();
    } else if (!handleCalendarCommand(*services_, verb, cursor)) {
      Serial.printf("error: unknown command 'calendar %s' — try 'help calendar'\n", verb);
    }
    return;
  }
  if (strcmp(family, "contacts") == 0) {
    if (verb == nullptr) {
      printContactsHelp();
    } else if (!handleContactsCommand(*services_, verb, cursor)) {
      Serial.printf("error: unknown command 'contacts %s' — try 'help contacts'\n", verb);
    }
    return;
  }
  if (strcmp(family, "settings") == 0) {
    if (verb == nullptr) {
      printSettingsHelp();
    } else if (!handleSettingsCommand(*services_, verb, cursor)) {
      Serial.printf("error: unknown command 'settings %s' — try 'help settings'\n", verb);
    }
    return;
  }
  if (strcmp(family, "news") == 0) {
    if (verb == nullptr) {
      printNewsHelp();
    } else if (!handleNewsCommand(*services_, verb, cursor)) {
      Serial.printf("error: unknown command 'news %s' — try 'help news'\n", verb);
    }
    return;
  }
  if (strcmp(family, "podcast") == 0) {
    if (verb == nullptr) {
      printPodcastHelp();
    } else if (!handlePodcastCommand(*services_, verb, cursor)) {
      Serial.printf("error: unknown command 'podcast %s' — try 'help podcast'\n", verb);
    }
    return;
  }

  Serial.printf("error: unknown command '%s' — try 'help'\n", family);
}
