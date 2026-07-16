#include "ProvisioningService.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "SettingsService.h"
#include "WifiService.h"

namespace {

WebServer* server = nullptr;
DNSServer* dns = nullptr;

constexpr uint32_t kIdleTimeoutMs = 10UL * 60UL * 1000UL;
constexpr uint32_t kSuccessLingerMs = 6000;

// One compact page; password field is never pre-filled (masked secrets).
const char kPortalHead[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Little Cube setup</title><style>
body{font-family:-apple-system,system-ui,sans-serif;background:#101014;color:#eee;
margin:0;padding:24px;max-width:420px;margin:auto}
h1{font-size:1.3em}label{display:block;margin:14px 0 4px;color:#9ad}
input,select{width:100%;padding:10px;border-radius:8px;border:1px solid #333;
background:#1a1a20;color:#eee;font-size:1em;box-sizing:border-box}
button{margin-top:20px;width:100%;padding:12px;border-radius:10px;border:0;
background:#0cf;color:#001;font-size:1.05em;font-weight:600}
.row{display:flex;gap:8px;align-items:center;margin-top:6px}
.row input{width:auto}small{color:#888}</style></head><body>
<h1>Little Cube setup</h1>)HTML";

const char kPortalFormTail[] PROGMEM = R"HTML(
<form method="POST" action="/save">
<label>Network</label><select name="pick" onchange="document.getElementById('ssid').value=this.value">
%OPTIONS%</select>
<label>SSID</label><input id="ssid" name="ssid" maxlength="32" required>
<label>Password</label><input id="pw" name="password" type="password" maxlength="63">
<div class="row"><input type="checkbox" onclick="document.getElementById('pw').type=this.checked?'text':'password'">show password</div>
<div class="row"><input type="checkbox" name="hidden" value="1">hidden network</div>
<label>Device name</label><input name="devname" value="%DEVNAME%" maxlength="24">
<label>Timezone (POSIX)</label><input name="tz" value="%TZ%" maxlength="48">
<small>e.g. PST8PDT,M3.2.0,M11.1.0 · EST5EDT,M3.2.0,M11.1.0 · UTC0</small>
<label>Weather city</label><input name="city" value="%CITY%" maxlength="40">
<button type="submit">Save &amp; connect</button></form>
<p><a style="color:#0cf" href="/">rescan networks</a></p>
</body></html>)HTML";

}  // namespace

void ProvisioningService::begin(WifiService* wifi, SettingsService* settings) {
  wifi_ = wifi;
  settings_ = settings;
}

bool ProvisioningService::start() {
  if (phase_ != Phase::Off) {
    return true;
  }
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[20];
  snprintf(name, sizeof(name), "LittleCube-%02X%02X", mac[4], mac[5]);
  apName_ = name;
  char pass[12];
  snprintf(pass, sizeof(pass), "cube%02x%02x", mac[4] ^ 0xA5, mac[5] ^ 0x5A);
  apPassword_ = pass;

  WiFi.mode(WIFI_AP_STA);  // STA stays available for the connection test
  if (!WiFi.softAP(apName_.c_str(), apPassword_.c_str())) {
    Serial.println("[setup] softAP failed");
    WiFi.mode(WIFI_STA);
    return false;
  }
  portalIp_ = WiFi.softAPIP().toString();

  dns = new DNSServer();
  dns->start(53, "*", WiFi.softAPIP());

  server = new WebServer(80);
  server->on("/", [this]() { handleRoot(); });
  server->on("/save", HTTP_POST, [this]() { handleSave(); });
  server->on("/status", [this]() { handleStatus(); });
  server->onNotFound([this]() {
    server->sendHeader("Location", String("http://") + portalIp_ + "/", true);
    server->send(302, "text/plain", "");
  });
  server->begin();

  // Warm the network list for the form.
  wifi_->startScan(false);

  phase_ = Phase::Portal;
  lastError_ = "";
  idleMs_ = 0;
  Serial.printf("[setup] portal up: join %s (password %s) then open http://%s\n",
                apName_.c_str(), apPassword_.c_str(), portalIp_.c_str());
  return true;
}

void ProvisioningService::handleRoot() {
  idleMs_ = 0;
  String options = "<option value=\"\">choose...</option>";
  uint8_t count = 0;
  const WifiService::ScanResult* results = wifi_->scanResults(count);
  for (uint8_t i = 0; i < count; i++) {
    options += "<option value=\"";
    options += results[i].ssid;
    options += "\">";
    options += results[i].ssid;
    options += results[i].secure ? " (locked)" : " (open)";
    options += "</option>";
  }
  if (count == 0) {
    wifi_->startScan(false);  // list fills in on refresh
    options += "<option value=\"\">scanning... refresh shortly</option>";
  }

  String page = FPSTR(kPortalHead);
  String form = FPSTR(kPortalFormTail);
  form.replace("%OPTIONS%", options);
  form.replace("%DEVNAME%", settings_->deviceName());
  form.replace("%TZ%", settings_->timezone());
  form.replace("%CITY%", settings_->weatherCity());
  page += form;
  server->send(200, "text/html", page);
}

void ProvisioningService::handleSave() {
  idleMs_ = 0;
  pendingSsid_ = server->arg("ssid");
  pendingPassword_ = server->arg("password");
  pendingHidden_ = server->arg("hidden") == "1";
  if (pendingSsid_.length() == 0 || pendingSsid_.length() > 32) {
    server->send(400, "text/html", "<h3>SSID missing</h3><a href='/'>back</a>");
    return;
  }

  if (settings_ != nullptr) {
    const String devname = server->arg("devname");
    if (devname.length() > 0) {
      settings_->setDeviceName(devname);
    }
    const String tz = server->arg("tz");
    if (tz.length() > 0) {
      settings_->setTimezone(tz);
    }
    const String city = server->arg("city");
    if (city.length() > 0) {
      // Coordinates resolve on the first weather fetch (geocoding).
      settings_->setWeatherLocation(city, 0.0f, 0.0f);
    }
  }

  phase_ = Phase::Testing;
  testStarted_ = false;
  String page = FPSTR(kPortalHead);
  page += "<h3>Testing connection to \"" + pendingSsid_ +
          "\"...</h3><p>This page refreshes automatically.</p>"
          "<meta http-equiv='refresh' content='3;url=/status'></body></html>";
  server->send(200, "text/html", page);
}

void ProvisioningService::handleStatus() {
  idleMs_ = 0;
  String page = FPSTR(kPortalHead);
  switch (phase_) {
    case Phase::Testing:
      page += "<h3>Still connecting...</h3><meta http-equiv='refresh' content='3;url=/status'>";
      break;
    case Phase::Success:
      page += "<h3>Connected!</h3><p>Setup is complete — this network will now switch off. "
              "Your cube is online.</p>";
      break;
    case Phase::Failed:
      page += String("<h3>Could not connect</h3><p>") + lastError_ +
              "</p><p><a style='color:#0cf' href='/'>try again</a></p>";
      break;
    default:
      page += "<a style='color:#0cf' href='/'>setup</a>";
      break;
  }
  page += "</body></html>";
  server->send(200, "text/html", page);
}

void ProvisioningService::teardown() {
  if (server != nullptr) {
    server->stop();
    delete server;
    server = nullptr;
  }
  if (dns != nullptr) {
    dns->stop();
    delete dns;
    dns = nullptr;
  }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  // Wipe the AP credential and the pending copies (WifiService keeps the
  // durable copy in NVS).
  for (size_t i = 0; i < pendingPassword_.length(); i++) {
    pendingPassword_.setCharAt(i, '\0');
  }
  pendingPassword_ = "";
  phase_ = Phase::Off;
  Serial.println("[setup] portal stopped");
}

void ProvisioningService::stop() {
  if (phase_ != Phase::Off) {
    teardown();
  }
}

void ProvisioningService::update(uint32_t deltaMs) {
  if (phase_ == Phase::Off) {
    return;
  }
  if (server != nullptr) {
    server->handleClient();
  }
  if (dns != nullptr) {
    dns->processNextRequest();
  }

  idleMs_ += deltaMs;
  if (idleMs_ > kIdleTimeoutMs) {
    Serial.println("[setup] portal timed out (inactivity)");
    teardown();
    return;
  }

  if (phase_ == Phase::Testing) {
    if (!testStarted_) {
      testStarted_ = true;
      wifi_->connectTo(pendingSsid_.c_str(), pendingPassword_.c_str(), pendingHidden_);
    }
    switch (wifi_->state()) {
      case WifiState::Connected:
      case WifiState::ConnectedNoInternet:
      case WifiState::CaptivePortalSuspected:
        phase_ = Phase::Success;
        successMs_ = 0;
        break;
      case WifiState::AuthenticationFailed:
        lastError_ = "Wrong password (authentication failed).";
        phase_ = Phase::Failed;
        break;
      case WifiState::NetworkNotFound:
        lastError_ = "Network not found — is it in range?";
        phase_ = Phase::Failed;
        break;
      case WifiState::Error:
        lastError_ = "Connection failed.";
        phase_ = Phase::Failed;
        break;
      default:
        break;  // still connecting
    }
  }

  if (phase_ == Phase::Success) {
    successMs_ += deltaMs;
    if (successMs_ > kSuccessLingerMs) {
      teardown();  // spec §22: shut the AP down after success
    }
  }
}
