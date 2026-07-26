#include "NewsApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"

namespace {
constexpr int16_t kContentTop = theme::kStatusBarHeight + 44;  // first list row
constexpr int16_t kRowStride = 62;                             // matches NotesApp pitch

// Chars of a headline that fit one list row at text size 2 (12 px/char) inside
// the row's inner width. listItem does NOT clip, so the app must. Computed once
// here and asserted against the real geometry: (DISPLAY_WIDTH - 2*pad - inset).
constexpr size_t kTitleClip = 27;

// Detail (reading) geometry, ReaderApp in miniature: the composed text fills
// the band between the position header and a slim footer. These MUST match
// what wrapWalk is fed for both pagination and drawing.
constexpr int16_t kDetailTop = theme::kStatusBarHeight + 30;
constexpr int16_t kDetailBottom = DISPLAY_HEIGHT - 36;  // footer band starts here
constexpr int16_t kDetailBodyW = DISPLAY_WIDTH - 2 * theme::kPadding;

// Fixed GFX cell, multiplied by text size — the same constants the fixed-cell
// widgets::textBlock wraps against.
constexpr uint8_t kWrapCharW = 6;
constexpr uint8_t kWrapCharH = 8;

// Font-size table cycled by double-tap, identical to ReaderApp's.
constexpr uint8_t kDetailFontSizes[] = {2, 3, 4};

uint16_t detailCharsPerLine(uint8_t fontSize) {
  return static_cast<uint16_t>(kDetailBodyW / (kWrapCharW * fontSize));
}

uint16_t detailMaxLines(uint8_t fontSize) {
  const int16_t lineH = kWrapCharH * fontSize + 2;  // matches textBlock's line pitch
  return static_cast<uint16_t>((kDetailBottom - kDetailTop) / lineH);
}

// Greedy fixed-cell word wrap — the exact algorithm of the uint8_t
// widgets::textBlock overload, but bounded by maxLines, resumable from an
// offset, and draw-optional. Pagination calls it with gfx == nullptr to find
// where each page starts; render() calls it again to draw — one walker, so the
// page breaks it computes are by construction the ones the user sees.
size_t wrapWalk(Arduino_GFX* gfx, const char* body, size_t from, uint8_t textSize, int16_t x,
                int16_t y, uint16_t maxLines, uint16_t color) {
  const int16_t charsPerLine = detailCharsPerLine(textSize);
  const int16_t lineH = kWrapCharH * textSize + 2;
  if (charsPerLine <= 0 || body == nullptr) {
    return strlen(body != nullptr ? body : "");
  }
  if (gfx != nullptr) {
    gfx->setTextSize(textSize);
    gfx->setTextColor(color);
  }
  const char* p = body + from;
  char line[96];
  uint16_t drawn = 0;

  while (*p != '\0' && drawn < maxLines) {
    int16_t take = 0;
    int16_t lastSpace = -1;
    while (p[take] != '\0' && p[take] != '\n' && take < charsPerLine &&
           take < static_cast<int16_t>(sizeof(line)) - 1) {
      if (p[take] == ' ') {
        lastSpace = take;
      }
      take++;
    }
    int16_t lineLen = take;
    if (p[take] != '\0' && p[take] != '\n' && lastSpace > 0) {
      lineLen = lastSpace;  // break at the last space that fits
    }
    if (gfx != nullptr) {
      memcpy(line, p, lineLen);
      line[lineLen] = '\0';
      gfx->setCursor(x, y);
      gfx->print(line);
    }
    y += lineH;
    drawn++;

    p += lineLen;
    while (*p == ' ') {
      p++;
    }
    if (*p == '\n') {
      p++;
    }
  }
  return static_cast<size_t>(p - body);
}

// Chars of a summary teaser that fit the list row's secondary line (Caption
// style is narrower than the size-2 title, so more chars fit than kTitleClip).
constexpr size_t kTeaserClip = 38;

// Copy up to maxChars chars, ending in "..." when the source is longer. dst
// must hold at least maxChars + 1 bytes.
void clipText(char* dst, size_t cap, const char* src, size_t maxChars) {
  const size_t len = strlen(src);
  if (len <= maxChars) {
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
    return;
  }
  size_t keep = maxChars >= 3 ? maxChars - 3 : 0;
  if (keep > cap - 4) {
    keep = cap - 4;
  }
  memcpy(dst, src, keep);
  dst[keep] = '.';
  dst[keep + 1] = '.';
  dst[keep + 2] = '.';
  dst[keep + 3] = '\0';
}
}  // namespace

