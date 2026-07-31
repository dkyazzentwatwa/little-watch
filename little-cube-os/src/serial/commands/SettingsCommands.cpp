#include "SettingsCommands.h"

#include <Arduino.h>
#include <stdlib.h>

#include "../../board_config.h"
#include "../../core/EventBus.h"
#include "../../core/Services.h"
#include "../../core/SystemState.h"
#include "../../hardware/audio/AudioAdapter.h"
#include "../../services/SettingsService.h"
#include "../../services/WeatherService.h"
#include "../../ui/ClockFaces.h"
#include "../../ui/Theme.h"
#include "../CmdArgs.h"

namespace {

// "HH:MM" -> minutes since midnight. Rejects anything else, including the
// 24:00 / 07:60 forms an off-by-one would produce.
bool parseHhMm(const char* text, uint16_t& outMinutes) {
  if (text == nullptr) {
    return false;
  }
  int hh = 0;
  int mm = 0;
  char extra = '\0';
  if (sscanf(text, "%d:%d%c", &hh, &mm, &extra) != 2) {
    return false;
  }
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
    return false;
  }
  outMinutes = static_cast<uint16_t>(hh * 60 + mm);
  return true;
}

bool parseBool(const char* text, bool& out) {
  if (text == nullptr) {
    return false;
  }
  if (strcmp(text, "on") == 0 || strcmp(text, "true") == 0 || strcmp(text, "yes") == 0 ||
      strcmp(text, "1") == 0) {
    out = true;
    return true;
  }
  if (strcmp(text, "off") == 0 || strcmp(text, "false") == 0 || strcmp(text, "no") == 0 ||
      strcmp(text, "0") == 0) {
    out = false;
    return true;
  }
  return false;
}

// Whole-number parse with no trailing junk: "60s" and "abc" are both refused
// rather than silently read as 60 and 0.
bool parseLong(const char* text, long& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  if (end == text || *end != '\0') {
    return false;
  }
  out = value;
  return true;
}

void printHhMm(uint16_t minutes) {
  Serial.printf("%02u:%02u", (unsigned)(minutes / 60), (unsigned)(minutes % 60));
}

void printTimeout(const SettingsService& s) {
  if (s.screenTimeoutSec() == 0) {
    Serial.print("never");
  } else {
    Serial.printf("%us", (unsigned)s.screenTimeoutSec());
  }
}

// One key, one line. Used by both `list` and `get` so the two can never drift.
void printValue(SettingsService& s, const char* key) {
  Serial.printf("%-18s ", key);
  if (strcmp(key, "brightness") == 0) {
    Serial.printf("%u\n", (unsigned)s.brightness());
  } else if (strcmp(key, "timeout") == 0) {
    printTimeout(s);
    Serial.println();
  } else if (strcmp(key, "alwayson") == 0) {
    Serial.println(s.alwaysOn() ? "on" : "off");
  } else if (strcmp(key, "devicename") == 0) {
    Serial.println(s.deviceName());
  } else if (strcmp(key, "timezone") == 0) {
    Serial.println(s.timezone());
  } else if (strcmp(key, "volume") == 0) {
    Serial.printf("%u%%\n", (unsigned)s.volumePercent());
  } else if (strcmp(key, "bedtime") == 0) {
    Serial.println(s.bedtimeEnabled() ? "on" : "off");
  } else if (strcmp(key, "bedtimestart") == 0) {
    printHhMm(s.bedtimeStartMin());
    Serial.println();
  } else if (strcmp(key, "bedtimeend") == 0) {
    printHhMm(s.bedtimeEndMin());
    Serial.println();
  } else if (strcmp(key, "bedtimebrightness") == 0) {
    Serial.printf("%u\n", (unsigned)s.bedtimeBrightness());
  } else if (strcmp(key, "clockface") == 0) {
    // No clockfaces::name() yet — Task 5 adds the renderers and the name
    // lookup. Print the bare index rather than inventing a label.
    Serial.printf("%u\n", (unsigned)s.clockFace());
  }
}

const char* const kKeys[] = {
    "brightness", "timeout",      "alwayson",     "devicename",       "timezone",
    "volume",     "bedtime",      "bedtimestart", "bedtimeend",       "bedtimebrightness",
    "clockface",
};
constexpr size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

bool knownKey(const char* key) {
  for (size_t i = 0; i < kKeyCount; i++) {
    if (strcmp(key, kKeys[i]) == 0) {
      return true;
    }
  }
  return false;
}

