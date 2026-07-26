#include "RecordingsCommands.h"

#include <Arduino.h>

#include "../../board_config.h"
#include "../../core/Services.h"
#include "../../hardware/audio/AudioAdapter.h"
#include "../../services/RecorderService.h"
#include "../../services/SettingsService.h"
#include "../../storage/SdStorage.h"
#include "../../storage/StoragePaths.h"
#include "../CmdArgs.h"

namespace {

constexpr size_t kMaxListed = 24;
RecordingInfo s_list[kMaxListed];
size_t s_listCount = 0;
bool s_listValid = false;

void refreshList(Services& services) {
  s_listCount = services.recorder->list(s_list, kMaxListed);
  s_listValid = true;
}

bool resolveRecording(Services& services, const char* token, String& outPath) {
  if (token == nullptr) {
    return false;
  }
  if (strchr(token, '/') != nullptr) {
    return services.storage->sanitizePath(token, outPath);
  }
  if (!s_listValid) {
    refreshList(services);
  }
  const size_t id = strtoul(token, nullptr, 10);
  if (id < 1 || id > s_listCount) {
    Serial.printf("error: no recording %s (run 'recordings list')\n", token);
    return false;
  }
  outPath = s_list[id - 1].path;
  return true;
}

}  // namespace

void printRecordingsHelp() {
  Serial.println("recordings list               list voice notes (ids below)");
  Serial.println("recordings start / stop       run a take from serial");
  Serial.println("recordings quiet [on|off]     pause PMU/RTC I2C during takes");
  Serial.println("recordings gain [0-7]         mic ADC gain, 6 dB steps (0..42 dB)");
  Serial.println("recordings normalize [on|off] lift takes to -3 dBFS before saving");
  Serial.println("recordings gate [on|off]      ease pauses -12 dB (hiss relief)");
  Serial.println("recordings info <id>          size + estimated duration");
  Serial.println("recordings rename <id> \"<name>\"");
  Serial.println("recordings delete <id> confirm");
}