void NewsApp::onOpen() {
  mode_ = Mode::List;
  pageStart_ = 0;
  dirty_ = true;
  // First open with nothing cached and a live connection: kick a pull now so
  // the user is not staring at "No headlines yet". Guarded (no double fetch,
  // internet-checked) inside the service, and non-blocking.
  if (services_.news != nullptr && services_.news->count() == 0) {
    services_.news->refresh();
  }
}

void NewsApp::onClose() {
  mode_ = Mode::List;
}

void NewsApp::onPause() {}

void NewsApp::onResume() {
  // Headlines may have refreshed while backgrounded; clamp a now-stale Detail
  // index (or recompose the survivor) and force a fresh frame.
  if (services_.news != nullptr && openIndex_ >= services_.news->count()) {
    mode_ = Mode::List;
  } else if (mode_ == Mode::Detail && services_.news != nullptr) {
    composeDetail();
  }
  dirty_ = true;
}

size_t NewsApp::count() const {
  return services_.news != nullptr ? services_.news->count() : 0;
}

void NewsApp::update(uint32_t deltaMs) {
  (void)deltaMs;
  NewsService* news = services_.news;
  if (news == nullptr) {
    return;
  }
  const bool busy = news->fetching();
  if (busy != lastFetching_) {
    lastFetching_ = busy;
    dirty_ = true;  // reflect the Loading.../refresh spinner promptly
  }
  const uint32_t gen = news->generation();
  if (gen != lastGen_) {
    lastGen_ = gen;
    dirty_ = true;
    const size_t n = news->count();
    if (mode_ == Mode::List) {
      if (pageStart_ != 0 && pageStart_ >= n) {
        pageStart_ = 0;
      }
    } else if (openIndex_ >= n) {
      mode_ = Mode::List;
    } else {
      // The story behind this index just changed under us — recompose so the
      // page shows (and paginates) the new content, not a stale buffer.
      composeDetail();
    }
  }
}

void NewsApp::render() {
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
    case Mode::Detail:
      renderDetail(gfx);
      break;
  }
  display->markDirty();
}

void NewsApp::renderList(Arduino_GFX& gfx) {
  const SystemState& state = *services_.state;
  NewsService* news = services_.news;
  const size_t n = count();
  const bool busy = news != nullptr && news->fetching();

  // Header: "News (n)" on the left, a refresh button on the right.
  char header[24];
  snprintf(header, sizeof(header), "News (%u)", static_cast<unsigned>(n));
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kAccent);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 8);
  gfx.print(header);

  refreshRect_ = widgets::Rect{static_cast<int16_t>(DISPLAY_WIDTH - theme::kPadding - 96),
                               static_cast<int16_t>(theme::kStatusBarHeight + 2), 96, 30};
  const uint16_t refreshColor = busy ? theme::kTextDim : theme::kAccent;
  gfx.drawRoundRect(refreshRect_.x, refreshRect_.y, refreshRect_.w, refreshRect_.h, 6,
                    refreshColor);
  gfx.setTextColor(refreshColor);
  gfx.setCursor(refreshRect_.x + 6, refreshRect_.y + 8);
  gfx.print(busy ? "..." : "refresh");

  // Subtitle: loading / freshness, marked offline when serving the cache.
  char sub[56];
  if (busy) {
    snprintf(sub, sizeof(sub), "Loading...");
  } else if (news != nullptr) {
    char fresh[48];
    news->freshness(fresh, sizeof(fresh));
    if (!state.internet && n > 0) {
      snprintf(sub, sizeof(sub), "%s - offline", fresh);
    } else {
      snprintf(sub, sizeof(sub), "%s", fresh);
    }
  } else {
    snprintf(sub, sizeof(sub), "unavailable");
  }
  const bool offlineCache = !busy && !state.internet && n > 0;
  gfx.setTextColor(offlineCache ? theme::kWarn : theme::kTextDim);
  gfx.setCursor(theme::kPadding, theme::kStatusBarHeight + 28);
  gfx.print(sub);

  // Honest empty states.
  if (n == 0) {
    const char* msg;
    if (busy) {
      msg = "Fetching headlines...";
    } else if (!state.internet) {
      msg = "No Wi-Fi. Connect a network, then\ntap refresh to load headlines.";
    } else {
      msg = "No headlines yet. Tap refresh.";
    }
    widgets::textBlock(gfx, theme::kPadding, kContentTop + 20, DISPLAY_WIDTH - 2 * theme::kPadding,
                       msg, theme::kTextSizeSmall, theme::kTextDim);
    return;
  }

  int16_t y = kContentTop;
  for (size_t i = 0; i < kPageSize; i++) {
    const size_t idx = pageStart_ + i;
    if (idx >= n) {
      rowRects_[i] = widgets::Rect{};
      continue;
    }
    const Headline& h = news->headline(idx);
    char title[kTitleClip + 4];
    clipText(title, sizeof(title), h.title, kTitleClip);
    // Teaser: the first stretch of the summary. Cached HN-era entries have no
    // summary — show a quiet placeholder instead of an empty gap.
    char secondary[kTeaserClip + 4];
    if (h.summary[0] != '\0') {
      clipText(secondary, sizeof(secondary), h.summary, kTeaserClip);
    } else {
      snprintf(secondary, sizeof(secondary), "(no summary)");
    }
    rowRects_[i] = widgets::listItem(gfx, theme::kPadding, y, DISPLAY_WIDTH - 2 * theme::kPadding,
                                     title, secondary, false);
    y += kRowStride;
  }

  char pager[40];
  snprintf(pager, sizeof(pager), "%u-%u of %u   swipe up/down",
           static_cast<unsigned>(pageStart_ + 1),
           static_cast<unsigned>(min(pageStart_ + kPageSize, n)), static_cast<unsigned>(n));
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
  gfx.print(pager);
}

