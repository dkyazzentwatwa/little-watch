#include "NewsApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../ui/AmoledProtection.h"
#include "../ui/QrCode.h"
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
// Footer band starts here. widgets::footer() puts its rule at
// DISPLAY_HEIGHT - 44 and returns a budget 12 px above that, so this must
// not exceed 392. The old value (DISPLAY_HEIGHT - 36 = 412) ran 8 px PAST
// the shared rule, so body text drew over the hairline.
constexpr int16_t kDetailBottom = DISPLAY_HEIGHT - 56;
static_assert(kDetailBottom <= DISPLAY_HEIGHT - 44 - 12,
              "detail text must stop above widgets::footer()'s content budget");
constexpr int16_t kDetailBodyW = DISPLAY_WIDTH - 2 * theme::kPadding;

// QR page geometry, laid out inside the same [kDetailTop, kDetailBottom] band
// the text pages use: a 260 px code, then the caption and host beneath it.
constexpr int16_t kQrBox = 260;
constexpr int16_t kQrX = (DISPLAY_WIDTH - kQrBox) / 2;
constexpr int16_t kQrY = kDetailTop + 4;                 // 62 .. 322
constexpr int16_t kQrCaptionY = kQrY + kQrBox + 8;       // 330, Body (29 tall)
constexpr int16_t kQrHostY = kQrCaptionY + 30;           // 360, Caption (22 tall)
static_assert(kQrHostY + 22 <= kDetailBottom, "QR page must fit above the footer band");

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

// Query keys that carry no routing information — dropping them shortens the
// URL, which lowers the QR version and so widens every module. BBC's RSS
// links arrive as "...?at_medium=RSS&at_campaign=rss", which is ~27 wasted
// characters, enough to cost a version step.
bool isTrackerKey(const char* key, size_t len) {
  if (len > 3 && strncmp(key, "at_", 3) == 0) {
    return true;
  }
  static const char* const kKeys[] = {"utm_source", "utm_medium", "utm_campaign",
                                      "utm_term",   "utm_content", "ref",
                                      "fbclid"};
  for (const char* k : kKeys) {
    if (strlen(k) == len && strncmp(key, k, len) == 0) {
      return true;
    }
  }
  return false;
}

// Copy src into dst, dropping the query string ONLY when every key in it is a
// known tracker. Deliberately conservative: guessing wrong here yields a
// tidier code that 404s, and a denser QR that resolves beats that every time.
void stripTrackers(char* dst, size_t cap, const char* src) {
  if (cap == 0) {
    return;
  }
  const char* query = strchr(src, '?');
  size_t keep = query != nullptr ? static_cast<size_t>(query - src) : strlen(src);
  if (query != nullptr) {
    bool allTrackers = true;
    for (const char* p = query + 1; *p != '\0';) {
      const char* amp = strchr(p, '&');
      const char* end = amp != nullptr ? amp : p + strlen(p);
      const char* eq =
          static_cast<const char*>(memchr(p, '=', static_cast<size_t>(end - p)));
      const size_t keyLen = static_cast<size_t>((eq != nullptr ? eq : end) - p);
      if (keyLen == 0 || !isTrackerKey(p, keyLen)) {
        allTrackers = false;
        break;
      }
      if (amp == nullptr) {
        break;
      }
      p = amp + 1;
    }
    if (!allTrackers) {
      keep = strlen(src);  // keep the query whole
    }
  }
  if (keep > cap - 1) {
    keep = cap - 1;
  }
  memcpy(dst, src, keep);
  dst[keep] = '\0';
}

// "https://www.bbc.co.uk/news/articles/c123" -> "bbc.co.uk". Shown under the
// QR so the user can see where the code points before scanning it.
void hostOf(char* dst, size_t cap, const char* url) {
  if (cap == 0) {
    return;
  }
  const char* p = strstr(url, "://");
  p = p != nullptr ? p + 3 : url;
  if (strncmp(p, "www.", 4) == 0) {
    p += 4;
  }
  size_t len = 0;
  while (p[len] != '\0' && p[len] != '/' && p[len] != ':' && p[len] != '?') {
    len++;
  }
  if (len > cap - 1) {
    len = cap - 1;
  }
  memcpy(dst, p, len);
  dst[len] = '\0';
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
  // 64, not 56: freshness() writes up to 47 chars and " - offline" adds 10,
  // so 56 could truncate (and did warn). Sized to hold the longest result.
  char sub[64];
  static_assert(sizeof(sub) >= 48 + sizeof(" - offline"),
                "sub must hold the widest freshness string plus the offline suffix");
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

  // Shared footer: status left (ellipsizes), action hint right (wins the
  // space), rule 44 px up so the text clears the bezel's corner radius, and
  // drifting with the burn-in offsets (spec §37) like all persistent chrome.
  char pager[40];
  snprintf(pager, sizeof(pager), "%u-%u of %u", static_cast<unsigned>(pageStart_ + 1),
           static_cast<unsigned>(min(pageStart_ + kPageSize, n)), static_cast<unsigned>(n));
  widgets::footer(gfx, pager, "swipe up/down", services_.amoled->shiftX(),
                  services_.amoled->shiftY());
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
  // QR payload, built here and only here: render() must never encode. The
  // text page above keeps the COMPLETE url; only the encoded copy is stripped.
  stripTrackers(qrUrl_, sizeof(qrUrl_), h.url);
  hostOf(qrHost_, sizeof(qrHost_), h.url);
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
  // Upper bound is detailPages_, not detailPages_ - 1: the QR page sits one
  // past the last text page.
  if (detailPage_ > detailPages_) {
    detailPage_ = detailPages_;
  }
}

