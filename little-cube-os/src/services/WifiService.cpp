#include "WifiService.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>

#include "../core/EventBus.h"
#include "../core/SystemState.h"

namespace {

Preferences wifiPrefs;
constexpr const char* kPrefNamespace = "lc-wifi";
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kReconnectDelayMs = 10000;
constexpr const char* kProbeUrl = "http://connectivitycheck.gstatic.com/generate_204";

WifiService* g_self = nullptr;

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

}  // namespace

const char* wifiStateName(WifiState state) {
  switch (state) {
    case WifiState::Disabled: return "disabled";
    case WifiState::Idle: return "idle";
    case WifiState::Scanning: return "scanning";
    case WifiState::NetworksFound: return "networks found";
    case WifiState::Connecting: return "connecting";
    case WifiState::Connected: return "connected";
    case WifiState::ConnectedNoInternet: return "connected, no internet";
    case WifiState::AuthenticationFailed: return "authentication failed";
    case WifiState::NetworkNotFound: return "network not found";
    case WifiState::CaptivePortalSuspected: return "captive portal suspected";
    case WifiState::Disconnected: return "disconnected";
    case WifiState::Error: return "error";
  }
  return "?";
}

// Runs on its own short-lived task; only touches the volatile result slot.
void wifiProbeTask(void* arg) {
  WifiService* self = static_cast<WifiService*>(arg);
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  int result = 2;
  if (http.begin(kProbeUrl)) {
    const int code = http.GET();
    if (code == 204) {
      result = 1;
    } else if (code >= 200 && code < 400) {
      result = 3;  // something answered, but not with the expected 204
    }
    http.end();
  }
  self->probeResult_ = result;
  vTaskDelete(nullptr);
}

namespace {
void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (g_self == nullptr) {
    return;
  }
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    g_self->noteDisconnectReason(info.wifi_sta_disconnected.reason);
  }
}
}  // namespace

void WifiService::setState(WifiState next) {
  if (state_ == next) {
    return;
  }
  state_ = next;
  Serial.printf("[wifi] %s\n", wifiStateName(state_));
  if (systemState_ != nullptr) {
    systemState_->wifi = state_;
    systemState_->internet = internet_;
    systemState_->version++;
  }
  if (events_ == nullptr) {
    return;
  }
  switch (next) {
    case WifiState::Connecting:
      events_->publish(SystemEvent::WifiConnecting);
      break;
    case WifiState::Connected:
    case WifiState::ConnectedNoInternet:
    case WifiState::CaptivePortalSuspected:
      events_->publish(SystemEvent::WifiConnected);
      break;
    case WifiState::Disconnected:
    case WifiState::AuthenticationFailed:
    case WifiState::NetworkNotFound:
      events_->publish(SystemEvent::WifiDisconnected);
      break;
    default:
      break;
  }
}

void WifiService::loadSaved() {
  savedCount_ = wifiPrefs.getUChar("count", 0);
  if (savedCount_ > kMaxSaved) {
    savedCount_ = kMaxSaved;
  }
  char key[8];
  for (uint8_t i = 0; i < savedCount_; i++) {
    snprintf(key, sizeof(key), "ssid%u", i);
    saved_[i].ssid = wifiPrefs.getString(key, "");
    snprintf(key, sizeof(key), "pass%u", i);
    saved_[i].pass = wifiPrefs.getString(key, "");
  }
  offline_ = wifiPrefs.getBool("offline", false);
}

void WifiService::persistSaved() {
  char key[8];
  // Drop every slot above the live count FIRST. Writing only 0..count-1 left
  // a forgotten network's ssidN/passN in the namespace, so `wifi forget` kept
  // the plaintext PSK readable in NVS forever.
  for (uint8_t i = savedCount_; i < kMaxSaved; i++) {
    snprintf(key, sizeof(key), "ssid%u", i);
    wifiPrefs.remove(key);
    snprintf(key, sizeof(key), "pass%u", i);
    wifiPrefs.remove(key);
  }
  for (uint8_t i = 0; i < savedCount_; i++) {
    snprintf(key, sizeof(key), "ssid%u", i);
    wifiPrefs.putString(key, saved_[i].ssid);
    snprintf(key, sizeof(key), "pass%u", i);
    wifiPrefs.putString(key, saved_[i].pass);
  }
  // Count last: a power cut mid-write then claims fewer entries than exist,
  // never more.
  wifiPrefs.putUChar("count", savedCount_);
}

