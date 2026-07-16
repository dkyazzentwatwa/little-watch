#include "OpenMeteoWeatherService.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include <time.h>

#include "../core/EventBus.h"
#include "../storage/AtomicFile.h"
#include "SettingsService.h"
#include "WifiService.h"

namespace {

constexpr const char* kCachePath = "/weather_cache.json";
constexpr uint32_t kAutoRefreshMs = 30UL * 60UL * 1000UL;

const char* conditionFromWmo(int code) {
  if (code == 0) return "Clear";
  if (code <= 2) return "Mostly clear";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code == 85 || code == 86) return "Snow showers";
  if (code >= 95) return "Thunderstorm";
  return "Weather";
}

String urlEncode(const String& in) {
  String out;
  out.reserve(in.length() * 3);
  for (size_t i = 0; i < in.length(); i++) {
    const char c = in.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.') {
      out += c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

bool httpGetJson(const String& url, JsonDocument& doc, const JsonDocument* filter) {
  WiFiClientSecure client;
  client.setInsecure();  // no cert store on-device; documented tradeoff
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(8000);
  if (!http.begin(client, url)) {
    return false;
  }
  const int code = http.GET();
  bool ok = false;
  if (code == 200) {
    DeserializationError err;
    if (filter != nullptr) {
      err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(*filter));
    } else {
      err = deserializeJson(doc, http.getStream());
    }
    ok = !err;
    if (err) {
      Serial.printf("[weather] json error: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[weather] http %d\n", code);
  }
  http.end();
  return ok;
}

}  // namespace

void weatherFetchTask(void* arg) {
  OpenMeteoWeatherService* self = static_cast<OpenMeteoWeatherService*>(arg);
  float lat = self->resolvedLat_;
  float lon = self->resolvedLon_;
  self->didGeocode_ = false;

  // Geocode when we only have a city name.
  if ((lat == 0.0f && lon == 0.0f) && self->resolvedName_[0] != '\0') {
    JsonDocument geo;
    const String url = String("https://geocoding-api.open-meteo.com/v1/search?count=1&name=") +
                       urlEncode(self->resolvedName_);
    if (!httpGetJson(url, geo, nullptr) || geo["results"].size() == 0) {
      Serial.println("[weather] geocoding failed");
      self->fetchState_ = 3;
      vTaskDelete(nullptr);
      return;
    }
    lat = geo["results"][0]["latitude"] | 0.0f;
    lon = geo["results"][0]["longitude"] | 0.0f;
    const char* name = geo["results"][0]["name"] | self->resolvedName_;
    strncpy(self->resolvedName_, name, sizeof(self->resolvedName_) - 1);
    self->resolvedLat_ = lat;
    self->resolvedLon_ = lon;
    self->didGeocode_ = true;
    Serial.printf("[weather] %s -> %.3f,%.3f\n", self->resolvedName_, lat, lon);
  }
  if (lat == 0.0f && lon == 0.0f) {
    self->fetchState_ = 3;
    vTaskDelete(nullptr);
    return;
  }

  JsonDocument filter;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["weather_code"] = true;
  filter["current"]["precipitation_probability"] = true;
  filter["daily"]["temperature_2m_max"] = true;
  filter["daily"]["temperature_2m_min"] = true;
  filter["daily"]["precipitation_probability_max"] = true;
  filter["daily"]["weather_code"] = true;

  JsonDocument doc;
  char url[256];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code,precipitation_probability"
           "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
           "weather_code&forecast_days=3&timezone=auto",
           lat, lon);
  if (!httpGetJson(String(url), doc, &filter)) {
    self->fetchState_ = 3;
    vTaskDelete(nullptr);
    return;
  }

  WeatherSnapshot& s = self->staging_;
  s = WeatherSnapshot();
  s.valid = true;
  strncpy(s.location, self->resolvedName_, sizeof(s.location) - 1);
  s.temperatureC = doc["current"]["temperature_2m"] | 0.0f;
  s.precipitationChancePct = doc["current"]["precipitation_probability"] | 0;
  strncpy(s.condition, conditionFromWmo(doc["current"]["weather_code"] | -1),
          sizeof(s.condition) - 1);
  for (int i = 0; i < 3; i++) {
    s.days[i].highC = doc["daily"]["temperature_2m_max"][i] | 0.0f;
    s.days[i].lowC = doc["daily"]["temperature_2m_min"][i] | 0.0f;
    s.days[i].precipitationChancePct = doc["daily"]["precipitation_probability_max"][i] | 0;
    strncpy(s.days[i].condition, conditionFromWmo(doc["daily"]["weather_code"][i] | -1),
            sizeof(s.days[i].condition) - 1);
  }
  s.highC = s.days[0].highC;
  s.lowC = s.days[0].lowC;
  s.fetchedAtUptimeMs = millis();
  time_t nowEpoch = time(nullptr);
  s.fetchedAtEpoch = nowEpoch > 1700000000 ? (uint32_t)nowEpoch : 0;

  self->fetchState_ = 2;
  vTaskDelete(nullptr);
}

