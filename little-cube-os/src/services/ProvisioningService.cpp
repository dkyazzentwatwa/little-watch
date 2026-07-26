#include "ProvisioningService.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>

#include "SettingsService.h"
#include "WifiService.h"

namespace {

WebServer* server = nullptr;
DNSServer* dns = nullptr;

constexpr uint32_t kIdleTimeoutMs = 10UL * 60UL * 1000UL;
constexpr uint32_t kSuccessLingerMs = 6000;

// AP password alphabet: no 0/O/1/l, because the user reads this off the
// AMOLED and types it into a phone. 31 symbols ^ 10 is ample for a session
// key that only exists while the portal is up.
constexpr char kApPassAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
constexpr uint8_t kApPassLen = 10;

// Everything interpolated into the page is attacker-controlled: SSIDs come
// from whatever APs are in range, and devname/tz/city are replayed from NVS
// on every later visit. An unescaped SSID could rewrite the form the user
// then types their home Wi-Fi password into.
String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    const char c = in.charAt(i);
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

// Portal fields are persisted and re-served forever, so they are filtered
// down to a plain printable whitelist rather than merely escaped — a stored
// value should be boring by construction. The POSIX TZ grammar needs
// ,.:+-/ ; device names and city names need spaces and hyphens. Accented
// city names lose their accents; Open-Meteo geocoding accepts the ASCII form.
String sanitizeField(const String& in, size_t maxLen) {
  String out;
  out.reserve(in.length() < maxLen ? in.length() : maxLen);
  for (size_t i = 0; i < in.length() && out.length() < maxLen; i++) {
    const char c = in.charAt(i);
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || strchr(" .,:+-_/", c) != nullptr;
    if (allowed) {
      out += c;
    }
  }
  out.trim();
  return out;
}

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

// The form is emitted as literal fragments with the dynamic values
// concatenated between them, instead of the previous four sequential
// String::replace() calls on %OPTIONS%/%DEVNAME%/%TZ%/%CITY%. Each replace()
// re-scans text an earlier one injected, so an SSID (or a saved city)
// containing the literal "%CITY%" would itself be substituted. Neutralising
// '%' in the escaper would also work, but single-pass concatenation removes
// the whole class of bug instead of relying on an extra escape rule — and it
// avoids four full scans of a multi-kilobyte String.
const char kFormPre[] PROGMEM = R"HTML(
<form method="POST" action="/save">
<label>Network</label><select name="pick" onchange="document.getElementById('ssid').value=this.value">
)HTML";

const char kFormAfterOptions[] PROGMEM = R"HTML(</select>
<label>SSID</label><input id="ssid" name="ssid" maxlength="32" required>
<label>Password</label><input id="pw" name="password" type="password" maxlength="63">
<div class="row"><input type="checkbox" onclick="document.getElementById('pw').type=this.checked?'text':'password'">show password</div>
<div class="row"><input type="checkbox" name="hidden" value="1">hidden network</div>
<label>Device name</label><input name="devname" value=")HTML";

const char kFormAfterDevName[] PROGMEM = R"HTML(" maxlength="24">
<label>Timezone (POSIX)</label><input name="tz" value=")HTML";

const char kFormAfterTz[] PROGMEM = R"HTML(" maxlength="48">
<small>e.g. PST8PDT,M3.2.0,M11.1.0 · EST5EDT,M3.2.0,M11.1.0 · UTC0</small>
<label>Weather city</label><input name="city" value=")HTML";

const char kFormAfterCity[] PROGMEM = R"HTML(" maxlength="40">
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

  // Fresh random password per portal session. The old scheme derived it from
  // mac[4]/mac[5] — the exact bytes the SSID broadcasts — so anyone in range
  // could recover it with two XORs. Delivery is the AMOLED (SettingsApp).
  char pass[kApPassLen + 1];
  for (uint8_t i = 0; i < kApPassLen; i++) {
    pass[i] = kApPassAlphabet[esp_random() % (sizeof(kApPassAlphabet) - 1)];
  }
  pass[kApPassLen] = '\0';
  apPassword_ = pass;

  WiFi.mode(WIFI_AP_STA);  // STA stays available for the connection test
  if (!WiFi.softAP(apName_.c_str(), apPassword_.c_str())) {
    Serial.println("[setup] softAP failed");
    WiFi.enableAP(false);  // drop the AP half only; never force the radio on
    apPassword_ = "";
    apName_ = "";
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
  // The password is deliberately absent here: it is shown on the cube's
  // screen only, so a serial log (or a shared terminal recording) never
  // carries it.
  Serial.printf("[setup] portal up: join %s (password is on the cube screen) "
                "then open http://%s\n",
                apName_.c_str(), portalIp_.c_str());
  return true;
}

