#pragma once

#include <Arduino.h>
#include <math.h>  // lroundf, for roundC() below

// Provider-agnostic weather interface (spec §13). Implementations fetch on
// a short-lived background task and publish SystemEvent::WeatherUpdated;
// readers use the cached snapshot. Cached data always carries its age —
// the UI must never present stale weather as current.
struct WeatherSnapshot {
  bool valid = false;
  char location[48] = "";
  float temperatureC = 0.0f;
  float highC = 0.0f;
  float lowC = 0.0f;
  int precipitationChancePct = 0;
  char condition[32] = "";
  // Simple 3-day outlook.
  struct Day {
    float highC = 0.0f;
    float lowC = 0.0f;
    int precipitationChancePct = 0;
    char condition[24] = "";
  } days[3];
  uint32_t fetchedAtEpoch = 0;  // 0 = unknown
  uint32_t fetchedAtUptimeMs = 0;
};

// Rounds a Celsius reading for display. Lives here, beside the struct whose
// floats it rounds, because three screens show the same snapshot — the Weather
// app, the Today readout and the Home carousel — and they must never disagree
// about what a temperature is.
//
// NOT `(int)(c + 0.5f)`: that rounds toward zero after the offset, so -3.4
// renders as -2. This is the bug that was already live on two screens.
inline int roundC(float c) { return static_cast<int>(lroundf(c)); }

class WeatherService {
 public:
  virtual ~WeatherService() = default;

  virtual void begin() = 0;
  virtual void update(uint32_t deltaMs) = 0;

  // Kicks an asynchronous refresh; returns false when offline/busy.
  virtual bool refresh() = 0;

  virtual const WeatherSnapshot& snapshot() const = 0;
};
