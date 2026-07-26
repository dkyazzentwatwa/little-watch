#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

// Calculator (spec §27): four operations, decimal point, percent, clear,
// backspace. No scientific functions in v1, no services — the whole app is
// a fixed keypad over a fixed entry buffer, so it works with no card, no
// clock and no network.
//
// The entry is a fixed char array rather than a String: a calculator takes
// unbounded taps, and an entry that can grow is an entry that eventually
// eats the heap. Digits past the cap are simply refused.
class CalculatorApp : public App {
 public:
  explicit CalculatorApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override;
  void onResume() override;

  void update(uint32_t deltaMs) override { (void)deltaMs; }
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  // 4 x 5 keypad. Index = row * kCols + col; the labels live in the .cpp.
  static constexpr uint8_t kCols = 4;
  static constexpr uint8_t kRows = 5;
  static constexpr uint8_t kKeyCount = kCols * kRows;

  // 12 significant digits is the widest entry the result line can show at
  // body size; results are formatted to 10 and may be a little longer.
  static constexpr size_t kEntryDigits = 12;
  static constexpr size_t kEntryCap = 24;

  void clearAll();
  void pressKey(uint8_t index);
  void appendChar(char c);
  void backspace();
  void applyPercent();
  void setOperator(char op);
  void evaluate();
  double entryValue() const;
  void setEntryFromValue(double v);
  void fail(const char* message);

  Services& services_;
  StatusBar statusBar_;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  // Hit rects are only valid once render() has laid them out for the screen
  // the user is actually looking at; taps before that are dropped.
  bool laidOut_ = false;
  widgets::Rect keyRects_[kKeyCount];

  char entry_[kEntryCap] = "0";
  double accumulator_ = 0.0;
  char pendingOp_ = '\0';   // '\0', '+', '-', 'x', '/'
  bool entering_ = false;   // entry_ is a live entry, not a shown result
  bool error_ = false;
  char errorText_[32] = "";
};