bool handleRecordingsCommand(Services& services, const char* verb, char* args) {
  if (services.recorder == nullptr || services.storage == nullptr) {
    return false;
  }

  if (strcmp(verb, "list") == 0) {
    refreshList(services);
    if (s_listCount == 0) {
      Serial.println("no recordings");
      return true;
    }
    for (size_t i = 0; i < s_listCount; i++) {
      const uint32_t seconds =
          s_list[i].sizeBytes > 44
              ? (uint32_t)((s_list[i].sizeBytes - 44) / (AUDIO_SAMPLE_RATE * 2))
              : 0;
      Serial.printf("%2u. %-32s %6u KB  ~%lus\n", (unsigned)(i + 1), s_list[i].name,
                    (unsigned)(s_list[i].sizeBytes / 1024), (unsigned long)seconds);
    }
    return true;
  }

  if (strcmp(verb, "start") == 0) {
    if (services.recorder->recording()) {
      Serial.println("already recording");
      return true;
    }
    // Failure reasons (SD state, space, audio busy) are printed by the service.
    if (services.recorder->start()) {
      Serial.println("ok — stop with: recordings stop (or tap STOP)");
    }
    s_listValid = false;
    return true;
  }

  if (strcmp(verb, "stop") == 0) {
    if (!services.recorder->recording()) {
      Serial.println("not recording");
      return true;
    }
    services.recorder->stop();
    Serial.println("stopping (file finalizes over the next frames)");
    s_listValid = false;
    return true;
  }

  if (strcmp(verb, "quiet") == 0) {
    char* cursor = args;
    const char* arg = cmdargs::nextToken(cursor);
    if (arg != nullptr && strcmp(arg, "on") == 0) {
      services.recorder->setQuietCapture(true);
    } else if (arg != nullptr && strcmp(arg, "off") == 0) {
      services.recorder->setQuietCapture(false);
    } else if (arg != nullptr) {
      Serial.println("usage: recordings quiet [on|off]");
      return true;
    }
    Serial.printf("quiet capture: %s (pause PMU/RTC I2C during takes)\n",
                  services.recorder->quietCapture() ? "on" : "off");
    return true;
  }

  if (strcmp(verb, "gain") == 0) {
    if (services.audio == nullptr) {
      return false;
    }
    char* cursor = args;
    const char* arg = cmdargs::nextToken(cursor);
    if (arg != nullptr) {
      if (services.recorder->recording()) {
        Serial.println("error: not while recording (codec is live)");
        return true;
      }
      const long v = strtol(arg, nullptr, 10);
      if (v < 0 || v > 7 || (v == 0 && strcmp(arg, "0") != 0)) {
        Serial.println("usage: recordings gain [0-7]   (6 dB steps: 0 = 0 dB .. 7 = 42 dB)");
        return true;
      }
      services.audio->setMicGain(static_cast<uint8_t>(v));
      // Persist too, or serial and the Sound screen drift apart on reboot.
      if (services.settings != nullptr) {
        services.settings->setMicGain(static_cast<uint8_t>(v));
      }
    }
    Serial.printf("mic ADC gain: %u (%u dB)\n", (unsigned)services.audio->micGain(),
                  (unsigned)(services.audio->micGain() * 6u));
    return true;
  }

  if (strcmp(verb, "normalize") == 0 || strcmp(verb, "gate") == 0) {
    if (services.audio == nullptr) {
      return false;
    }
    const bool isGate = strcmp(verb, "gate") == 0;
    char* cursor = args;
    const char* arg = cmdargs::nextToken(cursor);
    if (arg != nullptr) {
      const bool on = strcmp(arg, "on") == 0;
      if (!on && strcmp(arg, "off") != 0) {
        Serial.printf("usage: recordings %s [on|off]\n", verb);
        return true;
      }
      if (isGate) {
        services.audio->setRecordGate(on);
        if (services.settings != nullptr) {
          services.settings->setRecordGate(on);
        }
      } else {
        services.audio->setRecordNormalize(on);
        if (services.settings != nullptr) {
          services.settings->setRecordNormalize(on);
        }
      }
    }
    if (isGate) {
      Serial.printf("noise gate: %s (ease pauses -12 dB below the take's floor)\n",
                    services.audio->recordGate() ? "on" : "off");
    } else {
      Serial.printf("normalize: %s (lift takes to -3 dBFS before saving)\n",
                    services.audio->recordNormalize() ? "on" : "off");
    }
    return true;
  }

  if (strcmp(verb, "info") == 0) {
    char* cursor = args;
    String path;
    if (!resolveRecording(services, cmdargs::nextToken(cursor), path)) {
      Serial.println("usage: recordings info <id>");
      return true;
    }
    Serial.printf("path: %s\n", path.c_str());
    Serial.printf("format: WAV mono 16-bit @ %u Hz\n", (unsigned)AUDIO_SAMPLE_RATE);
    return true;
  }

  if (strcmp(verb, "rename") == 0) {
    char* cursor = args;
    const char* token = cmdargs::nextToken(cursor);
    const char* name = cmdargs::rest(cursor);
    String path;
    if (token == nullptr || name == nullptr || strchr(name, '/') != nullptr) {
      Serial.println("usage: recordings rename <id> \"<new name>\"");
      return true;
    }
    if (!resolveRecording(services, token, path)) {
      return true;
    }
    String dst = String(paths::kRecordings) + "/" + name;
    if (!dst.endsWith(".wav")) {
      dst += ".wav";
    }
    Serial.println(services.storage->renamePath(path.c_str(), dst.c_str())
                       ? "renamed"
                       : "error: rename failed");
    s_listValid = false;
    return true;
  }

  if (strcmp(verb, "delete") == 0) {
    char* cursor = args;
    const char* token = cmdargs::nextToken(cursor);
    const char* confirm = cmdargs::nextToken(cursor);
    String path;
    if (!resolveRecording(services, token, path)) {
      Serial.println("usage: recordings delete <id> confirm");
      return true;
    }
    if (confirm == nullptr || strcmp(confirm, "confirm") != 0) {
      Serial.printf("this deletes %s — run: recordings delete %s confirm\n", path.c_str(),
                    token);
      return true;
    }
    Serial.println(services.recorder->remove(path.c_str()) ? "deleted" : "error: delete failed");
    s_listValid = false;
    return true;
  }

  return false;
}
