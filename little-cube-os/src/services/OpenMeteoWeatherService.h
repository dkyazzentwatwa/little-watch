#pragma once

#include "WeatherService.h"

class SettingsService;
class WifiService;
class EventBus;

// Open-Meteo implementation: no API key. City -> lat/lon via
// geocoding-api.open-meteo.com (resolved coordinates persist in settings);
// forecast via api.open-meteo.com. HTTPS with setInsecure() — documented
// tradeoff, no certificate management on-device. Fetches run on a
// short-lived FreeRTOS task writing into a staging area the update() tick
// consumes, so the UI never blocks. The last good snapshot is cached in
// LittleFS and served (age-labelled) when offline.
class OpenMeteoWeatherService : public WeatherService {
 public:
  void begin() override;
  void update(uint32_t deltaMs) override;
  bool refresh() override;
  const WeatherSnapshot& snapshot() const override { return snapshot_; }

  void attach(SettingsService* settings, WifiService* wifi, EventBus* events);

 private:
  friend void weatherFetchTask(void* arg);

  void loadCache();
  void saveCache();

  SettingsService* settings_ = nullptr;
  WifiService* wifi_ = nullptr;
  EventBus* events_ = nullptr;

  WeatherSnapshot snapshot_;

  // Staging written by the fetch task, consumed on the main loop.
  WeatherSnapshot staging_;
  volatile int fetchState_ = 0;  // 0 idle, 1 running, 2 success, 3 failed
  float resolvedLat_ = 0.0f;
  float resolvedLon_ = 0.0f;
  char resolvedName_[48] = "";
  bool didGeocode_ = false;

  uint32_t sinceFetchMs_ = 0;
  bool everFetched_ = false;
};
