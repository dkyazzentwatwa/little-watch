#include "AppRouter.h"

#include <string.h>  // memmove, for the oldest-first stack eviction

const char* appName(AppId id) {
  switch (id) {
    case AppId::Home: return "Home";
    case AppId::Today: return "Today";
    case AppId::Clock: return "Clock";
    case AppId::Weather: return "Weather";
    case AppId::Calendar: return "Calendar";
    case AppId::Notes: return "Notes";
    case AppId::Recorder: return "Recorder";
    case AppId::Audio: return "Audio";
    case AppId::Files: return "Files";
    case AppId::Contacts: return "Contacts";
    case AppId::Calculator: return "Calculator";
    case AppId::Settings: return "Settings";
    case AppId::Reader: return "Reader";
    case AppId::News: return "News";
    case AppId::Assistant: return "Assistant";
  }
  return "?";
}

const char* inputActionName(InputAction action) {
  switch (action) {
    case InputAction::None: return "None";
    case InputAction::Tap: return "Tap";
    case InputAction::DoubleTap: return "DoubleTap";
    case InputAction::LongPress: return "LongPress";
    case InputAction::SwipeLeft: return "SwipeLeft";
    case InputAction::SwipeRight: return "SwipeRight";
    case InputAction::SwipeUp: return "SwipeUp";
    case InputAction::SwipeDown: return "SwipeDown";
    case InputAction::Back: return "Back";
    case InputAction::Home: return "Home";
    case InputAction::Confirm: return "Confirm";
    case InputAction::Cancel: return "Cancel";
  }
  return "?";
}

void AppRouter::registerApp(AppId id, App* app) {
  if (app == nullptr || count_ >= kAppCount) {
    return;
  }
  entries_[count_].id = id;
  entries_[count_].app = app;
  count_++;
}

App* AppRouter::find(AppId id) {
  for (uint8_t i = 0; i < count_; i++) {
    if (entries_[i].id == id) {
      return entries_[i].app;
    }
  }
  return nullptr;
}

void AppRouter::begin(AppId initial) {
  currentId_ = initial;
  depth_ = 0;
  started_ = true;
  if (App* app = find(currentId_)) {
    app->onOpen();
  }
}

bool AppRouter::stackContains(AppId id, uint8_t upTo) const {
  const uint8_t limit = upTo > kMaxStack ? kMaxStack : upTo;
  for (uint8_t i = 0; i < limit; i++) {
    if (stack_[i] == id) {
      return true;
    }
  }
  return false;
}

// Popping top-down and skipping ids that recur deeper means a duplicated app
// (Home -> Notes -> Settings -> Notes) gets a single onClose() rather than one
// per stack slot. currentId_ is skipped too: the caller has already closed the
// outgoing foreground app.
void AppRouter::clearStack(AppId keep) {
  while (depth_ > 0) {
    const AppId id = stack_[--depth_];
    if (id == keep || id == currentId_ || stackContains(id, depth_)) {
      continue;
    }
    if (App* app = find(id)) {
      app->onClose();
    }
  }
}

void AppRouter::open(AppId id) {
  if (!started_ || id == currentId_) {
    return;
  }
  App* prev = find(currentId_);

  // Home is the stack root; opening it unwinds history instead of nesting.
  // Everything it discards has to be closed on the way out — a paused app that
  // is dropped without onClose() keeps whatever it was holding (a provisioning
  // AP, a note body) until reboot.
  if (id == AppId::Home) {
    if (prev != nullptr) {
      prev->onClose();
    }
    clearStack(id);
  } else {
    // Paused, not closed: this app stays on the stack and back() revives it
    // with onResume().
    if (prev != nullptr) {
      prev->onPause();
    }
    AppId evicted = AppId::Home;
    bool didEvict = false;
    if (depth_ == kMaxStack) {
      // Stack full. Drop the OLDEST entry, never the immediate parent —
      // skipping the push would leave back() permanently one level out of
      // step with what the user sees.
      evicted = stack_[0];
      memmove(stack_, stack_ + 1, sizeof(AppId) * (kMaxStack - 1));
      depth_ = kMaxStack - 1;
      didEvict = true;
    }
    stack_[depth_++] = currentId_;
    if (didEvict && evicted != id && !stackContains(evicted, depth_)) {
      if (App* app = find(evicted)) {
        app->onClose();
      }
    }
  }

  currentId_ = id;
  if (App* next = find(currentId_)) {
    next->onOpen();
  }
}

void AppRouter::back() {
  if (!started_) {
    return;
  }
  if (depth_ == 0) {
    home();
    return;
  }
  const AppId target = stack_[--depth_];
  if (App* prev = find(currentId_)) {
    prev->onClose();
  }
  currentId_ = target;
  if (App* next = find(currentId_)) {
    next->onResume();
  }
}

void AppRouter::home() {
  if (!started_ || currentId_ == AppId::Home) {
    return;
  }
  if (App* prev = find(currentId_)) {
    prev->onClose();
  }
  clearStack(AppId::Home);
  currentId_ = AppId::Home;
  if (App* next = find(currentId_)) {
    next->onOpen();
  }
}

void AppRouter::update(uint32_t deltaMs) {
  if (App* app = current()) {
    app->update(deltaMs);
  }
}

void AppRouter::render() {
  if (App* app = current()) {
    app->render();
  }
}

void AppRouter::handleInput(const InputEvent& event) {
  App* app = current();
  if (app != nullptr && app->handleInput(event)) {
    return;
  }
  if (event.action == InputAction::Back) {
    back();
  } else if (event.action == InputAction::Home) {
    home();
  }
}
