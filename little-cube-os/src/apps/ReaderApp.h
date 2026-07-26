#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../services/BookService.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

// E-reader for plain-text books (spec §25 documents; a close cousin of NotesApp).
// Two modes: a paged book List over /littlecube/documents, and a Reading mode
// that paginates one screen at a time by streaming from the SD card — the whole
// book is NEVER loaded, so a multi-megabyte book costs the same RAM as a small
// one. Reading resumes where it was left off, per book. Double-tap cycles font
// size; a thin bar and a percentage show progress by byte offset.
class ReaderApp : public App {
 public:
  explicit ReaderApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override;
  void onResume() override;

  void update(uint32_t deltaMs) override { (void)deltaMs; }
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  enum class Mode {
    List,
    Reading,
  };

  void refreshList();
  bool openBook(size_t index);
  // Reads the current page (curOffset_) into pageBuf_. Called on open, page
  // turn, font change and resume — NEVER from render(), which only draws the
  // buffer (render() runs every loop iteration and must not touch the card).
  void layoutPage();
  void nextPage();
  void prevPage();
  void cycleFont();
  void returnToList();
  void saveResume();
  void pushBack(uint32_t offset);

  uint8_t fontSize() const;

  void renderList(Arduino_GFX& gfx);
  void renderReading(Arduino_GFX& gfx);
  bool handleList(const InputEvent& event);
  bool handleReading(const InputEvent& event);

  Services& services_;
  StatusBar statusBar_;
  Mode mode_ = Mode::List;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  // Book list window (fixed cap, honest "+N more" like NotesApp).
  static constexpr size_t kMaxBooks = 48;
  static constexpr size_t kPageSize = 5;
  BookInfo books_[kMaxBooks];
  size_t bookCount_ = 0;
  size_t bookTotal_ = 0;  // books on the card, which can exceed kMaxBooks
  size_t listStart_ = 0;
  widgets::Rect rowRects_[kPageSize];

  // Open book + current page. openPath_/openTitle_ survive onPause so onResume
  // can re-lay the same page after the data behind it may have moved.
  char openPath_[128] = "";
  char openTitle_[64] = "";
  size_t openIndex_ = 0;
  uint32_t bookSize_ = 0;
  uint32_t curOffset_ = 0;   // byte offset of the top of the current page
  uint32_t nextOffset_ = 0;  // byte offset where the next page begins
  bool atEnd_ = false;
  bool readError_ = false;

  // One page of text, streamed in. This is the ONLY book content held; the
  // whole point of the design is that it is bounded regardless of book size.
  static constexpr size_t kPageBufCap = 3072;
  char pageBuf_[kPageBufCap] = "";
  size_t pageBytes_ = 0;

  // Back-paging: a bounded stack of page-start offsets pushed going forward,
  // popped going back. When full the oldest is dropped so recent back-paging
  // always works and RAM stays capped (kMaxBackStack * 4 bytes).
  static constexpr size_t kMaxBackStack = 256;
  uint32_t backStack_[kMaxBackStack] = {0};
  size_t backDepth_ = 0;

  // Font size is an index into a fixed table (defined in the .cpp); not
  // persisted to flash (survives app reopen in RAM, resets to default on
  // reboot — like NotesApp).
  static constexpr uint8_t kFontCount = 3;
  uint8_t fontIdx_ = 0;

  widgets::Rect backRect_;
};
