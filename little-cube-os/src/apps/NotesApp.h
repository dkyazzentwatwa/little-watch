#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../services/NotesService.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

// Notes reader (spec §15): browse both note roots, open, scroll, hop
// between notes, resize text, favorite (long-press a list row), delete
// behind an explicit confirm modal. Writing happens over USB serial.
class NotesApp : public App {
 public:
  explicit NotesApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override {}
  void onPause() override {}
  void onResume() override { dirty_ = true; }

  void update(uint32_t deltaMs) override { (void)deltaMs; }
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  enum class Mode {
    List,
    Reading,
    ConfirmDelete,
  };

  void refreshList();
  bool openNote(size_t index);
  void renderList(Arduino_GFX& gfx);
  void renderReading(Arduino_GFX& gfx);
  void renderConfirmDelete(Arduino_GFX& gfx);
  bool handleList(const InputEvent& event);
  bool handleReading(const InputEvent& event);
  bool handleConfirmDelete(const InputEvent& event);

  Services& services_;
  StatusBar statusBar_;
  Mode mode_ = Mode::List;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  static constexpr size_t kMaxNotes = 48;
  static constexpr size_t kPageSize = 5;
  static constexpr size_t kMaxBodyBytes = 12000;

  NoteInfo notes_[kMaxNotes];
  size_t noteCount_ = 0;
  size_t pageStart_ = 0;
  widgets::Rect rowRects_[kPageSize];

  size_t openIndex_ = 0;
  String body_;
  bool bodyTruncated_ = false;
  int16_t scrollY_ = 0;
  uint8_t fontSize_ = 2;
  widgets::Rect deleteRect_;
  widgets::Rect confirmRect_;
  widgets::Rect cancelRect_;
};
