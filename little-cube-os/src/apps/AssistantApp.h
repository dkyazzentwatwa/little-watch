#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

// Voice assistant UI (docs/superpowers/specs/2026-07-25-voice-assistant-design.md):
// one big talk button (tap = listen, tap again = send), a live mic level while
// it listens, and the last exchange as two bubbles — question right, answer
// left, paged when the answer runs past the band. All work happens in
// AssistantService; this app only reflects its state and forwards taps.
//
// THIS SCREEN IS NOT MONOSPACE, unlike Clock/Today/Weather. Answers are prose
// that gets read, and the built-in 6x8 bitmap font is materially worse for
// that; the hacker character comes from the framing (widgets::header, the
// rules, the footer) rather than the body face.
class AssistantApp : public App {
 public:
  explicit AssistantApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override {}
  void onResume() override {
    needsLayout_ = true;
    dirty_ = true;
  }

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  // 1024-byte answers at ~20 chars/line worst case still fit inside this.
  static constexpr uint8_t kMaxPages = 12;

  // All wrapping, paging and box geometry happens here, called from update().
  // NEVER from render(): SystemState::version ticks every 60 s (pixel shift)
  // and every clock minute, so render() runs on an idle screen forever.
  void layout();

  Services& services_;
  StatusBar statusBar_;
  widgets::Rect talkRect_;

  // ASCII-folded copies of the service's strings. Two reasons they are copies:
  //   1. The Free* faces have no glyph above 0x7E, and both the model's answers
  //      and the service's own refusals ("no API key — run: assistant key")
  //      carry UTF-8 punctuation that would render as garbage.
  //   2. Page offsets index into a buffer this app owns, so they cannot be
  //      invalidated underneath us by the worker task.
  char question_[512] = "";
  char answer_[1024] = "";
  char error_[96] = "";

  // Layout products, all in UNSHIFTED canvas coordinates; the burn-in offsets
  // (spec §37) are added at draw time.
  int16_t talkY_ = 0;
  int16_t errY_ = 0;
  uint8_t errLines_ = 0;
  int16_t qX_ = 0, qY_ = 0, qW_ = 0, qH_ = 0;
  uint8_t qLines_ = 0;
  int16_t aY_ = 0, aH_ = 0;
  uint8_t aLines_ = 0;
  uint8_t aMaxLines_ = 0;
  int16_t idleY_ = 0, idleH_ = 0;
  uint8_t hintLines_ = 0;
  const char* readyLine_ = "";
  const char* readyHint_ = "";
  uint16_t readyColor_ = 0;
  bool showIdle_ = true;

  uint16_t pageOffsets_[kMaxPages] = {0};
  uint8_t pageCount_ = 0;
  uint8_t page_ = 0;

  uint8_t lastState_ = 255;
  uint32_t lastStateVersion_ = 0;
  uint32_t tickMs_ = 0;
  uint32_t meterMs_ = 0;
  uint8_t level_ = 0;  // 0..100, the drawn mic bar
  bool needsLayout_ = true;
  bool dirty_ = true;
};
