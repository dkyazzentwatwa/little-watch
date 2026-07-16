#include "SerialCommandService.h"

#include "../board_config.h"
#include "../core/AppRouter.h"

// TODO(task-10): full dispatcher (help/status/version/uptime/reboot +
// notes/files/storage families), MultilineBuffer integration, then the
// remaining families in task-18. The bounded, non-blocking line reader
// below is final: overflow discards to the next newline instead of growing.

void SerialCommandService::begin(Services* services) {
  services_ = services;
  lineLen_ = 0;
  overflowed_ = false;
}

void SerialCommandService::update() {
  while (Serial.available() > 0) {
    const int raw = Serial.read();
    if (raw < 0) {
      break;
    }
    const char c = static_cast<char>(raw);
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (overflowed_) {
        Serial.println("error: line too long (max 255 chars); ignored");
      } else if (lineLen_ > 0) {
        line_[lineLen_] = '\0';
        handleLine(line_);
      }
      lineLen_ = 0;
      overflowed_ = false;
      continue;
    }
    if (lineLen_ >= kMaxLineLen - 1) {
      overflowed_ = true;
      continue;
    }
    line_[lineLen_++] = c;
  }
}

void SerialCommandService::handleLine(char* line) {
  if (strcmp(line, "version") == 0) {
    Serial.printf("%s %s\n", FIRMWARE_NAME, FIRMWARE_VERSION);
    return;
  }

  // Router debug command: `open <app>` / `open home` — exercises the app
  // router from the serial console before touch UI exists on hardware.
  if (strncmp(line, "open ", 5) == 0 && services_ != nullptr && services_->router != nullptr) {
    const char* name = line + 5;
    for (uint8_t i = 0; i < kAppCount; i++) {
      const AppId id = static_cast<AppId>(i);
      if (strcasecmp(name, appName(id)) == 0) {
        services_->router->open(id);
        Serial.printf("opened %s\n", appName(id));
        return;
      }
    }
    Serial.printf("error: unknown app '%s' (try: Home, Today, Clock, Weather, Calendar, Notes, "
                  "Recorder, Audio, Files, Contacts, Calculator, Settings)\n",
                  name);
    return;
  }
  if (strcmp(line, "back") == 0 && services_ != nullptr && services_->router != nullptr) {
    services_->router->back();
    Serial.printf("now: %s\n", appName(services_->router->currentId()));
    return;
  }

  Serial.println("littlecube: command interface lands in task 10; try 'version', 'open <app>', 'back'");
}
