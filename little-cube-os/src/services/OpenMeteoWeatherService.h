#pragma once

#include "WeatherService.h"

class SettingsService;
class WifiService;
class EventBus;

// Open-Meteo implementation: geocoding-api.open-meteo.com for city -> lat/lon
// during setup, api.open-meteo.com/v1/forecast for data. No API key. HTTPS
// via WiFiClientSecure::setInsecure() (documented tradeoff). Fetches run on
// a short-lived FreeRTOS job task; results land in the snapshot and a
// WeatherUpdated event. Last good response is cached in LittleFS.
class OpenMeteoWeatherService : public WeatherService {
 public:
  void begin() override;
  void update(uint32_t deltaMs) override;
  bool refresh() override;
  const WeatherSnapshot& snapshot() const override { return snapshot_; }

  void attach(SettingsService* settings, WifiService* wifi, EventBus* events);

 private:
  SettingsService* settings_ = nullptr;
  WifiService* wifi_ = nullptr;
  EventBus* events_ = nullptr;
  WeatherSnapshot snapshot_;
};