uint8_t NewsApp::detailFontSize() const {
  static_assert(sizeof(kDetailFontSizes) / sizeof(kDetailFontSizes[0]) == kDetailFontCount,
                "kDetailFontSizes must have kDetailFontCount entries");
  return kDetailFontSizes[fontIdx_ < kDetailFontCount ? fontIdx_ : 0];
}

void NewsApp::composeDetail() {
  const Headline& h = services_.news->headline(openIndex_);
  snprintf(detail_, sizeof(detail_), "%s\n\n%s\n\nLink: %s", h.title,
           h.summary[0] != '\0' ? h.summary : "(no summary for this story)",
           h.url[0] != '\0' ? h.url : "(no article link)");
  detailPage_ = 0;
  layoutDetailPages();
  dirty_ = true;
}

void NewsApp::layoutDetailPages() {
  const uint8_t fs = detailFontSize();
  const uint16_t maxLines = detailMaxLines(fs);
  const size_t len = strlen(detail_);
  detailPages_ = 0;
  size_t from = 0;
  while (from < len && detailPages_ < kMaxDetailPages) {
    pageOffsets_[detailPages_++] = static_cast<uint16_t>(from);
    const size_t next = wrapWalk(nullptr, detail_, from, fs, 0, 0, maxLines, 0);
    if (next <= from) {
      break;  // safety: never loop on a walker that stops advancing
    }
    from = next;
  }
  if (detailPages_ == 0) {
    pageOffsets_[0] = 0;
    detailPages_ = 1;
  }
  if (detailPage_ >= detailPages_) {
    detailPage_ = detailPages_ - 1;
  }
}

void NewsApp::cycleDetailFont() {
  // Page boundaries depend on the font; keep the reading position by re-finding
  // the page that contains the old top-of-page offset (ReaderApp's contract,
  // trivial here because every page start is in pageOffsets_).
  const uint16_t oldOffset = pageOffsets_[detailPage_];
  fontIdx_ = static_cast<uint8_t>((fontIdx_ + 1) % kDetailFontCount);
  detailPage_ = 0;
  layoutDetailPages();
  for (uint8_t i = 0; i < detailPages_; i++) {
    if (pageOffsets_[i] <= oldOffset) {
      detailPage_ = i;
    }
  }
  dirty_ = true;
}

