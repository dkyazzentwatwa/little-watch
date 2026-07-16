#include "ProvisioningService.h"

// TODO(task-13): SoftAP + DNSServer(53, "*") + WebServer routes / and
// /save; masked secrets; inactivity timeout; teardown after success.

void ProvisioningService::begin(WifiService* wifi, SettingsService* settings) {
  wifi_ = wifi;
  settings_ = settings;
}

void ProvisioningService::update(uint32_t deltaMs) {
  if (active_) {
    idleMs_ += deltaMs;
  }
}

bool ProvisioningService::start() {
  return false;
}

void ProvisioningService::stop() {
  active_ = false;
}
