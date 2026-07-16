#include "StorageCommands.h"

#include <Arduino.h>

#include "../../core/Services.h"
#include "../../hardware/SdCardAdapter.h"
#include "../../services/RecorderService.h"
#include "../../storage/SdStorage.h"

void printStorageHelp() {
  Serial.println("storage status                card state + capacity");
  Serial.println("storage mount                 (re)try mounting the card");
  Serial.println("storage eject                 flush and unmount for safe removal");
  Serial.println("storage usage                 capacity summary");
  Serial.println("storage index                 rebuild the /littlecube tree");
  Serial.println("storage backup                (not implemented in v1)");
}

bool handleStorageCommand(Services& services, const char* verb, char* args) {
  (void)args;
  if (services.sdCard == nullptr) {
    return false;
  }

  if (strcmp(verb, "status") == 0 || strcmp(verb, "usage") == 0) {
    Serial.printf("state: %s\n", sdCardStateName(services.sdCard->state()));
    if (services.sdCard->mounted()) {
      const uint64_t total = services.sdCard->totalBytes();
      const uint64_t freeB = services.sdCard->freeBytes();
      Serial.printf("total: %llu MB  used: %llu MB  free: %llu MB\n",
                    (unsigned long long)(total / (1024 * 1024)),
                    (unsigned long long)((total - freeB) / (1024 * 1024)),
                    (unsigned long long)(freeB / (1024 * 1024)));
    }
    return true;
  }

  if (strcmp(verb, "mount") == 0) {
    Serial.println(services.sdCard->mount() ? "mounted" : "error: no card or mount failed");
    return true;
  }

  if (strcmp(verb, "eject") == 0) {
    if (services.recorder != nullptr && services.recorder->recording()) {
      Serial.println("error: recording active — stop it first");
      return true;
    }
    Serial.println(services.sdCard->safeEject() ? "safe to remove the card"
                                                : "error: nothing mounted");
    return true;
  }

  if (strcmp(verb, "index") == 0) {
    Serial.println(services.storage != nullptr && services.storage->ensureTree()
                       ? "tree ok"
                       : "error: card not writable");
    return true;
  }

  if (strcmp(verb, "backup") == 0) {
    Serial.println("storage backup is not implemented in v1 — see docs/product-spec.md follow-ups");
    return true;
  }

  return false;
}
