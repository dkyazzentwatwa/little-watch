#include "CalculatorApp.h"

#include <Arduino_GFX_Library.h>
#include <math.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"

namespace {

// Keypad labels, row-major. ASCII only: the built-in 6x8 GFX font has no
// reliable glyphs for the multiplication/division signs on this panel.
const char* const kKeys[] = {
    "C", "del", "%",   "/",
    "7", "8",   "9",   "x",
    "4", "5",   "6",   "-",
    "1", "2",   "3",   "+",
    "0", ".",   "+/-", "=",
};

constexpr int16_t kGap = 6;
constexpr int16_t kKeyH = 60;  // >= theme::kTouchTargetMin (48)
constexpr int16_t kKeypadBottom = DISPLAY_HEIGHT - theme::kPadding;
constexpr int16_t kKeypadTop = kKeypadBottom - (5 * kKeyH + 4 * kGap);

// Widest text size that still fits `text` across the content width.
uint8_t fitTextSize(const char* text, int16_t avail) {
  uint8_t size = theme::kTextSizeTitle;
  const int16_t len = static_cast<int16_t>(strlen(text));
  while (size > theme::kTextSizeSmall && len * 6 * size > avail) {
    size--;
  }
  return size;
}

bool isOperatorKey(const char* label) {
  return label[1] == '\0' &&
         (label[0] == '+' || label[0] == '-' || label[0] == 'x' || label[0] == '/');
}

}  // namespace

// Fresh entry: a calculator that reopens holding someone else's half-typed
// sum is a calculator that gives the wrong answer.
void CalculatorApp::onOpen() {
  clearAll();
  laidOut_ = false;
}

// Backgrounded, not finished. The expression survives so glancing at the
// clock mid-sum does not throw the work away (App.h lifecycle contract).
void CalculatorApp::onPause() {}

void CalculatorApp::onResume() {
  // Nothing to reload — the app owns all of its state — but the frame that
  // was on screen belongs to another app now.
  laidOut_ = false;
  dirty_ = true;
}

void CalculatorApp::onClose() {
  clearAll();
  laidOut_ = false;
}

void CalculatorApp::clearAll() {
  entry_[0] = '0';
  entry_[1] = '\0';
  accumulator_ = 0.0;
  pendingOp_ = '\0';
  entering_ = false;
  error_ = false;
  errorText_[0] = '\0';
  dirty_ = true;
}

void CalculatorApp::fail(const char* message) {
  strncpy(errorText_, message, sizeof(errorText_) - 1);
  errorText_[sizeof(errorText_) - 1] = '\0';
  error_ = true;
  entry_[0] = '0';
  entry_[1] = '\0';
  accumulator_ = 0.0;
  pendingOp_ = '\0';
  entering_ = false;
  dirty_ = true;
}

double CalculatorApp::entryValue() const {
  return atof(entry_);
}

void CalculatorApp::setEntryFromValue(double v) {
  // 10 significant digits keeps doubles honest without printing the binary
  // noise tail (0.1 + 0.2 reads as 0.3, not 0.30000000000000004).
  snprintf(entry_, sizeof(entry_), "%.10g", v);
  if (strcmp(entry_, "-0") == 0) {
    entry_[0] = '0';
    entry_[1] = '\0';
  }
  dirty_ = true;
}

void CalculatorApp::appendChar(char c) {
  if (error_) {
    clearAll();  // typing after an error starts the next sum
  }
  if (!entering_) {
    // Previous value was a *result*; the first digit replaces it.
    entry_[0] = '0';
    entry_[1] = '\0';
    entering_ = true;
  }

  const size_t len = strlen(entry_);
  if (c == '.') {
    if (strchr(entry_, '.') != nullptr || len + 2 > sizeof(entry_)) {
      return;
    }
    entry_[len] = '.';
    entry_[len + 1] = '\0';
    dirty_ = true;
    return;
  }

  size_t digits = 0;
  for (size_t i = 0; i < len; i++) {
    if (entry_[i] >= '0' && entry_[i] <= '9') {
      digits++;
    }
  }
  if (digits >= kEntryDigits || len + 2 > sizeof(entry_)) {
    return;  // refuse silently rather than let the entry grow past the line
  }
  if (strcmp(entry_, "0") == 0) {
    entry_[0] = c;
  } else if (strcmp(entry_, "-0") == 0) {
    entry_[1] = c;
    entry_[2] = '\0';
  } else {
    entry_[len] = c;
    entry_[len + 1] = '\0';
  }
  dirty_ = true;
}

void CalculatorApp::backspace() {
  if (error_) {
    clearAll();
    return;
  }
  // Backspacing a displayed result turns it into an editable entry.
  entering_ = true;
  const size_t len = strlen(entry_);
  if (len <= 1 || (len == 2 && entry_[0] == '-')) {
    entry_[0] = '0';
    entry_[1] = '\0';
  } else {
    entry_[len - 1] = '\0';
    if (strcmp(entry_, "-") == 0) {
      entry_[0] = '0';
      entry_[1] = '\0';
    }
  }
  dirty_ = true;
}

// Percent follows the desk-calculator convention: with + or - pending it is
// "that percent of the running total" (50 + 10 % = 55), otherwise a plain
// divide by 100.
void CalculatorApp::applyPercent() {
  if (error_) {
    return;
  }
  double v = entryValue();
  if (pendingOp_ == '+' || pendingOp_ == '-') {
    v = accumulator_ * v / 100.0;
  } else {
    v = v / 100.0;
  }
  if (!isfinite(v)) {
    fail("overflow");
    return;
  }
  setEntryFromValue(v);
  entering_ = true;
}