void ProvisioningService::handleRoot() {
  idleMs_ = 0;
  String options = "<option value=\"\">choose...</option>";
  uint8_t count = 0;
  const WifiService::ScanResult* results = wifi_->scanResults(count);
  for (uint8_t i = 0; i < count; i++) {
    const String ssid = htmlEscape(results[i].ssid);
    options += "<option value=\"";
    options += ssid;
    options += "\">";
    options += ssid;
    options += results[i].secure ? " (locked)" : " (open)";
    options += "</option>";
  }
  if (count == 0) {
    wifi_->startScan(false);  // list fills in on refresh
    options += "<option value=\"\">scanning... refresh shortly</option>";
  }

  String page = FPSTR(kPortalHead);
  page += FPSTR(kFormPre);
  page += options;
  page += FPSTR(kFormAfterOptions);
  page += htmlEscape(settings_->deviceName());
  page += FPSTR(kFormAfterDevName);
  page += htmlEscape(settings_->timezone());
  page += FPSTR(kFormAfterTz);
  page += htmlEscape(settings_->weatherCity());
  page += FPSTR(kFormAfterCity);
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
    // Clamp and filter before persisting: these land in NVS and are replayed
    // into the form on every later portal visit, so a bad value would be
    // stored XSS, not a one-shot reflection.
    const String devname = sanitizeField(server->arg("devname"), 24);
    if (devname.length() > 0) {
      settings_->setDeviceName(devname);
    }
    const String tz = sanitizeField(server->arg("tz"), 48);
    if (tz.length() > 0) {
      settings_->setTimezone(tz);
      // KNOWN GAP: a timezone change should publish SystemEvent::SettingsChanged
      // here so TimeService::onTimezoneChanged() re-applies it immediately (that
      // is what the `settings set timezone` serial verb does). This service has
      // no EventBus: begin() takes only WifiService* and SettingsService*, and
      // neither exposes one. Adding it means an EventBus* parameter on
      // ProvisioningService::begin() plus the matching wiring in Kernel.cpp.
      // Until then the new zone lands via one of the two fallbacks: the portal
      // flow ends in a fresh connection, and TimeService applies the current TZ
      // when it starts SNTP, and failing that its hourly re-derive notices the
      // zone changed underneath it (so worst case is up to an hour stale).
    }
    const String city = sanitizeField(server->arg("city"), 40);
    if (city.length() > 0) {
      // Coordinates resolve on the first weather fetch (geocoding).
      settings_->setWeatherLocation(city, 0.0f, 0.0f);
    }
  }

  phase_ = Phase::Testing;
  testStarted_ = false;
  String page = FPSTR(kPortalHead);
  page += "<h3>Testing connection to \"" + htmlEscape(pendingSsid_) +
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
  // Drop only the AP half. Forcing WIFI_STA here switched the radio back on
  // when the user had offline mode enabled.
  if (wifi_ != nullptr && wifi_->offlineMode()) {
    WiFi.mode(WIFI_OFF);
  } else {
    WiFi.enableAP(false);
  }
  // Wipe the AP credential and the pending copies (WifiService keeps the
  // durable copy in NVS). apName_/apPassword_ are readable through public
  // accessors, so they must not outlive the portal either.
  for (size_t i = 0; i < pendingPassword_.length(); i++) {
    pendingPassword_.setCharAt(i, '\0');
  }
  pendingPassword_ = "";
  pendingSsid_ = "";
  for (size_t i = 0; i < apPassword_.length(); i++) {
    apPassword_.setCharAt(i, '\0');
  }
  apPassword_ = "";
  apName_ = "";
  portalIp_ = "";
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

  if (phase_ == Phase::Testing && !testStarted_) {
    testStarted_ = true;
    if (!wifi_->connectTo(pendingSsid_.c_str(), pendingPassword_.c_str(), pendingHidden_)) {
      // connectTo refuses instantly in offline mode. Without this the browser
      // would poll /status showing "Still connecting..." until the 10 minute
      // idle timeout.
      lastError_ = "Wi-Fi is switched off on the cube (offline mode).";
      phase_ = Phase::Failed;
    }
  }

  if (phase_ == Phase::Testing) {
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