bool WifiService::isSaved(const char* ssid) const {
  for (uint8_t i = 0; i < savedCount_; i++) {
    if (saved_[i].ssid.equals(ssid)) {
      return true;
    }
  }
  return false;
}

void WifiService::rememberNetwork(const char* ssid, const char* password) {
  for (uint8_t i = 0; i < savedCount_; i++) {
    if (saved_[i].ssid.equals(ssid)) {
      saved_[i].pass = password;
      persistSaved();
      return;
    }
  }
  if (savedCount_ >= kMaxSaved) {
    // Drop the oldest entry to make room.
    for (uint8_t i = 0; i + 1 < kMaxSaved; i++) {
      saved_[i] = saved_[i + 1];
    }
    savedCount_ = kMaxSaved - 1;
  }
  saved_[savedCount_].ssid = ssid;
  saved_[savedCount_].pass = password;
  savedCount_++;
  persistSaved();
  Serial.printf("[wifi] saved network %s\n", ssid);  // never the password
}

bool WifiService::forget(const char* ssid) {
  for (uint8_t i = 0; i < savedCount_; i++) {
    if (saved_[i].ssid.equals(ssid)) {
      for (uint8_t j = i; j + 1 < savedCount_; j++) {
        saved_[j] = saved_[j + 1];
      }
      savedCount_--;
      saved_[savedCount_].ssid = "";
      saved_[savedCount_].pass = "";
      persistSaved();
      return true;
    }
  }
  return false;
}

void WifiService::begin(EventBus* events, SystemState* state) {
  events_ = events;
  systemState_ = state;
  g_self = this;

  wifiPrefs.begin(kPrefNamespace, false);
  loadSaved();

  WiFi.onEvent(onWifiEvent);
  WiFi.persistent(false);  // we manage credentials ourselves, in NVS
  WiFi.setAutoReconnect(false);

  if (offline_) {
    WiFi.mode(WIFI_OFF);
    setState(WifiState::Disabled);
    return;
  }
  WiFi.mode(WIFI_STA);
  setState(WifiState::Idle);

  if (savedCount_ > 0) {
    autoIndex_ = 0;
    tryNextSaved();
  }
}

bool WifiService::tryNextSaved() {
  if (autoIndex_ < 0 || autoIndex_ >= static_cast<int8_t>(savedCount_)) {
    autoIndex_ = -1;
    return false;
  }
  const SavedNet& net = saved_[autoIndex_];
  Serial.printf("[wifi] auto-connecting to %s\n", net.ssid.c_str());
  pendingSsid_ = net.ssid;
  pendingPass_ = net.pass;
  pendingSave_ = false;  // already saved
  pendingHidden_ = false;
  lastDisconnectReason_ = 0;
  WiFi.begin(net.ssid.c_str(), net.pass.c_str());
  connectStartMs_ = millis();
  setState(WifiState::Connecting);
  autoIndex_++;
  return true;
}

bool WifiService::connectTo(const char* ssid, const char* password, bool hidden) {
  if (offline_) {
    Serial.println("[wifi] offline mode is on — `wifi offline off` first");
    return false;
  }
  if (ssid == nullptr || ssid[0] == '\0') {
    return false;
  }
  // enableSTA ORs the STA bit in; WiFi.mode(WIFI_STA) replaced the mode and
  // silently killed the setup portal's SoftAP mid-connect.
  WiFi.enableSTA(true);
  // WiFi.begin() has no hidden-network flag (its 5th argument is tryConnect,
  // and wifi_sta_config_t has no such field either). A beacon-suppressed AP
  // is only reliably found by a full all-channel scan, so that is the lever.
  WiFi.setScanMethod(hidden ? WIFI_ALL_CHANNEL_SCAN : WIFI_FAST_SCAN);
  autoIndex_ = -1;  // manual connect overrides the boot sequence
  pendingSsid_ = ssid;
  pendingPass_ = password != nullptr ? password : "";
  pendingHidden_ = hidden;
  pendingSave_ = true;
  lastDisconnectReason_ = 0;
  internet_ = false;
  WiFi.disconnect(false, true);
  WiFi.begin(pendingSsid_.c_str(), pendingPass_.c_str(), 0, nullptr, true);
  connectStartMs_ = millis();
  setState(WifiState::Connecting);
  return true;
}

