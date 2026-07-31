#pragma once

#include <Arduino.h>

class SdStorage;

struct RadioStationInfo {
  char name[64] = "";
  char url[128] = "";
};

// Human-editable station presets stored on the SD card. The format is one
// station per line: Name|http://stream-url. Radio itself never writes audio
// data to the card.
class RadioService {
 public:
  static constexpr size_t kMaxStations = 16;

  void begin(SdStorage* storage);
  size_t list(RadioStationInfo* out, size_t capacity, size_t* total = nullptr);
  bool add(const char* name, const char* url);
  bool remove(size_t index);

 private:
  bool load(RadioStationInfo* out, size_t capacity, size_t* total);
  bool save(const RadioStationInfo* stations, size_t count);
  bool valid(const char* name, const char* url) const;

  SdStorage* storage_ = nullptr;
};
