#include "EventBus.h"

bool EventBus::subscribe(Handler handler, void* context) {
  if (handler == nullptr || count_ >= kMaxSubscribers) {
    return false;
  }
  subscribers_[count_].handler = handler;
  subscribers_[count_].context = context;
  count_++;
  return true;
}

void EventBus::publish(SystemEvent event) {
  for (uint8_t i = 0; i < count_; i++) {
    subscribers_[i].handler(event, subscribers_[i].context);
  }
}