void printUnknownKey(const char* key) {
  Serial.printf("error: no setting '%s'. keys:\n  ", key);
  for (size_t i = 0; i < kKeyCount; i++) {
    Serial.printf("%s%s", kKeys[i], i + 1 < kKeyCount ? " · " : "\n");
  }
}

// The timezone is the one setting that other services must react to at once:
// the RTC holds LOCAL wall time, so a new zone makes its contents wrong
// immediately. TimeService subscribes to this and re-applies (spec §38).
void publishSettingsChanged(Services& services) {
  if (services.events != nullptr) {
    services.events->publish(SystemEvent::SettingsChanged);
  }
}

}  // namespace

void printSettingsHelp() {
  Serial.println("settings list                    every setting and its current value");
  Serial.println("settings get <key>               one value");
  Serial.println("settings set <key> <value>       change one value");
  Serial.println("settings bedtime <on|off> [HH:MM HH:MM] [brightness]");
  Serial.println("                                 dim window; the only editor for the times");
  Serial.println("settings themes                  list the 10 palettes (dark + light)");
  Serial.println("keys:");
  Serial.println("  theme              0-9 or a name (e.g. 'Paper') — 'settings themes' lists them");
  Serial.printf("  brightness         %u-%u\n", (unsigned)MIN_BRIGHTNESS,
                (unsigned)MAX_BRIGHTNESS);
  Serial.println("  timeout            screen-off seconds (5-3600), or 0 / never");
  Serial.println("  alwayson           on|off — keep the screen lit while powered");
  Serial.println("  devicename         up to 32 characters");
  Serial.println("  timezone           POSIX TZ, e.g. GMT0BST,M3.5.0/1,M10.5.0 · UTC0");
  Serial.println("  volume             0-100");
  Serial.println("  bedtime            on|off");
  Serial.println("  bedtimestart       HH:MM   bedtimeend  HH:MM   (wraps midnight)");
  Serial.printf("  bedtimebrightness  %u-%u — a nightly ceiling, never a floor\n",
                (unsigned)MIN_BRIGHTNESS, (unsigned)MAX_BRIGHTNESS);
  Serial.printf("  clockface          0-%u — selects the Clock app face (Task 5)\n",
                (unsigned)(clockfaces::kFaceCount - 1));
  Serial.println("  weather.city       town name, e.g. London — geocoded over Wi-Fi");
  Serial.println("the stored value is printed back after every set. Numeric ranges are");
  Serial.println("clamped; enumerated keys (theme, clockface) reject out-of-range instead.");
  Serial.println("Wi-Fi credentials are not here: see 'help wifi'.");
}

