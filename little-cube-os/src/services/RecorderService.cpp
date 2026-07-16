#include "RecorderService.h"

// TODO(task-15): preflight, timestamped naming, .partial handling, low-space
// stop, card-removal recovery metadata, EventBus notifications.

void RecorderService::begin(AudioAdapter* audio, SdCardAdapter* card, SdStorage* storage,
                            EventBus* events, SystemState* state) {
  audio_ = audio;
  card_ = card;
  storage_ = storage;
  events_ = events;
  systemState_ = state;
}

void RecorderService::update(uint32_t deltaMs) {
  if (recording_) {
    elapsedMs_ += deltaMs;
  }
}

bool RecorderService::start() {
  return false;
}

bool RecorderService::stop() {
  recording_ = false;
  return false;
}

uint32_t RecorderService::estimatedRemainingSec() const {
  return 0;
}
