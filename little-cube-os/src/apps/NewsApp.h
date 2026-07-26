#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../services/NewsService.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

// World headlines (BBC News RSS) fetched over Wi-Fi by NewsService, cached to
// flash, read here as a paged list — a close cousin of NotesApp/ReaderApp. Two
// modes: a paged List of headlines (title clipped to one row, a dim summary
// teaser line), and a Detail that reads like a tiny ReaderApp page: the title,
// full summary, and article link (the cube can't browse — the URL is shown for
// reference) as one wrapped text, paginated by tap/swipe, with double-tap
// cycling the font size just like the reader. A header "refresh" button
// re-pulls. Honest offline / loading / empty states; no filesystem or network
// work in render().
class NewsApp : public App {
 public:
  explicit NewsApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override;
  void onResume() override;

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  enum class Mode { List, Detail };

  void renderList(Arduino_GFX& gfx);
  void renderDetail(Arduino_GFX& gfx);
  bool handleList(const InputEvent& event);
  bool handleDetail(const InputEvent& event);
  void triggerRefresh();
  size_t count() const;

  // Detail pagination (ReaderApp in miniature, but the "book" is a composed
  // in-RAM buffer, so page starts are just recomputed offsets — no back stack).
  void composeDetail();      // rebuild detail_ + pages for headlines_[openIndex_]
  void layoutDetailPages();  // recompute pageOffsets_ for the current font
  void cycleDetailFont();    // double-tap: next size, keep the reading position
  uint8_t detailFontSize() const;

  Services& services_;
  StatusBar statusBar_;
  Mode mode_ = Mode::List;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  // Change detection so a background pull or a loading transition redraws
  // promptly instead of waiting on the ~1 Hz SystemState version bump.
  uint32_t lastGen_ = 0xFFFFFFFF;
  bool lastFetching_ = false;

  static constexpr size_t kPageSize = 5;
  size_t pageStart_ = 0;
  widgets::Rect rowRects_[kPageSize];
  widgets::Rect refreshRect_;

  size_t openIndex_ = 0;  // headline shown in Detail mode

  // Detail: title + summary + link composed into one wrapped text, paginated
  // against the current font exactly the way it is rendered (one shared wrap
  // walker), so the two can never disagree.
  static constexpr size_t kDetailCap = 128 + 256 + 160 + 32;
  static constexpr size_t kMaxDetailPages = 24;
  static constexpr uint8_t kDetailFontCount = 3;
  char detail_[kDetailCap] = "";
  uint16_t pageOffsets_[kMaxDetailPages] = {0};
  uint8_t detailPages_ = 1;
  uint8_t detailPage_ = 0;
  uint8_t fontIdx_ = 0;  // index into the size table in the .cpp (RAM-only, like Reader)
};
