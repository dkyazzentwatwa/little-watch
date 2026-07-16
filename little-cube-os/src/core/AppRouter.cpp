#include "AppRouter.h"

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

void AppRouter::open(AppId id) {
  if (!started_ || id == currentId_) {
    return;
  }
  if (App* prev = find(currentId_)) {
    prev->onClose();
  }
  // Home is the stack root; opening it clears history instead of nesting.
  if (id == AppId::Home) {
    depth_ = 0;
  } else if (depth_ < kMaxStack) {
    stack_[depth_++] = currentId_;
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
  depth_ = 0;
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
