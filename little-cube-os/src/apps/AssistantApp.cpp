#include "AssistantApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../services/AssistantService.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"

namespace {
constexpr int16_t kTop = theme::kStatusBarHeight + 12;

// Wraps plain text into lines of at most maxChars (breaking at a space when
// one falls in the window), up to maxLines; returns the y below the last line.
int16_t printWrapped(Arduino_GFX& gfx, const char* text, int16_t x, int16_t y,
                     uint8_t maxChars, uint8_t maxLines, int16_t lineH) {
  const size_t len = strlen(text);
  size_t pos = 0;
  uint8_t lines = 0;
  while (pos < len && lines < maxLines) {
    size_t take = len - pos;
    if (take > maxChars) {
      take = maxChars;
      for (size_t i = take; i > maxChars / 2; i--) {
        if (text[pos + i] == ' ') {
          take = i;
          break;
        }
      }
    }
    char line[64];
    const size_t n = take < sizeof(line) - 1 ? take : sizeof(line) - 1;
    memcpy(line, text + pos, n);
    line[n] = '\0';
    gfx.setCursor(x, y);
    gfx.print(line);
    pos += take;
    while (text[pos] == ' ') {
      pos++;
    }
    y += lineH;
    lines++;
  }
  return y;
}
}  // namespace

void AssistantApp::onOpen() {
  lastState_ = 255;
  dirty_ = true;
}

void AssistantApp::onClose() {
  // Spec: conversation memory lives only while the app is open.
  if (services_.assistant != nullptr) {
    services_.assistant->resetHistory();
  }
}

void AssistantApp::update(uint32_t deltaMs) {
  AssistantService* ai = services_.assistant;
  if (ai == nullptr) {
    return;
  }
  const uint8_t s = static_cast<uint8_t>(ai->state());
  if (s != lastState_) {
    lastState_ = s;
    dirty_ = true;
  }
  // Elapsed-seconds readout while listening; 2 Hz is plenty.
  if (ai->state() == AssistantService::State::Listening) {
    tickMs_ += deltaMs;
    if (tickMs_ >= 500) {
      tickMs_ = 0;
      dirty_ = true;
    }
  }
}

void AssistantApp::render() {
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
  statusBar_.render(gfx, state, services_.amoled->shiftX(),
                    services_.amoled->shiftY());

  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Assistant");

  AssistantService* ai = services_.assistant;
  if (ai == nullptr) {
    display->markDirty();
    return;
  }

  // The talk button is the single control: tap = listen, tap again = send.
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  talkRect_ = {theme::kPadding, kTop + 40, w, 72};
  const AssistantService::State s = ai->state();
  const bool listening = s == AssistantService::State::Listening;
  const bool working = s == AssistantService::State::Transcribing ||
                       s == AssistantService::State::Thinking;
  uint16_t fill = theme::kPanel;
  char labelBuf[24];
  const char* label = "TALK";
  if (listening) {
    fill = theme::kBad;
    snprintf(labelBuf, sizeof(labelBuf), "SEND  %02lus",
             (unsigned long)(ai->listenElapsedMs() / 1000));
    label = labelBuf;
  } else if (working) {
    fill = theme::kPanelAlt;
    label = s == AssistantService::State::Transcribing ? "hearing..." : "thinking...";
  } else if (s == AssistantService::State::Speaking) {
    fill = theme::kAccent;
    label = "SPEAKING";
  }
  gfx.fillRoundRect(talkRect_.x, talkRect_.y, talkRect_.w, talkRect_.h,
                    theme::kCardRadius, fill);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(talkRect_.x + 24, talkRect_.y + 26);
  gfx.print(label);

  // Last exchange below the button; error text replaces nothing — it stacks
  // above so the failed question stays visible.
  int16_t y = talkRect_.y + talkRect_.h + 20;
  gfx.setTextSize(theme::kTextSizeSmall);
  if (s == AssistantService::State::Error && ai->lastError()[0] != '\0') {
    gfx.setTextColor(theme::kBad);
    y = printWrapped(gfx, ai->lastError(), theme::kPadding, y, 28, 3, 20);
    y += 8;
  }
  if (ai->lastTranscript()[0] != '\0') {
    gfx.setTextColor(theme::kTextDim);
    y = printWrapped(gfx, ai->lastTranscript(), theme::kPadding, y, 28, 3, 20);
    y += 10;
  }
  if (ai->lastAnswer()[0] != '\0') {
    gfx.setTextColor(theme::kText);
    printWrapped(gfx, ai->lastAnswer(), theme::kPadding, y, 28, 11, 20);
  }
  if (ai->historyDepth() > 0) {
    gfx.setTextColor(theme::kTextDim);
    gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
    char foot[40];
    snprintf(foot, sizeof(foot), "%u exchange%s in memory", ai->historyDepth(),
             ai->historyDepth() == 1 ? "" : "s");
    gfx.print(foot);
  }
  display->markDirty();
}

bool AssistantApp::handleInput(const InputEvent& event) {
  AssistantService* ai = services_.assistant;
  if (ai == nullptr) {
    return false;
  }
  if (event.action == InputAction::Tap && talkRect_.contains(event.x, event.y)) {
    if (ai->state() == AssistantService::State::Listening) {
      ai->finishListening();
    } else {
      ai->startListening();  // refusals surface inline via the Error state
    }
    dirty_ = true;
    return true;
  }
  if (event.action == InputAction::Back &&
      ai->state() == AssistantService::State::Listening) {
    ai->cancelListening();  // drop the take; Back never sends by accident
    dirty_ = true;
    return true;
  }
  return false;
}
