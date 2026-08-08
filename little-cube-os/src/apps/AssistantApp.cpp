#include "AssistantApp.h"

#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/SdCardAdapter.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../services/AssistantService.h"
#include "../services/SettingsService.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"
#include "../ui/widgets/Widgets.h"

namespace {

using widgets::TextStyle;

// --- geometry, all unshifted ------------------------------------------------
constexpr int16_t kContentLeft = theme::kPadding;                   // 12
constexpr int16_t kContentRight = DISPLAY_WIDTH - theme::kPadding;  // 356
constexpr int16_t kContentW = kContentRight - kContentLeft;         // 344

// widgets::footer() puts its rule at DISPLAY_HEIGHT - 44 and hands back a
// budget 12 px above it. Nothing this screen draws may cross that line.
constexpr int16_t kBodyBottom = DISPLAY_HEIGHT - 44 - 12;  // 392
static_assert(kBodyBottom == 392, "footer band moved; recheck the answer budget");

// The talk button. 56 px clears theme::kTouchTargetMin (48) with room for the
// level meter to live INSIDE its bottom edge — which is why the meter costs
// this layout no vertical space at all.
constexpr int16_t kTalkH = 56;
constexpr int16_t kMeterH = 8;
constexpr int16_t kMeterInsetX = 16;
constexpr int16_t kMeterBottomGap = 8;
constexpr uint32_t kAnimationFrameMs = 70;

constexpr int16_t kBubblePadX = 12;
constexpr int16_t kBubblePadY = 8;
constexpr int16_t kGap = 10;

// The question bubble is narrower than the answer's and hangs off the right
// edge; the answer keeps a right gutter. The asymmetry is what tells the two
// blocks apart at a glance, alongside the kPanelAlt / kPanel fills.
constexpr int16_t kQuestionMaxW = (kContentW * 82) / 100;  // 282
constexpr int16_t kAnswerW = kContentW - 16;               // 328

// Copies src into dst as pure ASCII (0x20..0x7E plus '\n'). The Free* faces
// stop at 0x7E, so anything above it draws as a garbage glyph — a real bug
// found across five screens on 2026-07-31. Model answers and Whisper
// transcripts routinely carry curly quotes, en/em dashes and ellipses, and
// AssistantService's own refusals carry em dashes, so this runs on all three.
// The General Punctuation block is folded to its ASCII equivalent; anything
// else non-ASCII is dropped rather than replaced, because a stray substitute
// character reads as a rendering fault.
void foldAscii(char* dst, size_t cap, const char* src) {
  if (dst == nullptr || cap == 0) {
    return;
  }
  dst[0] = '\0';
  if (src == nullptr) {
    return;
  }
  const unsigned char* p = reinterpret_cast<const unsigned char*>(src);
  size_t o = 0;
  while (*p != '\0' && o + 1 < cap) {
    const unsigned char c = *p;
    if (c == '\n') {
      dst[o++] = '\n';
      p++;
    } else if (c == '\t') {
      dst[o++] = ' ';
      p++;
    } else if (c >= 0x20 && c <= 0x7E) {
      dst[o++] = static_cast<char>(c);
      p++;
    } else if (c == 0xE2 && p[1] == 0x80 && p[2] != '\0') {
      // U+2010..U+2026: the punctuation an LLM actually emits.
      const unsigned char t = p[2];
      const char* rep = "";
      if (t == 0x98 || t == 0x99) {
        rep = "'";
      } else if (t == 0x9C || t == 0x9D) {
        rep = "\"";
      } else if (t >= 0x90 && t <= 0x95) {
        rep = "-";
      } else if (t == 0xA6) {
        rep = "...";
      }
      for (const char* r = rep; *r != '\0' && o + 1 < cap; r++) {
        dst[o++] = *r;
      }
      p += 3;
    } else {
      p++;  // unrenderable: drop it
    }
  }
  dst[o] = '\0';
}

// Lines a widgets::textBlock() of this width/style would draw, capped. Walks
// one line at a time through widgets::measureBlock — the SAME walker
// textBlock draws with, so the count can never disagree with the drawing.
uint8_t countLines(Arduino_GFX& gfx, const char* s, int16_t w, TextStyle style,
                   uint8_t cap) {
  if (s == nullptr || s[0] == '\0' || w <= 0) {
    return 0;
  }
  const size_t len = strlen(s);
  size_t pos = 0;
  uint8_t lines = 0;
  while (pos < len && lines < cap) {
    const size_t adv = widgets::measureBlock(gfx, s + pos, w, style, 1);
    if (adv == 0) {
      break;  // never loop on a walker that stops advancing
    }
    pos += adv;
    lines++;
  }
  return lines;
}

// widgets::textBlock clips at maxLines with no marker, so a question that runs
// past its budget just stops mid-word and reads as a rendering fault. Rewrite
// the buffer in place so the last surviving line ends in "..." — and shrink
// that line until the ellipsis fits, or it would wrap onto a line the cap then
// throws away, taking the marker with it.
void ellipsizeToLines(Arduino_GFX& gfx, char* s, size_t cap, int16_t w, TextStyle style,
                      uint8_t maxLines) {
  if (s == nullptr || s[0] == '\0' || w <= 0 || maxLines == 0 || cap < 8) {
    return;
  }
  const size_t len = strlen(s);
  size_t pos = 0;
  size_t lastLineStart = 0;
  uint8_t lines = 0;
  while (pos < len && lines < maxLines) {
    lastLineStart = pos;
    const size_t adv = widgets::measureBlock(gfx, s + pos, w, style, 1);
    if (adv == 0) {
      break;
    }
    pos += adv;
    lines++;
  }
  if (pos >= len) {
    return;  // the whole string fits; nothing to mark
  }

  char line[136];  // the walker's own line buffer is 128, plus "..." and NUL
  size_t take = pos - lastLineStart;
  if (take > sizeof(line) - 4) {
    take = sizeof(line) - 4;
  }
  while (take > 0) {
    memcpy(line, s + lastLineStart, take);
    line[take] = '\0';
    while (take > 0 && line[take - 1] == ' ') {
      take--;  // a trailing space would push the ellipsis out on its own
      line[take] = '\0';
    }
    if (take == 0) {
      break;
    }
    strcat(line, "...");  // safe: take <= sizeof(line) - 4
    if (widgets::textWidth(gfx, line, style) <= w) {
      break;
    }
    take--;
  }
  if (take == 0) {
    return;  // nothing survives alongside the marker; leave the clip unmarked
  }
  size_t end = lastLineStart + take;
  if (end + 4 > cap) {
    end = cap - 4;
  }
  s[end] = '\0';
  strcat(s, "...");
}

// 0 -> 7 -> 0 triangle wave, used for motion without floats or an animation
// buffer. It keeps the Assistant UI cheap enough to redraw from the loop.
uint8_t triangleWave(uint8_t phase) {
  phase &= 0x0F;
  return phase < 8 ? phase : 15 - phase;
}

}  // namespace

