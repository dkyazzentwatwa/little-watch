#include "ReaderApp.h"

#include <Arduino_GFX_Library.h>
#include <string.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"

namespace {

// List mode geometry (mirrors NotesApp so the two readers feel identical).
constexpr int16_t kListContentTop = theme::kStatusBarHeight + 44;
constexpr int16_t kListRowStep = 62;

// Reading mode geometry. The body fills the space between a slim title strip
// and a slim progress footer. These MUST match what render() feeds textBlock,
// because BookService::readPage() paginates against exactly these numbers.
constexpr int16_t kReaderTop = theme::kStatusBarHeight + 30;
constexpr int16_t kFooterH = 24;
constexpr int16_t kBodyWidth = DISPLAY_WIDTH - 2 * theme::kPadding;
constexpr int16_t kContentBottom = DISPLAY_HEIGHT - kFooterH;
constexpr int16_t kContentHeight = kContentBottom - kReaderTop;

// Base GFX cell, multiplied by text size — the same constants widgets::textBlock
// wraps against.
constexpr uint8_t kWrapCharW = 6;
constexpr uint8_t kWrapCharH = 8;

// Font-size table (index cycled by double-tap). Kept in the .cpp so there is no
// static-member definition to worry about. Must have kFontCount entries — the
// static_assert enforcing that lives in ReaderApp::fontSize(), which can see
// both this array and the (private) class constant.
constexpr uint8_t kFontSizes[] = {2, 3, 4};

uint16_t charsPerLineFor(uint8_t fontSize) {
  return static_cast<uint16_t>(kBodyWidth / (kWrapCharW * fontSize));
}

uint16_t maxLinesFor(uint8_t fontSize) {
  const int16_t lineH = kWrapCharH * fontSize + 2;  // matches textBlock's line pitch
  return static_cast<uint16_t>(kContentHeight / lineH);
}

void formatSize(uint32_t bytes, char* out, size_t cap) {
  if (bytes >= 1024u * 1024u) {
    snprintf(out, cap, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024u) {
    snprintf(out, cap, "%lu KB", static_cast<unsigned long>(bytes / 1024u));
  } else {
    snprintf(out, cap, "%lu B", static_cast<unsigned long>(bytes));
  }
}

}  // namespace

uint8_t ReaderApp::fontSize() const {
  static_assert(sizeof(kFontSizes) / sizeof(kFontSizes[0]) == kFontCount,
                "kFontSizes must have kFontCount entries");
  return kFontSizes[fontIdx_ < kFontCount ? fontIdx_ : 0];
}

void ReaderApp::onOpen() {
  refreshList();
  mode_ = Mode::List;
  dirty_ = true;
}

void ReaderApp::onPause() {
  // May not come back — persist the reading place before backgrounding.
  saveResume();
}

void ReaderApp::onResume() {
  // Serial file ops or an SD remount can move the list while backgrounded, and
  // the open book itself may be gone. Navigate by the stored path, not index.
  refreshList();
  if (mode_ == Mode::Reading) {
    bookSize_ = services_.books != nullptr ? services_.books->fileSize(openPath_) : 0;
    if (bookSize_ == 0) {
      mode_ = Mode::List;  // book or card gone; nothing to read
    } else {
      if (curOffset_ >= bookSize_) {
        curOffset_ = 0;  // book was truncated/edited under us
        backDepth_ = 0;
      }
      layoutPage();
    }
  }
  dirty_ = true;
}

void ReaderApp::onClose() {
  // Leaving for good: save the place and drop the page buffer. Unlike NotesApp
  // (which held up to 12 KB), the reader only ever holds one page — the whole
  // point of streaming — so there is nothing large to release, but reset anyway.
  saveResume();
  pageBuf_[0] = '\0';
  pageBytes_ = 0;
  backDepth_ = 0;
  mode_ = Mode::List;
}

void ReaderApp::refreshList() {
  bookCount_ = services_.books != nullptr ? services_.books->list(books_, kMaxBooks, &bookTotal_)
                                          : 0;
  if (listStart_ >= bookCount_) {
    listStart_ = 0;
  }
}

bool ReaderApp::openBook(size_t index) {
  if (index >= bookCount_ || services_.books == nullptr) {
    return false;
  }
  strncpy(openPath_, books_[index].path, sizeof(openPath_) - 1);
  openPath_[sizeof(openPath_) - 1] = '\0';
  strncpy(openTitle_, books_[index].title, sizeof(openTitle_) - 1);
  openTitle_[sizeof(openTitle_) - 1] = '\0';
  openIndex_ = index;

  bookSize_ = services_.books->fileSize(openPath_);
  uint32_t resume = services_.books->position(openPath_);
  if (bookSize_ == 0 || resume >= bookSize_) {
    resume = 0;  // empty / edited / truncated book — start at the top
  }
  curOffset_ = resume;
  backDepth_ = 0;
  mode_ = Mode::Reading;
  layoutPage();
  dirty_ = true;
  return true;
}

void ReaderApp::layoutPage() {
  if (services_.books == nullptr) {
    readError_ = true;
    pageBytes_ = 0;
    pageBuf_[0] = '\0';
    return;
  }
  const uint8_t fs = fontSize();
  bool atEnd = false;
  uint32_t next = curOffset_;
  const size_t wrote = services_.books->readPage(openPath_, curOffset_, charsPerLineFor(fs),
                                                 maxLinesFor(fs), pageBuf_, sizeof(pageBuf_),
                                                 next, atEnd);
  pageBytes_ = wrote;
  nextOffset_ = next;
  atEnd_ = atEnd;
  // 0 bytes with atEnd=false is a read failure (card pulled mid-book); 0 bytes
  // with atEnd=true is a legitimately empty book.
  readError_ = (wrote == 0 && !atEnd);
}

void ReaderApp::nextPage() {
  if (readError_) {
    layoutPage();  // retry — the card may be back
    dirty_ = true;
    return;
  }
  if (atEnd_) {
    return;  // already on the last page
  }
  pushBack(curOffset_);
  curOffset_ = nextOffset_;
  layoutPage();
  dirty_ = true;
}

void ReaderApp::prevPage() {
  if (readError_) {
    layoutPage();  // retry
    dirty_ = true;
    return;
  }
  if (backDepth_ == 0) {
    return;  // at the start, or at the oldest page the bounded stack remembers
  }
  curOffset_ = backStack_[--backDepth_];
  layoutPage();
  dirty_ = true;
}

void ReaderApp::pushBack(uint32_t offset) {
  if (backDepth_ < kMaxBackStack) {
    backStack_[backDepth_++] = offset;
    return;
  }
  // Full: drop the oldest so recent back-paging always works and RAM stays fixed.
  memmove(backStack_, backStack_ + 1, sizeof(uint32_t) * (kMaxBackStack - 1));
  backStack_[kMaxBackStack - 1] = offset;
}

void ReaderApp::cycleFont() {
  fontIdx_ = static_cast<uint8_t>((fontIdx_ + 1) % kFontCount);
  // Page boundaries depend on the font, so the recorded page-starts no longer
  // align. Drop them; the current position (top of page) is preserved by
  // re-laying out from curOffset_.
  backDepth_ = 0;
  layoutPage();
  dirty_ = true;
}

void ReaderApp::returnToList() {
  saveResume();
  mode_ = Mode::List;
  refreshList();  // refresh the resume % shown on the rows
  dirty_ = true;
}

void ReaderApp::saveResume() {
  if (mode_ == Mode::Reading && openPath_[0] != '\0' && bookSize_ > 0 &&
      services_.books != nullptr) {
    services_.books->setPosition(openPath_, curOffset_);
  }
}

void ReaderApp::render() {
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

  switch (mode_) {
    case Mode::List:
      renderList(gfx);
      break;
    case Mode::Reading:
      renderReading(gfx);
      break;
  }
  display->markDirty();
}

void ReaderApp::renderList(Arduino_GFX& gfx) {
  // Name the cap honestly: "Reader (48 of 130)" on a card holding more.
  char header[40];
  if (bookTotal_ > bookCount_) {
    snprintf(header, sizeof(header), "Reader (%u of %u)", static_cast<unsigned>(bookCount_),
             static_cast<unsigned>(bookTotal_));
  } else {
    snprintf(header, sizeof(header), "Reader (%u)", static_cast<unsigned>(bookCount_));
  }
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 12);
  gfx.print(header);