void NewsApp::renderDetail(Arduino_GFX& gfx) {
  const size_t n = count();
  if (openIndex_ >= n) {
    // update()/onResume() normally clamp this; guard so a race never indexes
    // past the end.
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kTextDim);
    gfx.setCursor(theme::kPadding, kContentTop);
    gfx.print("headline no longer available");
    return;
  }

  // Position, right-aligned in the header band.
  char pos[24];
  snprintf(pos, sizeof(pos), "%u / %u", static_cast<unsigned>(openIndex_ + 1),
           static_cast<unsigned>(n));
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(DISPLAY_WIDTH - theme::kPadding - static_cast<int16_t>(strlen(pos)) * 12,
                theme::kStatusBarHeight + 6);
  gfx.print(pos);

  // Current page of the composed title + summary + link text.
  const uint8_t fs = detailFontSize();
  wrapWalk(&gfx, detail_, pageOffsets_[detailPage_], fs, theme::kPadding, kDetailTop,
           detailMaxLines(fs), theme::kText);

  // Footer: page + font on the left, navigation hint on the right.
  char foot[24];
  snprintf(foot, sizeof(foot), "pg %u/%u   A%u", static_cast<unsigned>(detailPage_ + 1),
           static_cast<unsigned>(detailPages_), static_cast<unsigned>(fs));
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
  gfx.print(foot);

  const char* hint = "swipe: prev/next";
  gfx.setTextColor(theme::kPanelAlt);
  gfx.setCursor(DISPLAY_WIDTH - theme::kPadding - static_cast<int16_t>(strlen(hint)) * 12,
                DISPLAY_HEIGHT - 28);
  gfx.print(hint);
}

bool NewsApp::handleList(const InputEvent& event) {
  const size_t n = count();
  switch (event.action) {
    case InputAction::SwipeUp:
      if (pageStart_ + kPageSize < n) {
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
      if (refreshRect_.contains(event.x, event.y)) {
        triggerRefresh();
        return true;
      }
      for (size_t i = 0; i < kPageSize; i++) {
        if (rowRects_[i].contains(event.x, event.y)) {
          const size_t idx = pageStart_ + i;
          if (idx < n) {
            openIndex_ = idx;
            mode_ = Mode::Detail;
            composeDetail();
          }
          return true;
        }
      }
      return true;
    default:
      // Back/Home fall through to the router (leave the app).
      return false;
  }
}

bool NewsApp::handleDetail(const InputEvent& event) {
  const size_t n = count();
  switch (event.action) {
    // Left/right move between stories; up/down (and tap halves, below) page
    // within one story — the two axes stay distinct so neither steals the other.
    case InputAction::SwipeLeft:
      if (openIndex_ + 1 < n) {
        openIndex_++;
        composeDetail();
      }
      return true;
    case InputAction::SwipeRight:
      if (openIndex_ > 0) {
        openIndex_--;
        composeDetail();
      }
      return true;
    case InputAction::SwipeUp:
      if (detailPage_ + 1 < detailPages_) {
        detailPage_++;
        dirty_ = true;
      }
      return true;
    case InputAction::SwipeDown:
      if (detailPage_ > 0) {
        detailPage_--;
        dirty_ = true;
      }
      return true;
    case InputAction::DoubleTap:
      cycleDetailFont();
      return true;
    case InputAction::Tap:
      // Top half = previous page, bottom half = next page (ReaderApp's brief).
      if (event.y >= kDetailTop && event.y < kDetailBottom) {
        if (event.y < kDetailTop + (kDetailBottom - kDetailTop) / 2) {
          if (detailPage_ > 0) {
            detailPage_--;
            dirty_ = true;
          }
        } else if (detailPage_ + 1 < detailPages_) {
          detailPage_++;
          dirty_ = true;
        }
      }
      return true;
    case InputAction::Back:
    case InputAction::Cancel:
      mode_ = Mode::List;
      dirty_ = true;
      return true;
    default:
      return false;
  }
}

bool NewsApp::handleInput(const InputEvent& event) {
  switch (mode_) {
    case Mode::List:
      return handleList(event);
    case Mode::Detail:
      return handleDetail(event);
  }
  return false;
}

void NewsApp::triggerRefresh() {
  if (services_.news != nullptr) {
    if (services_.news->refresh()) {
      Serial.println("[news] manual refresh requested");
    }
  }
  dirty_ = true;
}