void WifiService::wipePending() {
  // Spec §19: clear temporary credential buffers after use. Overwrite the
  // String storage before releasing it. The SSID goes too — on its own it
  // identifies which PSK the durable copy belongs to.
  for (size_t i = 0; i < pendingPass_.length(); i++) {
    pendingPass_.setCharAt(i, '\0');
  }
  pendingPass_ = "";
  for (size_t i = 0; i < pendingSsid_.length(); i++) {
    pendingSsid_.setCharAt(i, '\0');
  }
  pendingSsid_ = "";
}

void WifiService::onConnected() {
  if (pendingSave_ && pendingSsid_.length() > 0) {
    rememberNetwork(pendingSsid_.c_str(), pendingPass_.c_str());
  }
  wipePending();
  autoIndex_ = -1;
  Serial.printf("[wifi] connected to %s (%s)\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str());
  setState(WifiState::Connected);
  startProbe();
}

void WifiService::startProbe() {
  if (probeRunning_) {
    return;
  }
  probeResult_ = 0;
  probeRunning_ = true;
  if (xTaskCreate(wifiProbeTask, "wifiprobe", 8192, this, 1, nullptr) != pdPASS) {
    // Leaving probeRunning_ set would block every future probe, and with it
    // internet_ — which gates SNTP and the weather refresh — for the whole boot.
    probeRunning_ = false;
    Serial.println("[wifi] probe task could not start (low memory)");
  }
}

void WifiService::startScan(bool printWhenDone) {
  if (offline_) {
    Serial.println("[wifi] offline mode is on");
    return;
  }
  if (scanPending_) {
    return;  // one scan at a time; a second scanNetworks() would be refused
  }
  // enableSTA ORs the STA bit in rather than replacing the mode, so the setup
  // portal's SoftAP keeps beaconing while we scan.
  WiFi.enableSTA(true);
  scanPrint_ = printWhenDone;
  scanPending_ = true;
  WiFi.scanNetworks(true /*async*/);
  // Scanning is tracked by scanPending_, not by the state. Overwriting state_
  // here abandoned a connect in flight: the Connecting branch in update()
  // stopped running, so the network was never saved, the plaintext PSK stayed
  // in RAM, and the connectivity probe never started.
  const bool busy = state_ == WifiState::Connecting || state_ == WifiState::Connected ||
                    state_ == WifiState::ConnectedNoInternet ||
                    state_ == WifiState::CaptivePortalSuspected;
  if (!busy) {
    setState(WifiState::Scanning);
  }
  if (events_ != nullptr) {
    events_->publish(SystemEvent::WifiScanStarted);
  }
}

void WifiService::disconnect() {
  WiFi.disconnect(false, true);
  internet_ = false;
  autoIndex_ = -1;
  setState(WifiState::Disconnected);
}

void WifiService::setOfflineMode(bool offline) {
  offline_ = offline;
  wifiPrefs.putBool("offline", offline);
  if (offline) {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    internet_ = false;
    setState(WifiState::Disabled);
  } else {
    WiFi.enableSTA(true);  // OR the bit in; never tear down a running SoftAP
    setState(WifiState::Idle);
    if (savedCount_ > 0) {
      autoIndex_ = 0;
      tryNextSaved();
    }
  }
}

String WifiService::currentSsid() const {
  if (state_ == WifiState::Connected || state_ == WifiState::ConnectedNoInternet ||
      state_ == WifiState::CaptivePortalSuspected) {
    return WiFi.SSID();
  }
  return String();
}

void WifiService::update(uint32_t deltaMs) {
  // Async scan completion. scanComplete() returns WIFI_SCAN_RUNNING (-1)
  // while it works and WIFI_SCAN_FAILED (-2) if the driver refused or the
  // 60 s timeout expired — the failure used to be ignored, which left
  // scanPending_ and the Scanning state stuck forever with scanPrint_ still
  // armed, so a much later scan dumped results nobody asked for.
  if (scanPending_) {
    const int16_t n = WiFi.scanComplete();
    if (n != WIFI_SCAN_RUNNING) {
      scanPending_ = false;
      if (n < 0) {
        scanCount_ = 0;
        WiFi.scanDelete();
        if (scanPrint_) {
          scanPrint_ = false;
          Serial.println("scan failed");
        }
      } else {
        scanCount_ = 0;
        for (int16_t i = 0; i < n && scanCount_ < kMaxScanResults; i++) {
          ScanResult& r = results_[scanCount_];
          strncpy(r.ssid, WiFi.SSID(i).c_str(), sizeof(r.ssid) - 1);
          r.ssid[sizeof(r.ssid) - 1] = '\0';
          r.rssi = WiFi.RSSI(i);
          r.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
          r.saved = isSaved(r.ssid);
          scanCount_++;
        }
        WiFi.scanDelete();
        if (events_ != nullptr) {
          events_->publish(SystemEvent::WifiScanCompleted);
        }
        if (scanPrint_) {
          scanPrint_ = false;
          if (scanCount_ == 0) {
            Serial.println("no networks found");
          }
          for (uint8_t i = 0; i < scanCount_; i++) {
            Serial.printf("%2u. %-24s %4d dBm %s%s\n", (unsigned)(i + 1), results_[i].ssid,
                          (int)results_[i].rssi, results_[i].secure ? "locked" : "open",
                          results_[i].saved ? " · saved" : "");
          }
        }
      }
      // Only the Scanning state is ours to leave; a connect in flight or a
      // live connection kept its own state throughout and must not be
      // clobbered here.
      if (state_ == WifiState::Scanning) {
        if (WiFi.status() == WL_CONNECTED) {
          onConnected();  // full path: save, wipe, probe — never setState alone
        } else {
          setState(scanCount_ > 0 ? WifiState::NetworksFound : WifiState::Idle);
        }
      }
    }
  }

  // Connect progress / timeout.
  if (state_ == WifiState::Connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      onConnected();
    } else if (millis() - connectStartMs_ > kConnectTimeoutMs ||
               lastDisconnectReason_ != 0) {
      const uint8_t reason = lastDisconnectReason_;
      const bool authFail = reason == WIFI_REASON_AUTH_FAIL ||
                            reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                            reason == WIFI_REASON_AUTH_EXPIRE ||
                            reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
      const bool notFound = reason == WIFI_REASON_NO_AP_FOUND;
      if (millis() - connectStartMs_ <= kConnectTimeoutMs && !authFail && !notFound) {
        // Transient reason (roaming etc.) — keep waiting until timeout.
      } else {
        WiFi.disconnect(false, true);
        wipePending();
        if (authFail) {
          setState(WifiState::AuthenticationFailed);
        } else if (notFound) {
          setState(WifiState::NetworkNotFound);
        } else {
          setState(WifiState::Error);
        }
        // Boot auto-connect: move on to the next saved network.
        if (autoIndex_ >= 0) {
          tryNextSaved();
        }
      }
    }
  }

  // Probe result consumption.
  if (probeRunning_ && probeResult_ != 0) {
    probeRunning_ = false;
    internet_ = probeResult_ == 1;
    if (systemState_ != nullptr) {
      systemState_->internet = internet_;
      systemState_->version++;
    }
    if (probeResult_ == 1) {
      Serial.println("[wifi] internet access confirmed");
      setState(WifiState::Connected);
    } else if (probeResult_ == 3) {
      Serial.println("[wifi] connected, but a captive portal seems to intercept traffic");
      setState(WifiState::CaptivePortalSuspected);
    } else {
      setState(WifiState::ConnectedNoInternet);
    }
    if (events_ != nullptr) {
      events_->publish(internet_ ? SystemEvent::InternetAvailable
                                 : SystemEvent::InternetUnavailable);
    }
  }

  // Drop detection + gentle auto-reconnect to saved networks.
  const bool connectedState = state_ == WifiState::Connected ||
                              state_ == WifiState::ConnectedNoInternet ||
                              state_ == WifiState::CaptivePortalSuspected;
  if (connectedState && WiFi.status() != WL_CONNECTED) {
    internet_ = false;
    if (systemState_ != nullptr) {
      systemState_->internet = false;
    }
    setState(WifiState::Disconnected);
    reconnectAccumMs_ = 0;
  }
  if (state_ == WifiState::Disconnected && !offline_ && savedCount_ > 0) {
    reconnectAccumMs_ += deltaMs;
    if (reconnectAccumMs_ >= kReconnectDelayMs) {
      reconnectAccumMs_ = 0;
      autoIndex_ = 0;
      tryNextSaved();
    }
  }
}