  if (bookCount_ == 0) {
    const SdCardState sd = services_.state->sd;
    const bool readable = sd == SdCardState::Mounted || sd == SdCardState::ReadOnly ||
                          sd == SdCardState::Full;
    widgets::textBlock(
        gfx, theme::kPadding, kListContentTop + 20, kBodyWidth,
        readable ? "No books here yet. Put .txt or .md files in /littlecube/documents."
                 : "Insert an SD card with books in /littlecube/documents.",
        theme::kTextSizeSmall, theme::kTextDim);
    return;
  }

  int16_t y = kListContentTop;
  for (size_t i = 0; i < kPageSize; i++) {
    const size_t idx = listStart_ + i;
    if (idx >= bookCount_) {
      rowRects_[i] = widgets::Rect{};
      continue;
    }
    const BookInfo& b = books_[idx];
    char sizeStr[16];
    formatSize(b.sizeBytes, sizeStr, sizeof(sizeStr));
    char secondary[48];
    if (b.sizeBytes > 0 && b.resumeOffset > 0) {
      uint32_t pct = static_cast<uint32_t>(static_cast<uint64_t>(b.resumeOffset) * 100u /
                                           b.sizeBytes);
      if (pct > 100) {
        pct = 100;
      }
      snprintf(secondary, sizeof(secondary), "%s  %lu%%", sizeStr,
               static_cast<unsigned long>(pct));
    } else {
      snprintf(secondary, sizeof(secondary), "%s", sizeStr);
    }
    rowRects_[i] =
        widgets::listItem(gfx, theme::kPadding, y, kBodyWidth, b.title, secondary, false);
    y += kListRowStep;
  }

