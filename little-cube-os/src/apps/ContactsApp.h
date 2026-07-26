#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../services/ContactsService.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

// Contacts (spec §26): reference-first. List with favourites first, a detail
// screen, favourite toggle, delete behind a confirm. No dialling and no
// messaging — the cube never implies it can call anyone.
//
// There is no on-device search: searching needs text entry, and this device
// has no keyboard (spec §15). The list footer points at `contacts search`
// over USB serial instead of pretending otherwise.
//
// ContactsService is pull-only, so every method that touches the card is
// called from onOpen()/onResume()/handleInput() — never from render(), which
// runs every frame. at()/cached()/total()/hiddenCount() are pure RAM.
class ContactsApp : public App {
 public:
  explicit ContactsApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override;
  void onResume() override;

  void update(uint32_t deltaMs) override { (void)deltaMs; }
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  enum class Screen {
    List,
    Detail,
    ConfirmDelete,
  };

  void refresh();                  // does SD I/O
  bool openDetail(size_t index);   // does SD I/O
  void backToList();
  size_t listCount() const;

  void renderList(Arduino_GFX& gfx);
  void renderDetail(Arduino_GFX& gfx);
  void renderConfirmDelete(Arduino_GFX& gfx);
  bool handleList(const InputEvent& event);
  bool handleDetail(const InputEvent& event);
  bool handleConfirmDelete(const InputEvent& event);

  Services& services_;
  StatusBar statusBar_;
  Screen screen_ = Screen::List;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  static constexpr size_t kPageSize = 5;

  size_t pageStart_ = 0;
  widgets::Rect rowRects_[kPageSize];

  // The open record is copied out of the service: the cache can be re-sorted
  // or re-read underneath this screen, and a row index would then point at a
  // different person.
  ContactDetail detail_;
  bool detailLoaded_ = false;

  widgets::Rect favRect_;
  widgets::Rect deleteRect_;
  widgets::Rect confirmRect_;
  widgets::Rect cancelRect_;
};
