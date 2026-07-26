#include "NotesApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"

namespace {
constexpr int16_t kContentTop = theme::kStatusBarHeight + 44;
constexpr int16_t kScrollStep = 280;
constexpr int16_t kTruncBarH = 28;

// Mirrors of widgets::textBlock's geometry. Keep these in step with
// ui/widgets/Widgets.cpp — the scroll bound is only right if the count below
// reproduces that function exactly.
constexpr int16_t kWrapCharW = 6;    // base GFX cell, multiplied by text size
constexpr int16_t kWrapCharH = 8;
constexpr int16_t kWrapLineMax = 95;  // its internal line[96], minus the NUL

int16_t charsPerLineFor(uint8_t fontSize) {
  return (DISPLAY_WIDTH - 2 * theme::kPadding) / (kWrapCharW * fontSize);
}

// A faithful replay of widgets::textBlock's greedy wrap, counting lines
// instead of drawing them. The old estimate (length / charsPerLine + 8)
// ignored that textBlock also breaks on '\n' and at the last space that
// fits, so a note of many short lines produced far more lines than the
// estimate — and the tail was pinned below the bottom of the screen with no
// way to scroll to it.
int32_t countWrappedLines(const char* text, int16_t charsPerLine) {
  if (text == nullptr || charsPerLine <= 0) {
    return 0;
  }
  int32_t lines = 0;
  const char* p = text;
  while (*p != '\0') {
    int16_t take = 0;
    int16_t lastSpace = -1;
    while (p[take] != '\0' && p[take] != '\n' && take < charsPerLine && take < kWrapLineMax) {
      if (p[take] == ' ') {
        lastSpace = take;
      }
      take++;
    }
    int16_t lineLen = take;
    if (p[take] != '\0' && p[take] != '\n' && lastSpace > 0) {
      lineLen = lastSpace;
    }
    lines++;
    p += lineLen;
    while (*p == ' ') {
      p++;
    }
    if (*p == '\n') {
      p++;
    }
  }
  return lines;
}
}  // namespace

void NotesApp::onOpen() {
  refreshList();
  mode_ = Mode::List;
  dirty_ = true;
}

void NotesApp::onPause() {
  // Never leave a delete confirm armed across a background trip: it would be
  // sitting under the user's first tap when they come back.
  if (mode_ == Mode::ConfirmDelete) {
    mode_ = Mode::List;
    dirty_ = true;
  }
}

void NotesApp::onResume() {
  // Serial `notes new`/`notes delete` and an SD remount both mutate the list
  // while this app is backgrounded, and deleting renumbers everything after
  // the hole — so the cached array and openIndex_ can each be stale.
  refreshList();
  if (openIndex_ >= noteCount_) {
    openIndex_ = 0;
    mode_ = Mode::List;  // renderReading() would index past the end
  }
  dirty_ = true;
}

void NotesApp::onClose() {
  // body_ is up to 12 KB of heap held for an app the user has left; the next
  // visit re-reads from the card anyway.
  body_ = String();
  bodyTruncated_ = false;
  wrappedLines_ = 0;
  scrollY_ = 0;
  mode_ = Mode::List;
}

void NotesApp::refreshList() {
  noteCount_ =
      services_.notes != nullptr ? services_.notes->list(notes_, kMaxNotes, &noteTotal_) : 0;
  if (pageStart_ >= noteCount_) {
    pageStart_ = 0;
  }
}

void NotesApp::measureBody() {
  wrappedLines_ = countWrappedLines(body_.c_str(), charsPerLineFor(fontSize_));
}

int32_t NotesApp::maxScroll() const {
  const int32_t lineH = kWrapCharH * fontSize_ + 2;
  // The truncation warning sits in its own strip, so the last line has to
  // clear it or it can never be read.
  const int32_t viewH =
      DISPLAY_HEIGHT - kContentTop - (bodyTruncated_ ? kTruncBarH : 0);
  const int32_t over = wrappedLines_ * lineH - viewH;
  return over > 0 ? over : 0;
}

bool NotesApp::openNote(size_t index) {
  if (index >= noteCount_ || services_.notes == nullptr) {
    return false;
  }
  if (!services_.notes->read(notes_[index].path, body_, kMaxBodyBytes, bodyTruncated_)) {
    body_ = "(could not read note)";
    bodyTruncated_ = false;
  }
  measureBody();  // once per note, not once per frame
  openIndex_ = index;
  scrollY_ = 0;
  mode_ = Mode::Reading;
  dirty_ = true;
  return true;
}