void CalculatorApp::setOperator(char op) {
  if (error_) {
    return;
  }
  // Chaining: 1 + 2 + shows 3 before taking the second +. Pressing two
  // operators in a row only swaps the pending one.
  if (pendingOp_ != '\0' && entering_) {
    evaluate();
    if (error_) {
      return;
    }
  }
  accumulator_ = entryValue();
  pendingOp_ = op;
  entering_ = false;
  dirty_ = true;
}

void CalculatorApp::evaluate() {
  if (error_) {
    return;
  }
  if (pendingOp_ == '\0') {
    entering_ = false;
    dirty_ = true;
    return;
  }
  const double rhs = entryValue();
  double result = accumulator_;
  switch (pendingOp_) {
    case '+': result = accumulator_ + rhs; break;
    case '-': result = accumulator_ - rhs; break;
    case 'x': result = accumulator_ * rhs; break;
    case '/':
      if (rhs == 0.0) {
        fail("cannot divide by 0");
        return;
      }
      result = accumulator_ / rhs;
      break;
    default: break;
  }
  if (!isfinite(result)) {
    fail("overflow");  // catches inf and nan alike
    return;
  }
  pendingOp_ = '\0';
  accumulator_ = result;
  setEntryFromValue(result);
  entering_ = false;
}

void CalculatorApp::pressKey(uint8_t index) {
  if (index >= kKeyCount) {
    return;
  }
  const char* label = kKeys[index];
  if (strcmp(label, "C") == 0) {
    clearAll();
  } else if (strcmp(label, "del") == 0) {
    backspace();
  } else if (strcmp(label, "%") == 0) {
    applyPercent();
  } else if (strcmp(label, "=") == 0) {
    evaluate();
  } else if (strcmp(label, "+/-") == 0) {
    if (!error_) {
      const size_t len = strlen(entry_);
      if (entry_[0] == '-') {
        memmove(entry_, entry_ + 1, len);  // includes the terminator
      } else if (strcmp(entry_, "0") != 0 && len + 2 <= sizeof(entry_)) {
        memmove(entry_ + 1, entry_, len + 1);
        entry_[0] = '-';
      }
      dirty_ = true;
    }
  } else if (isOperatorKey(label)) {
    setOperator(label[0]);
  } else {
    appendChar(label[0]);  // digits and '.'
  }
}

void CalculatorApp::render() {
  const SystemState& state = *services_.state;
  if (!dirty_ && state.version == lastStateVersion_) {
    return;
  }
  lastStateVersion_ = state.version;
  dirty_ = false;

  DisplayAdapter* display = services_.display;
  if (display == nullptr || !display->ready()) {
    return;
  }
  Arduino_GFX& gfx = *display->canvas();
  gfx.fillScreen(theme::kBg);
  statusBar_.render(gfx, state, services_.amoled->shiftX(), services_.amoled->shiftY());

  const int16_t avail = DISPLAY_WIDTH - 2 * theme::kPadding;

  // Running total + pending operator, so the machine's state is never hidden.
  char pending[40] = "";
  if (pendingOp_ != '\0') {
    char lhs[24];
    snprintf(lhs, sizeof(lhs), "%.10g", accumulator_);
    snprintf(pending, sizeof(pending), "%s %c", lhs, pendingOp_);
  }
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(DISPLAY_WIDTH - theme::kPadding -
                    static_cast<int16_t>(strlen(pending)) * 6 * theme::kTextSizeSmall,
                theme::kStatusBarHeight + 8);
  gfx.print(pending);

  // Result line, right-aligned and shrunk to fit rather than clipped.
  const char* shown = error_ ? errorText_ : entry_;
  const uint8_t size = fitTextSize(shown, avail);
  gfx.setTextSize(size);
  gfx.setTextColor(error_ ? theme::kBad : theme::kText);
  gfx.setCursor(DISPLAY_WIDTH - theme::kPadding -
                    static_cast<int16_t>(strlen(shown)) * 6 * size,
                kKeypadTop - 8 - 8 * size);
  gfx.print(shown);

  const int16_t keyW = (avail - (kCols - 1) * kGap) / kCols;
  for (uint8_t row = 0; row < kRows; row++) {
    for (uint8_t col = 0; col < kCols; col++) {
      const uint8_t i = row * kCols + col;
      const char* label = kKeys[i];
      // The live operator stays lit so a pending + is visible on the key
      // itself, not just in the header line.
      const bool emphasized =
          strcmp(label, "=") == 0 || (isOperatorKey(label) && label[0] == pendingOp_);
      keyRects_[i] = widgets::button(gfx, theme::kPadding + col * (keyW + kGap),
                                     kKeypadTop + row * (kKeyH + kGap), keyW, kKeyH, label,
                                     emphasized);
    }
  }

  laidOut_ = true;
  display->markDirty();
}

bool CalculatorApp::handleInput(const InputEvent& event) {
  switch (event.action) {
    case InputAction::Tap:
      if (!laidOut_) {
        return true;  // rects belong to a frame that was never drawn
      }
      for (uint8_t i = 0; i < kKeyCount; i++) {
        if (keyRects_[i].contains(event.x, event.y)) {
          pressKey(i);
          break;
        }
      }
      return true;
    case InputAction::Confirm:
      evaluate();
      return true;
    case InputAction::Cancel:
      clearAll();
      return true;
    default:
      // Back and Home are the router's (spec §10); swipes mean nothing here.
      return false;
  }
}
