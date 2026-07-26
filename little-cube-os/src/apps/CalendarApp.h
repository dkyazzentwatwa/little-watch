#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../services/CalendarService.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

// Calendar (spec §14): agenda viewing over editing. One day at a time with
// day stepping, an event detail, mark-done, and delete behind a confirm.
// Creating events and typing titles happens over USB serial — there is no
// on-screen keyboard (spec §15).
//
// CalendarService is pull-only and every method that names a date does SD
// I/O, so loading happens in onOpen()/onResume()/handleInput() and never in
// render(). event()/eventCount()/hiddenCount()/clockKnown() are pure RAM.
//
// A cube that has never had its clock set cannot know which day "today" is.
// That is a reachable first-boot state, not an error, and it gets its own
// screen rather than a blank agenda.
class CalendarApp : public App {
 public:
  explicit CalendarApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override;
  void onResume() override;

  void update(uint32_t deltaMs) override { (void)deltaMs; }
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  enum class Screen {
    Agenda,
    Detail,
    ConfirmDelete,
  };

  void go(Screen screen);
  void loadDay(int dayOffset);    // does SD I/O
  void refresh();                 // re-reads the day on screen; does SD I/O
  bool openDetail(size_t index);  // copies out of the cached day; no I/O
  size_t eventCount() const;

  void renderAgenda(Arduino_GFX& gfx);
  void renderNoClock(Arduino_GFX& gfx);
  void renderDetail(Arduino_GFX& gfx);
  void renderConfirmDelete(Arduino_GFX& gfx);
  bool handleAgenda(const InputEvent& event);
  bool handleDetail(const InputEvent& event);
  bool handleConfirmDelete(const InputEvent& event);

  Services& services_;
  StatusBar statusBar_;
  Screen screen_ = Screen::Agenda;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  static constexpr size_t kPageSize = 4;

  int dayOffset_ = 0;  // days from today; 0 = today
  size_t pageStart_ = 0;
  widgets::Rect rowRects_[kPageSize];
  widgets::Rect prevRect_;
  widgets::Rect todayRect_;
  widgets::Rect nextRect_;
  widgets::Rect setupRect_;

  // A copy, not an index: the cached day can be re-read underneath this
  // screen and position 3 would then be a different event.
  CalendarEvent detail_;
  size_t detailIndex_ = 0;
  bool detailLoaded_ = false;

  widgets::Rect doneRect_;
  widgets::Rect deleteRect_;
  widgets::Rect confirmRect_;
  widgets::Rect cancelRect_;
};