void NotesApp::render() {
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

  switch (mode_) {
    case Mode::List:
      renderList(gfx);
      break;
    case Mode::Reading:
      renderReading(gfx);
      break;
    case Mode::ConfirmDelete:
      renderReading(gfx);
      renderConfirmDelete(gfx);
      break;
  }
  display->markDirty();
}

void NotesApp::renderList(Arduino_GFX& gfx) {
  // "Notes (48)" on a card holding 132 of them is a lie by omission — the
  // cap is real, so name it.
  char header[40];
  if (noteTotal_ > noteCount_) {
    snprintf(header, sizeof(header), "Notes (%u of %u)", (unsigned)noteCount_,
             (unsigned)noteTotal_);
  } else {
    snprintf(header, sizeof(header), "Notes (%u)", (unsigned)noteCount_);
  }
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 12);
  gfx.print(header);

  if (noteCount_ == 0) {
    const bool sdOk = services_.state->sd == SdCardState::Mounted ||
                      services_.state->sd == SdCardState::ReadOnly;
    widgets::textBlock(gfx, theme::kPadding, kContentTop + 20, DISPLAY_WIDTH - 2 * theme::kPadding,
                       sdOk ? "No notes yet. Create one over USB serial: notes write ideas.md"
                            : "Insert an SD card to read notes.",
                       theme::kTextSizeSmall, theme::kTextDim);
    return;
  }

  int16_t y = kContentTop;
  for (size_t i = 0; i < kPageSize; i++) {
    const size_t idx = pageStart_ + i;
    if (idx >= noteCount_) {
      rowRects_[i] = widgets::Rect{};
      continue;
    }
    const NoteInfo& note = notes_[idx];
    char secondary[64];
    snprintf(secondary, sizeof(secondary), "%s%s%u B", note.fromDeck ? "deck · " : "",
             note.favorite ? "* " : "", (unsigned)note.sizeBytes);
    rowRects_[i] = widgets::listItem(gfx, theme::kPadding, y,
                                     DISPLAY_WIDTH - 2 * theme::kPadding, note.title,
                                     secondary, false);
    if (note.pinned) {
      gfx.fillCircle(DISPLAY_WIDTH - theme::kPadding - 8, y + 12, 4, theme::kAccent);
    }
    y += 62;
  }

  char pager[48];
  if (noteTotal_ > noteCount_) {
    snprintf(pager, sizeof(pager), "%u-%u of %u  +%u more", (unsigned)(pageStart_ + 1),
             (unsigned)min(pageStart_ + kPageSize, noteCount_), (unsigned)noteCount_,
             (unsigned)(noteTotal_ - noteCount_));
  } else {
    snprintf(pager, sizeof(pager), "%u-%u of %u   swipe up/down", (unsigned)(pageStart_ + 1),
             (unsigned)min(pageStart_ + kPageSize, noteCount_), (unsigned)noteCount_);
  }
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
  gfx.print(pager);
}

void NotesApp::renderReading(Arduino_GFX& gfx) {
  const NoteInfo& note = notes_[openIndex_];

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kAccent);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 8);
  gfx.print(note.title);

  deleteRect_ = widgets::Rect{DISPLAY_WIDTH - 76, theme::kStatusBarHeight + 2, 64, 30};
  gfx.drawRoundRect(deleteRect_.x, deleteRect_.y, deleteRect_.w, deleteRect_.h, 6, theme::kBad);
  gfx.setTextColor(theme::kBad);
  gfx.setCursor(deleteRect_.x + 8, deleteRect_.y + 8);
  gfx.print("del");

  widgets::textBlock(gfx, theme::kPadding, kContentTop - scrollY_,
                     DISPLAY_WIDTH - 2 * theme::kPadding, body_.c_str(), fontSize_,
                     theme::kText);

  if (bodyTruncated_) {
    // Shown at every scroll position, not just the top. The warning matters
    // most when the reader reaches the end and wonders why it stops there —
    // which is exactly where it used to have vanished. It needs its own
    // opaque strip because the body scrolls underneath it.
    gfx.fillRect(0, DISPLAY_HEIGHT - kTruncBarH, DISPLAY_WIDTH, kTruncBarH, theme::kBg);
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kWarn);
    gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - kTruncBarH + 6);
    gfx.print("note truncated on device");
  }
}

