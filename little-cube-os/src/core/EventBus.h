#pragma once

#include <Arduino.h>

#include "SystemEvent.h"

// Small synchronous publish/subscribe hub. Handlers must be fast and
// non-blocking — they run inline on the publisher's call.
class EventBus {
 public:
  using Handler = void (*)(SystemEvent event, void* context);

  bool subscribe(Handler handler, void* context);
  void publish(SystemEvent event);

 private:
  static constexpr uint8_t kMaxSubscribers = 16;
  struct Subscriber {
    Handler handler = nullptr;
    void* context = nullptr;
  };
  Subscriber subscribers_[kMaxSubscribers];
  uint8_t count_ = 0;
};