bool handleSettingsCommand(Services& services, const char* verb, char* args) {
  if (services.settings == nullptr) {
    Serial.println("error: settings service unavailable");
    return true;
  }
  SettingsService& s = *services.settings;

  if (strcmp(verb, "list") == 0) {
    for (size_t i = 0; i < kKeyCount; i++) {
      printValue(s, kKeys[i]);
    }
    // Settable by name; the service geocodes it to coordinates over Wi-Fi.
    const String city = s.weatherCity();
    Serial.printf("%-18s %s\n", "weather.city", city.length() > 0 ? city.c_str() : "-");
    Serial.printf("%-18s %u (%s)\n", "theme", (unsigned)s.themeIndex(),
                  theme::themeName(s.themeIndex()));
    return true;
  }

  if (strcmp(verb, "themes") == 0) {
    Serial.println("themes  (settings set theme <n|name>):");
    for (uint8_t i = 0; i < theme::kThemeCount; i++) {
      Serial.printf("  %u  %-11s %s%s\n", (unsigned)i, theme::themeName(i),
                    theme::themeIsLight(i) ? "light" : "dark",
                    i == s.themeIndex() ? "  <-- active" : "");
    }
    return true;
  }

  if (strcmp(verb, "get") == 0) {
    char* cursor = args;
    const char* key = cmdargs::nextToken(cursor);
    if (key == nullptr) {
      Serial.println("usage: settings get <key>   ('settings list' shows them all)");
      return true;
    }
    if (!knownKey(key)) {
      printUnknownKey(key);
      return true;
    }
    printValue(s, key);
    return true;
  }

  if (strcmp(verb, "set") == 0) {
    char* cursor = args;
    const char* key = cmdargs::nextToken(cursor);
    // rest() so device names and POSIX TZ strings survive with their spaces.
    const char* value = cmdargs::rest(cursor);
    if (key == nullptr || value == nullptr) {
      Serial.println("usage: settings set <key> <value>   ('help settings' lists the keys)");
      return true;
    }

    if (strcmp(key, "brightness") == 0 || strcmp(key, "bedtimebrightness") == 0) {
      long v = 0;
      if (!parseLong(value, v)) {
        Serial.printf("usage: settings set %s <%u-%u>\n", key, (unsigned)MIN_BRIGHTNESS,
                      (unsigned)MAX_BRIGHTNESS);
        return true;
      }
      // Clamped inside the setter; clamp the cast here only so a value past
      // 255 does not wrap round to a dim one on the way in.
      const uint8_t byteValue = v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v));
      if (strcmp(key, "brightness") == 0) {
        // Setting only — AmoledProtection owns the panel and follows within a
        // frame. Two writers would fight over dim and blank.
        s.setBrightness(byteValue);
      } else {
        s.setBedtimeBrightness(byteValue);
      }
      printValue(s, key);
      return true;
    }

    if (strcmp(key, "timeout") == 0) {
      long v = 0;
      if (strcmp(value, "never") == 0) {
        v = 0;
      } else if (!parseLong(value, v) || v < 0) {
        Serial.println("usage: settings set timeout <seconds|never>   (5-3600, 0 = never)");
        return true;
      }
      s.setScreenTimeoutSec(static_cast<uint32_t>(v));
      printValue(s, key);
      return true;
    }

    if (strcmp(key, "alwayson") == 0 || strcmp(key, "bedtime") == 0) {
      bool on = false;
      if (!parseBool(value, on)) {
        Serial.printf("usage: settings set %s <on|off>\n", key);
        return true;
      }
      if (strcmp(key, "alwayson") == 0) {
        s.setAlwaysOn(on);
      } else {
        s.setBedtimeEnabled(on);
      }
      printValue(s, key);
      return true;
    }

    if (strcmp(key, "devicename") == 0) {
      s.setDeviceName(String(value));
      printValue(s, key);
      return true;
    }

    if (strcmp(key, "timezone") == 0) {
      s.setTimezone(String(value));
      printValue(s, key);
      // Without this the new zone would not land until the next reboot (or
      // the hourly safety net in TimeService).
      publishSettingsChanged(services);
      return true;
    }

    if (strcmp(key, "volume") == 0) {
      long v = 0;
      if (!parseLong(value, v)) {
        Serial.println("usage: settings set volume <0-100>");
        return true;
      }
      const uint8_t pct = v < 0 ? 0 : (v > 100 ? 100 : static_cast<uint8_t>(v));
      s.setVolumePercent(pct);
      // The codec keeps its own copy; leaving it behind would make the stored
      // value a lie until the next reboot.
      if (services.audio != nullptr) {
        services.audio->setVolumePercent(s.volumePercent());
      }
      printValue(s, key);
      return true;
    }

    if (strcmp(key, "bedtimestart") == 0 || strcmp(key, "bedtimeend") == 0) {
      uint16_t minutes = 0;
      if (!parseHhMm(value, minutes)) {
        Serial.printf("usage: settings set %s HH:MM   (24-hour local time)\n", key);
        return true;
      }
      // setBedtimeWindow takes both ends, so the other one is passed through
      // unchanged rather than reset to a default.
      if (strcmp(key, "bedtimestart") == 0) {
        s.setBedtimeWindow(minutes, s.bedtimeEndMin());
      } else {
        s.setBedtimeWindow(s.bedtimeStartMin(), minutes);
      }
      printValue(s, key);
      return true;
    }

    if (strcmp(key, "theme") == 0) {
      // Accept an index (0..N-1) or a name (case-insensitive prefix).
      long v = -1;
      if (parseLong(value, v) && v >= 0 && v < theme::kThemeCount) {
        // numeric index chosen
      } else {
        v = -1;
        for (uint8_t i = 0; i < theme::kThemeCount; i++) {
          if (strcasecmp(value, theme::themeName(i)) == 0) {
            v = i;
            break;
          }
        }
      }
      if (v < 0) {
        Serial.println("usage: settings set theme <0-9 or name>  ('settings themes' lists them)");
        return true;
      }
      s.setThemeIndex(static_cast<uint8_t>(v));
      theme::applyTheme(s.themeIndex());
      // Bump the shared version so the foreground app repaints with the new
      // palette on its next frame (apps redraw when state.version changes).
      if (services.state != nullptr) {
        services.state->version++;
      }
      Serial.printf("theme %u (%s, %s)\n", (unsigned)s.themeIndex(),
                    theme::themeName(s.themeIndex()),
                    theme::themeIsLight(s.themeIndex()) ? "light" : "dark");
      return true;
    }

    if (strcmp(key, "clockface") == 0) {
      // No name lookup yet (Task 5 adds clockfaces::name()) — numeric index
      // only, mirroring theme's numeric path without the name-matching leg.
      long v = -1;
      if (!parseLong(value, v) || v < 0 || v >= clockfaces::kFaceCount) {
        Serial.printf("usage: settings set clockface <0-%u>\n",
                      (unsigned)(clockfaces::kFaceCount - 1));
        return true;
      }
      s.setClockFace(static_cast<uint8_t>(v));
      printValue(s, key);
      return true;
    }

    if (strcmp(key, "weather.city") == 0 || strcmp(key, "weathercity") == 0 ||
        strcmp(key, "city") == 0) {
      // Store the name with zeroed coordinates; the weather service geocodes a
      // name-only location on its next fetch and writes the resolved lat/lon
      // back into settings. Needs Wi-Fi to actually resolve.
      s.setWeatherLocation(String(value), 0.0f, 0.0f);
      Serial.printf("weather city set to \"%s\"\n", value);
      if (services.weather != nullptr && services.weather->refresh()) {
        Serial.println("resolving location and fetching forecast...");
      } else {
        Serial.println("saved — will resolve once Wi-Fi/internet is available");
      }
      return true;
    }

    printUnknownKey(key);
    return true;
  }

  if (strcmp(verb, "bedtime") == 0) {
    char* cursor = args;
    const char* state = cmdargs::nextToken(cursor);
    const char* startToken = cmdargs::nextToken(cursor);
    const char* endToken = cmdargs::nextToken(cursor);
    const char* brightToken = cmdargs::nextToken(cursor);

    if (state == nullptr) {
      Serial.print("bedtime ");
      Serial.print(s.bedtimeEnabled() ? "on" : "off");
      Serial.print("  ");
      printHhMm(s.bedtimeStartMin());
      Serial.print("-");
      printHhMm(s.bedtimeEndMin());
      Serial.printf(" at brightness %u\n", (unsigned)s.bedtimeBrightness());
      Serial.println("usage: settings bedtime <on|off> [HH:MM HH:MM] [brightness]");
      return true;
    }
    bool on = false;
    if (!parseBool(state, on)) {
      Serial.println("usage: settings bedtime <on|off> [HH:MM HH:MM] [brightness]");
      return true;
    }
    // Times come as a pair: one alone is ambiguous about which end it is.
    if ((startToken == nullptr) != (endToken == nullptr)) {
      Serial.println("usage: settings bedtime <on|off> [HH:MM HH:MM] [brightness]");
      Serial.println("  give both ends of the window, or neither");
      return true;
    }
    uint16_t startMin = 0;
    uint16_t endMin = 0;
    if (startToken != nullptr) {
      if (!parseHhMm(startToken, startMin) || !parseHhMm(endToken, endMin)) {
        Serial.println("error: times are HH:MM, 24-hour (22:00 07:00)");
        return true;
      }
      if (startMin == endMin) {
        Serial.println("error: the window would be empty (start and end are the same)");
        return true;
      }
    }
    long bright = 0;
    if (brightToken != nullptr && !parseLong(brightToken, bright)) {
      Serial.printf("error: brightness is a number %u-%u\n", (unsigned)MIN_BRIGHTNESS,
                    (unsigned)MAX_BRIGHTNESS);
      return true;
    }

    // Nothing is written until every argument has parsed, so a typo in the
    // last one cannot leave the window half-changed.
    if (startToken != nullptr) {
      s.setBedtimeWindow(startMin, endMin);
    }
    if (brightToken != nullptr) {
      s.setBedtimeBrightness(bright < 0 ? 0 : (bright > 255 ? 255 : static_cast<uint8_t>(bright)));
    }
    s.setBedtimeEnabled(on);

    Serial.print("bedtime ");
    Serial.print(s.bedtimeEnabled() ? "on" : "off");
    Serial.print("  ");
    printHhMm(s.bedtimeStartMin());
    Serial.print("-");
    printHhMm(s.bedtimeEndMin());
    Serial.printf(" at brightness %u", (unsigned)s.bedtimeBrightness());
    Serial.println(s.bedtimeStartMin() > s.bedtimeEndMin() ? "  (wraps midnight)" : "");
    return true;
  }

  return false;
}
