#include "AudioCommands.h"

#include <Arduino.h>

#include "../../core/Services.h"
#include "../../hardware/audio/AudioAdapter.h"
#include "../../services/RecorderService.h"
#include "../../services/SettingsService.h"
#include "../../storage/SdStorage.h"
#include "../CmdArgs.h"

namespace {

constexpr size_t kMaxListed = 24;
RecordingInfo s_list[kMaxListed];
size_t s_listCount = 0;
int s_currentIndex = -1;

void refreshList(Services& services) {
  s_listCount = services.recorder->list(s_list, kMaxListed);
}

bool playIndex(Services& services, int index) {
  if (index < 0 || index >= static_cast<int>(s_listCount)) {
    return false;
  }
  // Stops whatever is playing and starts this file once the old task has been
  // reaped — without blocking the loop waiting for it.
  if (services.audio->requestPlayWavFile(s_list[index].path)) {
    s_currentIndex = index;
    Serial.printf("playing %s\n", s_list[index].name);
    return true;
  }
  Serial.println("error: playback failed (recording active? file gone?)");
  return false;
}

}  // namespace

void printAudioHelp() {
  Serial.println("audio list                    playable files (WAV recordings in v1)");
  Serial.println("audio play <id|path>          play a WAV");
  Serial.println("audio pause / resume / stop");
  Serial.println("audio next / previous");
  Serial.println("audio sleep <minutes|off>     stop playback after N minutes");
  Serial.println("volume <0-100>                speaker volume");
}

bool handleVolumeCommand(Services& services, char* args) {
  char* cursor = args;
  const char* value = cmdargs::nextToken(cursor);
  if (value == nullptr) {
    Serial.printf("volume: %u%%\n",
                  (unsigned)(services.settings != nullptr ? services.settings->volumePercent()
                                                          : 0));
    return true;
  }
  const long v = strtol(value, nullptr, 10);
  if (v < 0 || v > 100) {
    Serial.println("usage: volume <0-100>");
    return true;
  }
  if (services.settings != nullptr) {
    services.settings->setVolumePercent(static_cast<uint8_t>(v));
  }
  if (services.audio != nullptr) {
    services.audio->setVolumePercent(static_cast<uint8_t>(v));
  }
  Serial.printf("volume: %ld%%\n", v);
  return true;
}

bool handleAudioCommand(Services& services, const char* verb, char* args) {
  AudioAdapter* audio = services.audio;
  if (audio == nullptr || services.recorder == nullptr) {
    return false;
  }

  if (strcmp(verb, "list") == 0) {
    refreshList(services);
    if (s_listCount == 0) {
      Serial.println("nothing playable yet (music/podcasts/radio arrive in a later phase)");
      return true;
    }
    for (size_t i = 0; i < s_listCount; i++) {
      Serial.printf("%2u. %s%s\n", (unsigned)(i + 1), s_list[i].name,
                    s_currentIndex == static_cast<int>(i) && audio->isPlaying() ? "  <- playing"
                                                                                : "");
    }
    return true;
  }

  if (strcmp(verb, "play") == 0) {
    char* cursor = args;
    const char* token = cmdargs::nextToken(cursor);
    if (token == nullptr) {
      Serial.println("usage: audio play <id|path>");
      return true;
    }
    if (strchr(token, '/') != nullptr) {
      String path;
      if (!services.storage->sanitizePath(token, path)) {
        Serial.println("error: invalid path");
        return true;
      }
      Serial.println(audio->requestPlayWavFile(path.c_str()) ? "playing"
                                                             : "error: playback failed");
      return true;
    }
    if (s_listCount == 0) {
      refreshList(services);
    }
    playIndex(services, static_cast<int>(strtoul(token, nullptr, 10)) - 1);
    return true;
  }

  if (strcmp(verb, "pause") == 0) {
    audio->pausePlayback(true);
    Serial.println("paused");
    return true;
  }
  if (strcmp(verb, "resume") == 0) {
    audio->pausePlayback(false);
    Serial.println("resumed");
    return true;
  }
  if (strcmp(verb, "stop") == 0) {
    audio->stopPlayback();
    Serial.println("stopped");
    return true;
  }
  if (strcmp(verb, "sleep") == 0) {
    char* cursor = args;
    const char* arg = cmdargs::rest(cursor);
    if (arg == nullptr) {
      const uint32_t sec = audio->sleepRemainingSec();
      if (sec > 0) {
        Serial.printf("sleep timer: %lum %lus left\n", (unsigned long)(sec / 60),
                      (unsigned long)(sec % 60));
      } else {
        Serial.println("sleep timer: off  (usage: audio sleep <minutes|off>)");
      }
      return true;
    }
    if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
      audio->setSleepTimerMinutes(0);
      Serial.println("sleep timer off");
      return true;
    }
    const long mins = strtol(arg, nullptr, 10);
    if (mins <= 0) {
      Serial.println("usage: audio sleep <minutes|off>");
      return true;
    }
    audio->setSleepTimerMinutes(static_cast<uint32_t>(mins));
    Serial.printf("sleep timer: stop playback in %ld min\n", mins);
    return true;
  }
  if (strcmp(verb, "next") == 0 || strcmp(verb, "previous") == 0) {
    if (s_listCount == 0) {
      refreshList(services);
    }
    const int step = strcmp(verb, "next") == 0 ? 1 : -1;
    if (!playIndex(services, s_currentIndex + step)) {
      Serial.println("end of list");
    }
    return true;
  }

  return false;
}