void NotesApp::renderConfirmDelete(Arduino_GFX& gfx) {
  confirmRect_ = widgets::modalConfirm(gfx, "Delete note?", notes_[openIndex_].title, cancelRect_);
}

bool NotesApp::handleList(const InputEvent& event) {
  switch (event.action) {
    case InputAction::SwipeUp:
      if (pageStart_ + kPageSize < noteCount_) {
        pageStart_ += kPageSize;
        dirty_ = true;
      }
      return true;
    case InputAction::SwipeDown:
      if (pageStart_ >= kPageSize) {
        pageStart_ -= kPageSize;
      } else {
        pageStart_ = 0;
      }
      dirty_ = true;
      return true;
    case InputAction::Tap:
      for (size_t i = 0; i < kPageSize; i++) {
        if (rowRects_[i].contains(event.x, event.y)) {
          openNote(pageStart_ + i);
          return true;
        }
      }
      return true;
    case InputAction::LongPress:
      for (size_t i = 0; i < kPageSize; i++) {
        if (rowRects_[i].contains(event.x, event.y)) {
          const size_t idx = pageStart_ + i;
          if (services_.notes != nullptr) {
            services_.notes->setFavorite(notes_[idx].path, !notes_[idx].favorite);
            refreshList();
            dirty_ = true;
          }
          return true;
        }
      }
      return true;
    default:
      return false;
  }
}

bool NotesApp::handleReading(const InputEvent& event) {
  switch (event.action) {
    case InputAction::SwipeUp: {
      // Bound comes from the cached line count measured in openNote(), and
      // the last step is clamped so the final line lands flush with the
      // bottom instead of overshooting past it.
      const int32_t limit = maxScroll();
      if (scrollY_ < limit) {
        const int32_t next = static_cast<int32_t>(scrollY_) + kScrollStep;
        scrollY_ = static_cast<int16_t>(next > limit ? limit : next);
        dirty_ = true;
      }
      return true;
    }
    case InputAction::SwipeDown:
      if (scrollY_ > 0) {
        scrollY_ = scrollY_ > kScrollStep ? scrollY_ - kScrollStep : 0;
        dirty_ = true;
      }
      return true;
    case InputAction::SwipeLeft:
      if (openIndex_ + 1 < noteCount_) {
        openNote(openIndex_ + 1);
      }
      return true;
    case InputAction::SwipeRight:
      if (openIndex_ > 0) {
        openNote(openIndex_ - 1);
      }
      return true;
    case InputAction::DoubleTap:
      fontSize_ = fontSize_ == 2 ? 3 : 2;
      measureBody();  // a different font wraps to a different line count
      scrollY_ = 0;
      dirty_ = true;
      return true;
    case InputAction::Tap:
      if (deleteRect_.contains(event.x, event.y)) {
        mode_ = Mode::ConfirmDelete;
        dirty_ = true;
      }
      return true;
    case InputAction::Back:
    case InputAction::Cancel:
      mode_ = Mode::List;
      refreshList();
      dirty_ = true;
      return true;
    default:
      return false;
  }
}

bool NotesApp::handleConfirmDelete(const InputEvent& event) {
  if (event.action == InputAction::Tap) {
    if (confirmRect_.contains(event.x, event.y)) {
      if (services_.notes != nullptr) {
        services_.notes->remove(notes_[openIndex_].path);
      }
      refreshList();
      mode_ = Mode::List;
      dirty_ = true;
      return true;
    }
    if (cancelRect_.contains(event.x, event.y)) {
      mode_ = Mode::Reading;
      dirty_ = true;
      return true;
    }
    return true;
  }
  if (event.action == InputAction::Back || event.action == InputAction::Cancel) {
    mode_ = Mode::Reading;
    dirty_ = true;
    return true;
  }
  return false;
}

bool NotesApp::handleInput(const InputEvent& event) {
  switch (mode_) {
    case Mode::List:
      return handleList(event);
    case Mode::Reading:
      return handleReading(event);
    case Mode::ConfirmDelete:
      return handleConfirmDelete(event);
  }
  return false;
}