void AssistantApp::onOpen() {
  lastState_ = 255;
  page_ = 0;
  level_ = 0;
  needsLayout_ = true;
  dirty_ = true;
}

void AssistantApp::onClose() {
  // Spec: conversation memory lives only while the app is open.
  if (services_.assistant != nullptr) {
    services_.assistant->resetHistory();
  }
}

void AssistantApp::layout() {
  AssistantService* ai = services_.assistant;
  DisplayAdapter* display = services_.display;
  if (ai == nullptr || display == nullptr || !display->ready()) {
    return;
  }
  Arduino_GFX& gfx = *display->canvas();

  foldAscii(question_, sizeof(question_), ai->lastTranscript());
  foldAscii(answer_, sizeof(answer_), ai->lastAnswer());
  foldAscii(error_, sizeof(error_),
            ai->state() == AssistantService::State::Error ? ai->lastError() : "");

  // Mirrors widgets::header()'s internal arithmetic (status bar + 10, title,
  // + 10 to the rule, + 12 below it) so the button lands exactly on the
  // content-start header() returns at draw time.
  const int16_t contentTop = theme::kStatusBarHeight + 10 +
                             widgets::ascent(gfx, TextStyle::Title) + 10 + 12;
  talkY_ = contentTop;
  int16_t y = talkY_ + kTalkH + 14;

  const int16_t capH = widgets::lineHeight(TextStyle::Caption);
  const int16_t bodyH = widgets::lineHeight(TextStyle::Body);

  errLines_ = countLines(gfx, error_, kContentW, TextStyle::Caption, 3);
  errY_ = y;
  if (errLines_ > 0) {
    y += errLines_ * capH + kGap + 2;
  }

  // The echo of what was heard. It yields lines to the answer when there is
  // one; with no answer (every failure path) it gets room to show in full.
  const uint8_t qCap = answer_[0] != '\0' ? 2 : 4;
  const int16_t qMaxInner = kQuestionMaxW - 2 * kBubblePadX;
  // Safe to mutate: foldAscii() rebuilt question_ from the service above, so
  // the ellipsis never compounds across layouts.
  ellipsizeToLines(gfx, question_, sizeof(question_), qMaxInner, TextStyle::Caption, qCap);
  qLines_ = countLines(gfx, question_, qMaxInner, TextStyle::Caption, qCap);
  qY_ = y;
  qW_ = 0;
  qH_ = 0;
  if (qLines_ > 0) {
    const int16_t oneLineW = widgets::textWidth(gfx, question_, TextStyle::Caption);
    const int16_t inner = (qLines_ == 1 && oneLineW <= qMaxInner) ? oneLineW : qMaxInner;
    qW_ = inner + 2 * kBubblePadX;
    qH_ = qLines_ * capH + 2 * kBubblePadY;
    qX_ = kContentRight - qW_;
    y += qH_ + kGap;
  }

  // Answer pagination. Offsets index answer_, a buffer THIS APP owns — and it
  // is rebuilt on the service's state transitions, never by comparing
  // ai->lastAnswer(), which is a pointer into one fixed service-owned buffer
  // and is therefore byte-identical for every answer the service ever
  // produces. That comparison would fire exactly never.
  aY_ = y;
  const int16_t aInner = kAnswerW - 2 * kBubblePadX;
  const int16_t band = kBodyBottom - aY_ - 2 * kBubblePadY;
  aMaxLines_ = band >= bodyH ? static_cast<uint8_t>(band / bodyH) : 0;
  pageCount_ = 0;
  aLines_ = 0;
  aH_ = 0;
  if (answer_[0] != '\0' && aMaxLines_ > 0) {
    const size_t len = strlen(answer_);
    size_t from = 0;
    while (from < len && pageCount_ < kMaxPages) {
      pageOffsets_[pageCount_++] = static_cast<uint16_t>(from);
      const size_t adv = widgets::measureBlock(gfx, answer_ + from, aInner,
                                               TextStyle::Body, aMaxLines_);
      if (adv == 0) {
        break;
      }
      from += adv;
    }
    if (pageCount_ == 0) {
      pageOffsets_[pageCount_++] = 0;
    }
    if (page_ >= pageCount_) {
      page_ = pageCount_ - 1;
    }
    // The last page is usually short; the box hugs it instead of leaving a
    // slab of empty fill.
    aLines_ = countLines(gfx, answer_ + pageOffsets_[page_], aInner, TextStyle::Body,
                         aMaxLines_);
    aH_ = aLines_ * bodyH + 2 * kBubblePadY;
  } else {
    page_ = 0;
  }

  // The idle card. The screen must never be a title over a black void — that
  // was the complaint — so with nothing said yet it reports whether the
  // service can actually run and how to start it.
  showIdle_ = error_[0] == '\0' && question_[0] == '\0' && answer_[0] == '\0';
  idleY_ = talkY_ + kTalkH + 14;
  hintLines_ = 0;
  idleH_ = 0;
  if (showIdle_) {
    if (services_.settings == nullptr || !services_.settings->hasOpenaiKey()) {
      readyLine_ = "no API key";
      readyHint_ = "serial: assistant key -- the key is stored on the cube, not the card";
      readyColor_ = theme::kBad;
    } else if (services_.sdCard == nullptr || !services_.sdCard->writable()) {
      readyLine_ = "no SD card";
      readyHint_ = "the take is recorded to the card before it is sent";
      readyColor_ = theme::kWarn;
    } else if (services_.state == nullptr || !services_.state->internet) {
      readyLine_ = "offline";
      readyHint_ = "connect Wi-Fi, then ask again";
      readyColor_ = theme::kWarn;
    } else {
      readyLine_ = "ready";
      readyHint_ = "memory of this conversation clears when you leave this screen";
      readyColor_ = theme::kGood;
    }
    hintLines_ = countLines(gfx, readyHint_, kContentW - 2 * kBubblePadX,
                            TextStyle::Caption, 2);
    idleH_ = kBubblePadY + bodyH + hintLines_ * capH + kGap + 4 * capH + kBubblePadY;
  }
}