  char pager[48];
  if (bookTotal_ > bookCount_) {
    snprintf(pager, sizeof(pager), "%u-%u of %u  +%u more",
             static_cast<unsigned>(listStart_ + 1),
             static_cast<unsigned>(min(listStart_ + kPageSize, bookCount_)),
             static_cast<unsigned>(bookCount_), static_cast<unsigned>(bookTotal_ - bookCount_));
  } else {
    snprintf(pager, sizeof(pager), "%u-%u of %u   swipe up/down",
             static_cast<unsigned>(listStart_ + 1),
             static_cast<unsigned>(min(listStart_ + kPageSize, bookCount_)),
             static_cast<unsigned>(bookCount_));
  }
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
  gfx.print(pager);
}

void ReaderApp::renderReading(Arduino_GFX& gfx) {
  // Visible back-to-list control (never gesture-only), matched before tap-turn.
  // Inset the box by kSafeInset (not kPadding) so it clears the rounded
  // top-left corner instead of sitting in the bezel.
  backRect_ = widgets::Rect{theme::kSafeInset, theme::kStatusBarHeight + 2, 40, 26};
  gfx.drawRoundRect(backRect_.x, backRect_.y, backRect_.w, backRect_.h, 6, theme::kTextDim);
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(backRect_.x + 12, backRect_.y + 6);
  gfx.print("<");

  // Book title: left edge past the back box, right edge stopping at the safe
  // inset. Measure how many characters fit at this text size (6px cell * size)
  // and, when the title is longer, cut it and append ".." — the 6x8 GFX font
  // has no ellipsis glyph, so a literal marker is the only honest option (no
  // stray '~'/'^').
  const int16_t titleX = backRect_.x + backRect_.w + 12;
  const int16_t titleCharW = kWrapCharW * theme::kTextSizeSmall;
  const int16_t titleMaxChars =
      static_cast<int16_t>((DISPLAY_WIDTH - theme::kSafeInset - titleX) / titleCharW);
  char shown[68];
  if (titleMaxChars > 0 && static_cast<int16_t>(strlen(openTitle_)) > titleMaxChars) {
    const int keep = titleMaxChars > 2 ? titleMaxChars - 2 : 0;
    snprintf(shown, sizeof(shown), "%.*s..", keep, openTitle_);
  } else {
    const int cap = titleMaxChars > 0 ? titleMaxChars : 0;
    snprintf(shown, sizeof(shown), "%.*s", cap, openTitle_);  // bounded: silences -Wformat-truncation
  }
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kAccent);
  gfx.setCursor(titleX, theme::kStatusBarHeight + 8);
  gfx.print(shown);

  if (readError_) {
    widgets::textBlock(gfx, theme::kPadding, kReaderTop + 20, kBodyWidth,
                       "Couldn't read this page. The SD card may have been removed. "
                       "Tap the middle to retry, or tap < to go back.",
                       theme::kTextSizeSmall, theme::kWarn);
    return;
  }

  if (pageBytes_ == 0) {
    widgets::textBlock(gfx, theme::kPadding, kReaderTop + 20, kBodyWidth, "This book is empty.",
                       theme::kTextSizeSmall, theme::kTextDim);
  } else {
    widgets::textBlock(gfx, theme::kPadding, kReaderTop, kBodyWidth, pageBuf_, fontSize(),
                       theme::kText);
  }

  // Progress: a percentage plus a thin bar that grows as the book is read. The
  // bar moves with reading, so it is not a burn-in-prone static element.
  uint32_t pct = 0;
  if (bookSize_ > 0) {
    const uint32_t num = atEnd_ ? bookSize_ : curOffset_;
    pct = static_cast<uint32_t>(static_cast<uint64_t>(num) * 100u / bookSize_);
    if (pct > 100) {
      pct = 100;
    }
  }
  // Footer laid out inside the rounded-corner safe area (mirrors how AudioApp
  // pulls its transport controls in). The % sits at the left inset, the font
  // indicator's right edge stops at the right inset, and the baseline is the
  // top of the footer band (kContentBottom) so the text clears the bottom
  // bezel — the pagination math guarantees the body never reaches this row.
  const int16_t footX = theme::kSafeInset;
  const int16_t footRight = DISPLAY_WIDTH - theme::kSafeInset;
  const int16_t footY = kContentBottom;

  char foot[16];
  snprintf(foot, sizeof(foot), "%lu%%", static_cast<unsigned long>(pct));
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(footX, footY);
  gfx.print(foot);

  char fbuf[8];
  snprintf(fbuf, sizeof(fbuf), "A%u", static_cast<unsigned>(fontSize()));
  const int16_t fw = static_cast<int16_t>(strlen(fbuf)) * kWrapCharW * theme::kTextSizeSmall;
  gfx.setCursor(footRight - fw, footY);
  gfx.print(fbuf);

  // Thin progress bar, ends inset to the safe area and lifted 1px off the true
  // bottom edge so the corners don't clip it.
  const int16_t barX = theme::kSafeInset;
  const int16_t barW = DISPLAY_WIDTH - 2 * theme::kSafeInset;
  const int16_t barY = DISPLAY_HEIGHT - 4;
  gfx.fillRect(barX, barY, barW, 3, theme::kPanelAlt);
  const int16_t fillW = static_cast<int16_t>(static_cast<int32_t>(barW) *
                                             static_cast<int32_t>(pct) / 100);
  gfx.fillRect(barX, barY, fillW, 3, theme::kAccent);
}

