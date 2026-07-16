#include "AmoledProtection.h"

// TODO(task-17): idle timeout -> dim -> off, wake on activity, bedtime
// window, +/-2px shift cycle on a slow timer, always-on override.

void AmoledProtection::begin(DisplayAdapter* display, SettingsService* settings) {
  display_ = display;
  settings_ = settings;
}

void AmoledProtection::update(uint32_t deltaMs) {
  idleMs_ += deltaMs;
}

void AmoledProtection::onActivity() {
  idleMs_ = 0;
}