void NewsApp::cycleDetailFont() {
  // Page boundaries depend on the font; keep the reading position by re-finding
  // the page that contains the old top-of-page offset (ReaderApp's contract,
  // trivial here because every page start is in pageOffsets_).
  const bool onQr = detailPage_ >= detailPages_;
  const uint16_t oldOffset = onQr ? 0 : pageOffsets_[detailPage_];
  fontIdx_ = static_cast<uint8_t>((fontIdx_ + 1) % kDetailFontCount);
  detailPage_ = 0;
  layoutDetailPages();
  if (onQr) {
    // The QR page has no text to re-flow — it is the same page at any font,
    // just at a new index now that the text ahead of it repaginated. Reading
    // pageOffsets_[detailPages_] here would index past the last written entry.
    detailPage_ = detailPages_;
  } else {
    for (uint8_t i = 0; i < detailPages_; i++) {
      if (pageOffsets_[i] <= oldOffset) {
        detailPage_ = i;
      }
    }
  }
  dirty_ = true;
}

// The last page of every story: the article link as a QR, so the phone in the
// user's other hand can open what the cube can't browse. Everything drawn here
// comes from buffers composeDetail() filled — no encoding, no allocation, no
// I/O, because this runs on every SystemState version bump for as long as the
// page stays open.
void NewsApp::renderQrPage(Arduino_GFX& gfx, const char* fullUrl) {
  const int16_t sx = services_.amoled->shiftX();
  const int16_t sy = services_.amoled->shiftY();

  // bg MUST be light and is checked by qrcode::draw(): scanners need dark
  // modules on a light field, and half this firmware's palettes are dark, so
  // theme::kBg would be rejected outright. White/black, always.
  const bool drawn = qrUrl_[0] != '\0' && qrcode::draw(gfx, kQrX + sx, kQrY + sy, kQrBox,
                                                       qrUrl_, 0x0000, 0xFFFF);
  if (drawn) {
    // Centred inside the safe band, not the raw panel width: at shiftX = +2 a
    // full-width centring box would itself hang 2 px off the right edge.
    constexpr int16_t kCapW = DISPLAY_WIDTH - 2 * theme::kSafeInset;
    widgets::textCentered(gfx, theme::kSafeInset + sx, kQrCaptionY + sy, kCapW, "scan to open",
                          widgets::TextStyle::Body, theme::kText);
    widgets::textCentered(gfx, theme::kSafeInset + sx, kQrHostY + sy, kCapW,
                          qrHost_[0] != '\0' ? qrHost_ : "unknown host",
                          widgets::TextStyle::Caption, theme::kTextDim);
    return;
  }

  // Degraded, never blank: say why there is no code and show the link itself
  // so the page still carries the same information.
  const char* why = qrUrl_[0] == '\0' ? "no article link" : "link too long to encode";
  widgets::text(gfx, theme::kSafeInset + sx, kDetailTop + 8 + sy, why, widgets::TextStyle::Body,
                theme::kWarn);
  if (fullUrl != nullptr && fullUrl[0] != '\0') {
    widgets::textBlock(gfx, theme::kSafeInset + sx, kDetailTop + 44 + sy,
                       DISPLAY_WIDTH - 2 * theme::kSafeInset, fullUrl,
                       widgets::TextStyle::Caption, theme::kTextDim,
                       static_cast<uint8_t>((kDetailBottom - kDetailTop - 44) /
                                            widgets::lineHeight(widgets::TextStyle::Caption)));
  }
}

void NewsApp::renderDetail(Arduino_GFX& gfx) {
  const int16_t sx = services_.amoled->shiftX();
  const int16_t sy = services_.amoled->shiftY();
  const size_t n = count();
  if (openIndex_ >= n) {
    // update()/onResume() normally clamp this; guard so a race never indexes
    // past the end.
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kTextDim);
    gfx.setCursor(theme::kSafeInset + sx, kContentTop + sy);
    gfx.print("headline no longer available");
    widgets::footer(gfx, "no story", "back", sx, sy);
    return;
  }

  // Position, right-aligned in the header band.
  char pos[24];
  snprintf(pos, sizeof(pos), "%u / %u", static_cast<unsigned>(openIndex_ + 1),
           static_cast<unsigned>(n));
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(DISPLAY_WIDTH - theme::kPadding - static_cast<int16_t>(strlen(pos)) * 12 + sx,
                theme::kStatusBarHeight + 6 + sy);
  gfx.print(pos);

  // Pages [0, detailPages_) are the composed title + summary + link text;
  // detailPages_ itself is the QR.
  const uint8_t fs = detailFontSize();
  if (detailPage_ < detailPages_) {
    wrapWalk(&gfx, detail_, pageOffsets_[detailPage_], fs, theme::kPadding + sx, kDetailTop + sy,
             detailMaxLines(fs), theme::kText);
  } else {
    renderQrPage(gfx, services_.news->headline(openIndex_).url);
  }

  // Shared footer: page + font on the left (yields, ellipsizes), navigation
  // hint on the right (wins the space). Both in kTextDim — theme::kPanelAlt,
  // which the old hand-placed hint used, is a FILL colour: 1.24-1.40:1 against
  // kBg on every palette, i.e. invisible ink.
  char foot[32];
  snprintf(foot, sizeof(foot), "pg %u/%u  A%u", static_cast<unsigned>(detailPage_ + 1),
           static_cast<unsigned>(detailPages_ + 1), static_cast<unsigned>(fs));
  widgets::footer(gfx, foot, "swipe: prev/next", sx, sy);
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
    // The pager runs to detailPages_ inclusive — one past the last text page
    // is the QR page, so "next" stops at detailPages_, not detailPages_ - 1.
    case InputAction::SwipeUp:
      if (detailPage_ < detailPages_) {
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
        } else if (detailPage_ < detailPages_) {
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
