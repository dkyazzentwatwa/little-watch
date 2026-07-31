#include "RadioService.h"

#include <SD_MMC.h>

#include "../hardware/SdCardAdapter.h"
#include "../storage/AtomicFile.h"
#include "../storage/SdStorage.h"
#include "../storage/StoragePaths.h"

namespace {

constexpr const char* kStationsPath = paths::kRadioStations;

struct DefaultStation {
  const char* name;
  const char* url;
};

constexpr DefaultStation kDefaults[] = {
    {"France Inter", "http://icecast.radiofrance.fr/franceinter-lofi.mp3"},
    {"FIP", "http://icecast.radiofrance.fr/fip-midfi.mp3"},
    {"France Musique", "http://icecast.radiofrance.fr/francemusique-lofi.mp3"},
};

void copyField(char* out, size_t outSize, const String& value) {
  if (out == nullptr || outSize == 0) {
    return;
  }
  strncpy(out, value.c_str(), outSize - 1);
  out[outSize - 1] = '\0';
}

}  // namespace

void RadioService::begin(SdStorage* storage) {
  storage_ = storage;
}

bool RadioService::valid(const char* name, const char* url) const {
  if (name == nullptr || url == nullptr || name[0] == '\0' || url[0] == '\0') {
    return false;
  }
  if (strlen(name) >= 64 || strlen(url) >= 128) {
    return false;
  }
  if (strchr(name, '|') != nullptr || strchr(name, '\n') != nullptr || strchr(name, '\r') != nullptr) {
    return false;
  }
  // The installed ESP8266Audio HTTP source uses a plain NetworkClient. Keep
  // v1 explicit and compatible with direct HTTP MP3 streams.
  return strncmp(url, "http://", 7) == 0;
}

bool RadioService::load(RadioStationInfo* out, size_t capacity, size_t* total) {
  if (total != nullptr) {
    *total = 0;
  }
  if (storage_ == nullptr || storage_->card() == nullptr || !storage_->card()->mounted()) {
    return false;
  }

  if (!SD_MMC.exists(kStationsPath)) {
    if (!SD_MMC.exists(paths::kRadio)) {
      SD_MMC.mkdir(paths::kRadio);
    }
    RadioStationInfo defaults[sizeof(kDefaults) / sizeof(kDefaults[0])];
    for (size_t i = 0; i < sizeof(kDefaults) / sizeof(kDefaults[0]); i++) {
      strncpy(defaults[i].name, kDefaults[i].name, sizeof(defaults[i].name) - 1);
      defaults[i].name[sizeof(defaults[i].name) - 1] = '\0';
      strncpy(defaults[i].url, kDefaults[i].url, sizeof(defaults[i].url) - 1);
      defaults[i].url[sizeof(defaults[i].url) - 1] = '\0';
    }
    save(defaults, sizeof(kDefaults) / sizeof(kDefaults[0]));
  }

  fs::File file = SD_MMC.open(kStationsPath, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    return false;
  }

  size_t count = 0;
  while (file.available() > 0 && count < kMaxStations) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) {
      continue;
    }
    const int split = line.indexOf('|');
    if (split <= 0 || split >= static_cast<int>(line.length() - 1)) {
      continue;
    }
    String name = line.substring(0, split);
    String url = line.substring(split + 1);
    name.trim();
    url.trim();
    if (!valid(name.c_str(), url.c_str())) {
      continue;
    }
    copyField(out != nullptr && count < capacity ? out[count].name : nullptr,
              64, name);
    copyField(out != nullptr && count < capacity ? out[count].url : nullptr,
              128, url);
    count++;
  }
  file.close();
  if (total != nullptr) {
    *total = count;
  }
  return true;
}

size_t RadioService::list(RadioStationInfo* out, size_t capacity, size_t* total) {
  size_t count = 0;
  if (!load(out, capacity, &count)) {
    if (total != nullptr) {
      *total = 0;
    }
    return 0;
  }
  if (total != nullptr) {
    *total = count;
  }
  return count < capacity ? count : capacity;
}

bool RadioService::save(const RadioStationInfo* stations, size_t count) {
  if (storage_ == nullptr || storage_->card() == nullptr || !storage_->card()->writable() ||
      stations == nullptr || count > kMaxStations) {
    return false;
  }
  String text;
  for (size_t i = 0; i < count; i++) {
    if (!valid(stations[i].name, stations[i].url)) {
      return false;
    }
    text += stations[i].name;
    text += '|';
    text += stations[i].url;
    text += '\n';
  }
  return AtomicFile::writeAll(SD_MMC, kStationsPath, text);
}

bool RadioService::add(const char* name, const char* url) {
  if (!valid(name, url)) {
    return false;
  }
  RadioStationInfo stations[kMaxStations];
  size_t count = 0;
  load(stations, kMaxStations, &count);
  if (count >= kMaxStations) {
    return false;
  }
  strncpy(stations[count].name, name, sizeof(stations[count].name) - 1);
  strncpy(stations[count].url, url, sizeof(stations[count].url) - 1);
  return save(stations, count + 1);
}

bool RadioService::remove(size_t index) {
  RadioStationInfo stations[kMaxStations];
  size_t count = 0;
  if (!load(stations, kMaxStations, &count) || index >= count) {
    return false;
  }
  for (size_t i = index + 1; i < count; i++) {
    stations[i - 1] = stations[i];
  }
  return save(stations, count - 1);
}
