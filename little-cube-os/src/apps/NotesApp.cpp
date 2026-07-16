#include "NotesApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../ui/Theme.h"

namespace {
constexpr int16_t kContentTop = theme::kStatusBarHeight + 44;
constexpr int16_t kScrollStep = 280;
}  // namespace

void NotesApp::onOpen() {
  refreshList();
  mode_ = Mode::List;
  dirty_ = true;
}

void NotesApp::refreshList() {
  noteCount_ = services_.notes != nullptr ? services_.notes->list(notes_, kMaxNotes) : 0;
  if (pageStart_ >= noteCount_) {
    pageStart_ = 0;
  }
}

bool NotesApp::openNote(size_t index) {
  if (index >= noteCount_ || services_.notes == nullptr) {
    return false;
  }
  if (!services_.notes->read(notes_[index].path, body_, kMaxBodyBytes, bodyTruncated_)) {
    body_ = "(could not read note)";
    bodyTruncated_ = false;
  }
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
  statusBar_.render(gfx, state, 0, 0);

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
  char header[32];
  snprintf(header, sizeof(header), "Notes (%u)", (unsigned)noteCount_);
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

  char pager[40];
  snprintf(pager, sizeof(pager), "%u-%u of %u   swipe up/down", (unsigned)(pageStart_ + 1),
           (unsigned)min(pageStart_ + kPageSize, noteCount_), (unsigned)noteCount_);
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

  if (bodyTruncated_ && scrollY_ == 0) {
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kWarn);
    gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
    gfx.print("(long note truncated on device)");
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
      // Rough content height from wrapped-line count keeps the scroll
      // bounded without measuring every frame.
      const int16_t charsPerLine = (DISPLAY_WIDTH - 2 * theme::kPadding) / (6 * fontSize_);
      const int32_t roughLines = charsPerLine > 0
                                     ? static_cast<int32_t>(body_.length()) / charsPerLine + 8
                                     : 8;
      const int32_t maxScroll = roughLines * (8 * fontSize_ + 2) - (DISPLAY_HEIGHT - kContentTop);
      if (scrollY_ < maxScroll) {
        scrollY_ += kScrollStep;
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