void AssistantApp::update(uint32_t deltaMs) {
  AssistantService* ai = services_.assistant;
  if (ai == nullptr) {
    return;
  }
  const uint8_t s = static_cast<uint8_t>(ai->state());
  if (s != lastState_) {
    // A new transcript, answer or error ALWAYS arrives with a state
    // transition, so this is the re-pagination trigger.
    lastState_ = s;
    page_ = 0;
    needsLayout_ = true;
    dirty_ = true;
  }
  const bool listening = ai->state() == AssistantService::State::Listening;
  const bool animated = listening || ai->state() == AssistantService::State::Transcribing ||
                        ai->state() == AssistantService::State::Thinking ||
                        ai->state() == AssistantService::State::Speaking;
  if (animated) {
    animationMs_ += deltaMs;
    if (animationMs_ >= kAnimationFrameMs) {
      animationMs_ %= kAnimationFrameMs;
      animationFrame_++;
      dirty_ = true;
    }
  } else {
    animationMs_ = 0;
    animationFrame_ = 0;
  }
  if (listening) {
    // Elapsed-seconds readout on the button; 2 Hz is plenty.
    tickMs_ += deltaMs;
    if (tickMs_ >= 500) {
      tickMs_ = 0;
      dirty_ = true;
    }
    // Mic level, ~10 Hz. At 30 Hz the bar reads as noise and costs a full
    // canvas flush per frame for a two-pixel change.
    meterMs_ += deltaMs;
    if (meterMs_ >= 100) {
      meterMs_ = 0;
      uint8_t target = 0;
      if (services_.audio != nullptr) {
        // takeLivePeak() is the loudest |sample| (0..32767) since the previous
        // call — i.e. over this 100 ms window — and consumes it. NOT
        // recordedPeak(): that is the take's running maximum, it never falls,
        // and it is load-bearing for the stop-time normalize gain.
        //
        // Square-rooted, because a linear bar barely leaves the left edge:
        // conversational speech through this mic chain peaks around 2000-8000
        // before the normalize.
        const uint32_t peak = services_.audio->takeLivePeak();
        const float norm = static_cast<float>(peak) / 32767.0f;
        target = static_cast<uint8_t>(sqrtf(norm) * 100.0f + 0.5f);
        if (target > 100) {
          target = 100;
        }
      }
      // Attack instantly, decay slowly: a meter that falls as fast as it rises
      // reads as noise. 4 points per 100 ms tick is a ~2.5 s fall from full.
      uint8_t next = level_;
      if (target > next) {
        next = target;
      } else if (next > 4) {
        next -= 4;
      } else {
        next = 0;
      }
      if (next != level_) {
        level_ = next;
        dirty_ = true;
      }
    }
  } else if (level_ != 0) {
    level_ = 0;
    dirty_ = true;
  }
  if (needsLayout_) {
    needsLayout_ = false;
    layout();
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
  const int16_t sx = services_.amoled->shiftX();
  const int16_t sy = services_.amoled->shiftY();

  gfx.fillScreen(theme::kBg);
  statusBar_.render(gfx, state, sx, sy);
  widgets::header(gfx, "Assistant", sx, sy);

  AssistantService* ai = services_.assistant;
  if (ai == nullptr) {
    widgets::footer(gfx, "assistant unavailable", nullptr, sx, sy);
    display->markDirty();
    return;
  }

  // The talk button is the single control: tap = listen, tap again = send.
  talkRect_ = {static_cast<int16_t>(kContentLeft + sx), static_cast<int16_t>(talkY_ + sy),
               kContentW, kTalkH};
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
  // While listening the label rides above the meter that shares the button.
  const int16_t labelBand =
      listening ? kTalkH - (kMeterH + kMeterBottomGap) : kTalkH;
  widgets::textCentered(gfx, talkRect_.x,
                        talkRect_.y + (labelBand - widgets::ascent(gfx, TextStyle::Body)) / 2,
                        talkRect_.w, label, TextStyle::Body, theme::kText);
  if (listening) {
    const int16_t mX = talkRect_.x + kMeterInsetX;
    const int16_t mW = talkRect_.w - 2 * kMeterInsetX;
    const int16_t mY = talkRect_.y + kTalkH - kMeterBottomGap - kMeterH;
    gfx.fillRoundRect(mX, mY, mW, kMeterH, kMeterH / 2, theme::kBg);
    const int16_t barW = static_cast<int16_t>((static_cast<int32_t>(mW) * level_) / 100);
    if (barW >= kMeterH) {
      gfx.fillRoundRect(mX, mY, barW, kMeterH, kMeterH / 2, theme::kText);
    } else if (barW > 0) {
      gfx.fillRect(mX, mY, barW, kMeterH, theme::kText);
    }
    // A breathing outline says "the mic is live" even during a quiet pause,
    // when the honest signal meter is flat.
    const int16_t inset = 4 + triangleWave(animationFrame_) / 3;
    gfx.drawRoundRect(talkRect_.x + inset, talkRect_.y + inset,
                      talkRect_.w - 2 * inset, talkRect_.h - 2 * inset,
                      theme::kCardRadius - 2, theme::kText);
  } else if (working) {
    // Three unequal dots travel below the label. This distinguishes upload /
    // transcription and model work from a frozen button without pretending we
    // have a real percentage from the API.
    const int16_t cy = talkRect_.y + kTalkH - 14;
    const int16_t center = talkRect_.x + talkRect_.w / 2;
    for (uint8_t i = 0; i < 3; i++) {
      const uint8_t crest = triangleWave(animationFrame_ + i * 5);
      const int16_t x = center + (static_cast<int16_t>(i) - 1) * 16;
      const int16_t radius = 2 + crest / 4;
      gfx.fillCircle(x, cy - crest / 5, radius,
                     crest >= 5 ? theme::kAccent : theme::kTextDim);
    }
  } else if (s == AssistantService::State::Speaking) {
    // Playback does not expose a safe output peak, so this is intentionally a
    // deterministic voice waveform rather than a false level meter.
    const int16_t baseline = talkRect_.y + kTalkH - 14;
    const int16_t startX = talkRect_.x + 64;
    for (uint8_t i = 0; i < 23; i++) {
      const uint8_t crest = triangleWave(animationFrame_ * 3 + i * 3);
      const int16_t amp = 2 + crest / 2;
      gfx.drawFastVLine(startX + i * 9, baseline - amp, 2 * amp + 1, theme::kBg);
    }
  }

  // Error text stacks ABOVE the transcript so the failed question stays
  // visible; it never replaces it.
  if (errLines_ > 0) {
    widgets::textBlock(gfx, kContentLeft + sx, errY_ + sy, kContentW, error_,
                       TextStyle::Caption, theme::kBad, errLines_);
  }
  if (qLines_ > 0) {
    // kPanelAlt is a FILL here, never ink: as text it measures 1.24-1.40:1
    // against kBg on all ten palettes.
    gfx.fillRoundRect(qX_ + sx, qY_ + sy, qW_, qH_, theme::kCardRadius, theme::kPanelAlt);
    widgets::textBlock(gfx, qX_ + kBubblePadX + sx, qY_ + kBubblePadY + sy,
                       qW_ - 2 * kBubblePadX, question_, TextStyle::Caption,
                       theme::kText, qLines_);
  }
  if (aLines_ > 0 && pageCount_ > 0) {
    // Fill FIRST, then the text over it: the canvas has no transparency, and
    // the box height is only known once the text has been wrapped (in
    // layout(), which is why render() can draw it in one pass).
    gfx.fillRoundRect(kContentLeft + sx, aY_ + sy, kAnswerW, aH_, theme::kCardRadius,
                      theme::kPanel);
    widgets::textBlock(gfx, kContentLeft + kBubblePadX + sx, aY_ + kBubblePadY + sy,
                       kAnswerW - 2 * kBubblePadX, answer_ + pageOffsets_[page_],
                       TextStyle::Body, theme::kText, aLines_);
  }

  if (showIdle_) {
    const int16_t capH = widgets::lineHeight(TextStyle::Caption);
    const int16_t bodyH = widgets::lineHeight(TextStyle::Body);
    gfx.fillRoundRect(kContentLeft + sx, idleY_ + sy, kContentW, idleH_,
                      theme::kCardRadius, theme::kPanel);
    int16_t ty = idleY_ + kBubblePadY + sy;
    const int16_t tx = kContentLeft + kBubblePadX + sx;
    const int16_t tw = kContentW - 2 * kBubblePadX;
    widgets::text(gfx, tx, ty, readyLine_, TextStyle::Body, readyColor_);
    ty += bodyH;
    if (hintLines_ > 0) {
      widgets::textBlock(gfx, tx, ty, tw, readyHint_, TextStyle::Caption,
                         theme::kTextDim, hintLines_);
      ty += hintLines_ * capH;
    }
    ty += kGap;
    static const char* const kSteps[4] = {
        "> tap TALK, then speak",
        "> tap SEND to ask",
        "> the reply plays back aloud",
        "> back cancels a take",
    };
    for (uint8_t i = 0; i < 4; i++) {
      widgets::text(gfx, tx, ty, kSteps[i], TextStyle::Caption, theme::kText);
      ty += capH;
    }
  }

  char foot[40];
  foot[0] = '\0';
  if (ai->historyDepth() > 0) {
    snprintf(foot, sizeof(foot), "%u exchange%s in memory",
             (unsigned)ai->historyDepth(), ai->historyDepth() == 1 ? "" : "s");
  }
  char pageLabel[16];
  const char* right = nullptr;
  if (pageCount_ > 1) {
    snprintf(pageLabel, sizeof(pageLabel), "pg %u/%u", (unsigned)(page_ + 1),
             (unsigned)pageCount_);
    right = pageLabel;
  }
  widgets::footer(gfx, foot, right, sx, sy);
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
    // A refused start can leave the state unchanged (Error -> Error) while the
    // reason changes, so relayout rather than trusting the transition alone.
    needsLayout_ = true;
    dirty_ = true;
    return true;
  }
  if (event.action == InputAction::Back &&
      ai->state() == AssistantService::State::Listening) {
    ai->cancelListening();  // drop the take; Back never sends by accident
    needsLayout_ = true;
    dirty_ = true;
    return true;
  }
  if (pageCount_ > 1 &&
      (event.action == InputAction::SwipeUp || event.action == InputAction::SwipeDown)) {
    if (event.action == InputAction::SwipeUp) {
      if (page_ + 1 < pageCount_) {
        page_++;
      }
    } else if (page_ > 0) {
      page_--;
    }
    needsLayout_ = true;  // the last page's box hugs its own line count
    dirty_ = true;
    return true;
  }
  return false;
}