bool ReaderApp::handleList(const InputEvent& event) {
  switch (event.action) {
    case InputAction::SwipeUp:
      if (listStart_ + kPageSize < bookCount_) {
        listStart_ += kPageSize;
        dirty_ = true;
      }
      return true;
    case InputAction::SwipeDown:
      if (listStart_ >= kPageSize) {
        listStart_ -= kPageSize;
      } else {
        listStart_ = 0;
      }
      dirty_ = true;
      return true;
    case InputAction::Tap:
      for (size_t i = 0; i < kPageSize; i++) {
        if (rowRects_[i].contains(event.x, event.y)) {
          openBook(listStart_ + i);
          return true;
        }
      }
      return true;
    default:
      return false;  // Back/Home fall through to the router (closes Reader)
  }
}

bool ReaderApp::handleReading(const InputEvent& event) {
  switch (event.action) {
    case InputAction::SwipeUp:
    case InputAction::SwipeLeft:
      nextPage();
      return true;
    case InputAction::SwipeDown:
    case InputAction::SwipeRight:
      prevPage();
      return true;
    case InputAction::DoubleTap:
      cycleFont();
      return true;
    case InputAction::Tap:
      if (backRect_.contains(event.x, event.y)) {
        returnToList();
      } else if (event.y >= kReaderTop && event.y < kContentBottom) {
        // Top half = previous page, bottom half = next page (spec brief).
        if (event.y < kReaderTop + kContentHeight / 2) {
          prevPage();
        } else {
          nextPage();
        }
      }
      return true;
    case InputAction::Back:
    case InputAction::Cancel:
      // Internal navigation Reading -> List (matches NotesApp); Home is left
      // unconsumed below so the router takes it.
      returnToList();
      return true;
    default:
      return false;
  }
}

bool ReaderApp::handleInput(const InputEvent& event) {
  switch (mode_) {
    case Mode::List:
      return handleList(event);
    case Mode::Reading:
      return handleReading(event);
  }
  return false;
}