void OpenMeteoWeatherService::attach(SettingsService* settings, WifiService* wifi,
                                     EventBus* events) {
  settings_ = settings;
  wifi_ = wifi;
  events_ = events;
}

void OpenMeteoWeatherService::begin() {
  loadCache();
}

bool OpenMeteoWeatherService::refresh() {
  if (fetchState_ == 1) {
    return false;  // already running
  }
  if (wifi_ == nullptr || !wifi_->internet()) {
    Serial.println("[weather] no internet; keeping cached data");
    return false;
  }
  if (settings_ == nullptr ||
      (settings_->weatherCity().length() == 0 && settings_->weatherLat() == 0.0f &&
       settings_->weatherLon() == 0.0f)) {
    Serial.println("[weather] no location set (portal or: settings set weather.city <name>)");
    return false;
  }
  resolvedLat_ = settings_->weatherLat();
  resolvedLon_ = settings_->weatherLon();
  strncpy(resolvedName_, settings_->weatherCity().c_str(), sizeof(resolvedName_) - 1);
  resolvedName_[sizeof(resolvedName_) - 1] = '\0';

  fetchState_ = 1;
  xTaskCreate(weatherFetchTask, "weather", 12288, this, 1, nullptr);
  return true;
}

void OpenMeteoWeatherService::update(uint32_t deltaMs) {
  sinceFetchMs_ += deltaMs;

  if (fetchState_ == 2) {
    fetchState_ = 0;
    snapshot_ = staging_;
    everFetched_ = true;
    sinceFetchMs_ = 0;
    if (didGeocode_ && settings_ != nullptr) {
      settings_->setWeatherLocation(resolvedName_, resolvedLat_, resolvedLon_);
    }
    saveCache();
    Serial.printf("[weather] %s: %.0f C, %s\n", snapshot_.location, snapshot_.temperatureC,
                  snapshot_.condition);
    if (events_ != nullptr) {
      events_->publish(SystemEvent::WeatherUpdated);
    }
  } else if (fetchState_ == 3) {
    fetchState_ = 0;
    sinceFetchMs_ = 0;  // back off a full period before retrying
  }

  // Gentle auto-refresh whenever the internet is up.
  if (wifi_ != nullptr && wifi_->internet() &&
      (!everFetched_ || sinceFetchMs_ >= kAutoRefreshMs) && fetchState_ == 0) {
    refresh();
  }
}

void OpenMeteoWeatherService::loadCache() {
  fs::File f = LittleFS.open(kCachePath, FILE_READ);
  if (!f) {
    return;
  }
  JsonDocument doc;
  const bool ok = !deserializeJson(doc, f);
  f.close();
  if (!ok) {
    return;
  }
  snapshot_ = WeatherSnapshot();
  snapshot_.valid = true;
  strncpy(snapshot_.location, doc["loc"] | "", sizeof(snapshot_.location) - 1);
  snapshot_.temperatureC = doc["t"] | 0.0f;
  snapshot_.highC = doc["hi"] | 0.0f;
  snapshot_.lowC = doc["lo"] | 0.0f;
  snapshot_.precipitationChancePct = doc["pp"] | 0;
  strncpy(snapshot_.condition, doc["c"] | "", sizeof(snapshot_.condition) - 1);
  for (int i = 0; i < 3; i++) {
    snapshot_.days[i].highC = doc["d"][i]["hi"] | 0.0f;
    snapshot_.days[i].lowC = doc["d"][i]["lo"] | 0.0f;
    snapshot_.days[i].precipitationChancePct = doc["d"][i]["pp"] | 0;
    strncpy(snapshot_.days[i].condition, doc["d"][i]["c"] | "",
            sizeof(snapshot_.days[i].condition) - 1);
  }
  snapshot_.fetchedAtEpoch = doc["at"] | 0;
  snapshot_.fetchedAtUptimeMs = 0;  // readers treat 0 as "cached from disk"
  Serial.println("[weather] loaded cached snapshot");
}

void OpenMeteoWeatherService::saveCache() {
  JsonDocument doc;
  doc["loc"] = snapshot_.location;
  doc["t"] = snapshot_.temperatureC;
  doc["hi"] = snapshot_.highC;
  doc["lo"] = snapshot_.lowC;
  doc["pp"] = snapshot_.precipitationChancePct;
  doc["c"] = snapshot_.condition;
  JsonArray days = doc["d"].to<JsonArray>();
  for (int i = 0; i < 3; i++) {
    JsonObject d = days.add<JsonObject>();
    d["hi"] = snapshot_.days[i].highC;
    d["lo"] = snapshot_.days[i].lowC;
    d["pp"] = snapshot_.days[i].precipitationChancePct;
    d["c"] = snapshot_.days[i].condition;
  }
  doc["at"] = snapshot_.fetchedAtEpoch;
  String body;
  serializeJson(doc, body);
  AtomicFile::writeAll(LittleFS, kCachePath, body);
}
