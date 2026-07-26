#include "CalendarApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/AppRouter.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"

namespace {
constexpr int16_t kTop = theme::kStatusBarHeight + 12;
constexpr int16_t kNavTop = theme::kStatusBarHeight + 46;
constexpr int16_t kNavH = 44;
constexpr int16_t kListTop = kNavTop + kNavH + 14;
constexpr int16_t kRowStep = 62;
constexpr int kMaxDayOffset = 3650;  // matches the `calendar` serial family

// Mirrors of widgets::textBlock's geometry — keep in step with
// ui/widgets/Widgets.cpp.
constexpr int16_t kCharW = 6;
constexpr int16_t kCharH = 8;
constexpr int16_t kSmallLineH = kCharH * theme::kTextSizeSmall + 2;
constexpr int16_t kBodyLineH = kCharH * theme::kTextSizeBody + 2;

// Characters that fit inside a listItem row (it insets text by 8 px).
constexpr size_t kRowChars = (DISPLAY_WIDTH - 2 * theme::kPadding - 16) /
                             (kCharW * theme::kTextSizeSmall);

// Single-line fit for a list row. The full text is one tap away on the
// detail screen; ".." says the row is showing less than the whole thing.
void fitLine(char* dst, size_t dstSize, const char* src, size_t maxChars) {
  if (dst == nullptr || dstSize == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  if (maxChars + 1 > dstSize) {
    maxChars = dstSize - 1;
  }
  if (strlen(src) <= maxChars) {
    snprintf(dst, dstSize, "%s", src);
    return;
  }
  const size_t keep = maxChars > 2 ? maxChars - 2 : 0;
  snprintf(dst, dstSize, "%.*s..", (int)keep, src);
}
}  // namespace

void CalendarApp::go(Screen screen) {
  screen_ = screen;
  dirty_ = true;
}

size_t CalendarApp::eventCount() const {
  return services_.calendar != nullptr ? services_.calendar->eventCount() : 0;
}

void CalendarApp::loadDay(int dayOffset) {
  // Same bound the serial family uses: stepping is one day per gesture, but
  // nothing should be able to walk mktime() off into fantasy years.
  if (dayOffset < -kMaxDayOffset) {
    dayOffset = -kMaxDayOffset;
  } else if (dayOffset > kMaxDayOffset) {
    dayOffset = kMaxDayOffset;
  }
  dayOffset_ = dayOffset;
  pageStart_ = 0;
  if (services_.calendar != nullptr) {
    // Returns false with no clock or no card; the render path reads
    // clockKnown()/dayLoaded()/dayCorrupt() and says which it was.
    services_.calendar->loadDay(dayOffset_);
  }
  dirty_ = true;
}

void CalendarApp::refresh() {
  // Re-resolves the offset rather than calling reload(): the date behind
  // "today" moves at midnight, and this app can be left open across it.
  loadDay(dayOffset_);
}

bool CalendarApp::openDetail(size_t index) {
  CalendarService* cal = services_.calendar;
  if (cal == nullptr || index >= cal->eventCount()) {
    return false;
  }
  const CalendarEvent* e = cal->event(index);
  if (e == nullptr) {
    return false;
  }
  detail_ = *e;
  detailIndex_ = index;
  detailLoaded_ = true;
  go(Screen::Detail);
  return true;
}

void CalendarApp::onOpen() {
  screen_ = Screen::Agenda;
  detailLoaded_ = false;
  loadDay(0);
}

void CalendarApp::onPause() {
  // An armed delete confirm must never be waiting under the user's first tap
  // when they come back to this app.
  if (screen_ == Screen::ConfirmDelete) {
    go(Screen::Detail);
  }
}

void CalendarApp::onResume() {
  // `calendar add`/`done`/`delete`/`import` over serial and an SD remount all
  // move the day underneath this screen — and deleting renumbers everything
  // after the hole, so detailIndex_ can point somewhere else entirely.
  refresh();
  if (screen_ != Screen::Agenda) {
    CalendarService* cal = services_.calendar;
    const CalendarEvent* e =
        (detailLoaded_ && cal != nullptr) ? cal->event(detailIndex_) : nullptr;
    // Same slot, same event? Only then is it safe to stay on the detail.
    if (e != nullptr && strcmp(e->title, detail_.title) == 0) {
      detail_ = *e;
    } else {
      detailLoaded_ = false;
      screen_ = Screen::Agenda;
    }
  }
  dirty_ = true;
}

void CalendarApp::onClose() {
  screen_ = Screen::Agenda;
  detailLoaded_ = false;
  dayOffset_ = 0;
  pageStart_ = 0;
}

void CalendarApp::render() {
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

  const bool clockKnown = services_.calendar != nullptr && services_.calendar->clockKnown();
  switch (screen_) {
    case Screen::Agenda:
      if (clockKnown) {
        renderAgenda(gfx);
      } else {
        renderNoClock(gfx);
      }
      break;
    case Screen::Detail:
      renderDetail(gfx);
      break;
    case Screen::ConfirmDelete:
      renderDetail(gfx);
      renderConfirmDelete(gfx);
      break;
  }
  display->markDirty();
}

// First boot with no network and no `time set`: there is no "today" to show
// an agenda for. Saying so — and naming both ways out — beats an empty list
// that looks like an empty calendar.
void CalendarApp::renderNoClock(Arduino_GFX& gfx) {
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;

  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Calendar");

  widgets::textBlock(gfx, theme::kPadding, kTop + 44, w,
                     "The clock has never been set, so the cube does not know what today is.\n\n"
                     "Connect Wi-Fi and the time arrives on its own, or set it over USB serial:",
                     theme::kTextSizeSmall, theme::kTextDim);

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kAccent);
  gfx.setCursor(theme::kPadding, kTop + 200);
  gfx.print("time set 2026-07-25 09:30");
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, kTop + 226);
  gfx.print("calendar list 2026-07-25");

  setupRect_ = widgets::button(gfx, theme::kPadding, DISPLAY_HEIGHT - 12 - 58, w, 58,
                               "Wi-Fi setup", true);
}

void CalendarApp::renderAgenda(Arduino_GFX& gfx) {
  CalendarService* cal = services_.calendar;
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;

  char label[24];
  if (!CalendarService::formatDayLabel(cal->loadedDate(), label, sizeof(label))) {
    snprintf(label, sizeof(label), "%s", cal->loadedDate());
  }
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print(label);

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(dayOffset_ == 0 ? theme::kAccent : theme::kTextDim);
  gfx.setCursor(DISPLAY_WIDTH - theme::kPadding - 7 * kCharW * theme::kTextSizeSmall, kTop + 4);
  if (dayOffset_ == 0) {
    gfx.print("today");
  } else {
    char rel[12];
    snprintf(rel, sizeof(rel), "%+d d", dayOffset_);
    gfx.print(rel);
  }

  const int16_t navW = (w - 16) / 3;
  prevRect_ = widgets::button(gfx, theme::kPadding, kNavTop, navW, kNavH, "<", false);
  // Doubles as the manual refresh (spec §14) — it always re-reads the file.
  todayRect_ = widgets::button(gfx, theme::kPadding + navW + 8, kNavTop, navW, kNavH, "today",
                               dayOffset_ == 0);
  nextRect_ = widgets::button(gfx, theme::kPadding + 2 * (navW + 8), kNavTop, navW, kNavH, ">",
                              false);

  for (size_t i = 0; i < kPageSize; i++) {
    rowRects_[i] = widgets::Rect{};
  }

  // Bad JSON latches and every mutation on that day is refused, so an empty
  // agenda here would be a lie about the user's data.
  if (cal->dayCorrupt()) {
    widgets::textBlock(gfx, theme::kPadding, kListTop, w,
                       "This day's file could not be read. It was left untouched - nothing was "
                       "lost. Fix or remove it under /littlecube/calendar, then tap today.",
                       theme::kTextSizeSmall, theme::kWarn);
    return;
  }

  if (!cal->dayLoaded()) {
    const bool sdOk = services_.state->sd == SdCardState::Mounted ||
                      services_.state->sd == SdCardState::ReadOnly;
    widgets::textBlock(gfx, theme::kPadding, kListTop, w,
                       sdOk ? "Could not read this day. Tap today to try again."
                            : "Insert an SD card to read the calendar.",
                       theme::kTextSizeSmall, theme::kTextDim);
    return;
  }

  if (cal->eventTotal() == 0) {
    widgets::textBlock(gfx, theme::kPadding, kListTop, w,
                       "Nothing on this day. Add one over USB serial:\n"
                       "calendar add today 09:30 \"Standup\"",
                       theme::kTextSizeSmall, theme::kTextDim);
    return;
  }

  const size_t count = cal->eventCount();
  int16_t y = kListTop;
  for (size_t i = 0; i < kPageSize; i++) {
    const size_t idx = pageStart_ + i;
    if (idx >= count) {
      continue;
    }
    const CalendarEvent* e = cal->event(idx);
    if (e == nullptr) {
      continue;
    }
    char raw[80];
    char primary[48];
    snprintf(raw, sizeof(raw), "%s  %s", e->timed() ? e->time : "--:--", e->title);
    fitLine(primary, sizeof(primary), raw, kRowChars);

    char secondary[48];
    const char* flag = e->done ? "done" : (e->timed() ? "" : "all day");
    if (flag[0] != '\0' && e->note[0] != '\0') {
      snprintf(raw, sizeof(raw), "%s - %s", flag, e->note);
    } else if (flag[0] != '\0') {
      snprintf(raw, sizeof(raw), "%s", flag);
    } else if (e->note[0] != '\0') {
      snprintf(raw, sizeof(raw), "%s", e->note);
    } else {
      snprintf(raw, sizeof(raw), "%s", "-");
    }
    fitLine(secondary, sizeof(secondary), raw, kRowChars);

    rowRects_[i] = widgets::listItem(gfx, theme::kPadding, y, w, primary, secondary, false);
    if (e->done) {
      gfx.fillCircle(DISPLAY_WIDTH - theme::kPadding - 8, y + 12, 4, theme::kGood);
    }
    y += kRowStep;
  }

  char footer[64];
  const size_t last = pageStart_ + kPageSize < count ? pageStart_ + kPageSize : count;
  if (cal->hiddenCount() > 0) {
    // The day holds more than the cache window. Name the number rather than
    // stopping the list without a word.
    snprintf(footer, sizeof(footer), "%u-%u of %u  +%u more on the card",
             (unsigned)(pageStart_ + 1), (unsigned)last, (unsigned)count,
             (unsigned)cal->hiddenCount());
  } else {
    snprintf(footer, sizeof(footer), "%u-%u of %u  %u to do  swipe L/R = day",
             (unsigned)(pageStart_ + 1), (unsigned)last, (unsigned)count,
             (unsigned)cal->pendingCount());
  }
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
  gfx.print(footer);
}

void CalendarApp::renderDetail(Arduino_GFX& gfx) {
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  CalendarService* cal = services_.calendar;

  char label[24];
  const char* iso = cal != nullptr ? cal->loadedDate() : "";
  if (!CalendarService::formatDayLabel(iso, label, sizeof(label))) {
    snprintf(label, sizeof(label), "%s", iso);
  }
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print(label);

  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kAccent);
  gfx.setCursor(theme::kPadding, kTop + kSmallLineH + 6);
  gfx.print(detail_.timed() ? detail_.time : "all day");

  int16_t y = kTop + kSmallLineH + 6 + kBodyLineH + 10;
  widgets::textBlock(gfx, theme::kPadding, y, w, detail_.title, theme::kTextSizeBody,
                     detail_.done ? theme::kTextDim : theme::kText);
  y += 4 * kBodyLineH;

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("note");
  widgets::textBlock(gfx, theme::kPadding, y + kSmallLineH + 4, w,
                     detail_.note[0] != '\0' ? detail_.note : "-", theme::kTextSizeSmall,
                     theme::kText);
  y += kSmallLineH + 4 + 4 * kSmallLineH;

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(detail_.done ? theme::kGood : theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print(detail_.done ? "done" : "not done yet");

  const int16_t by = DISPLAY_HEIGHT - 12 - 52;
  doneRect_ = widgets::button(gfx, theme::kPadding, by, (w - 8) / 2, 52,
                              detail_.done ? "not done" : "mark done", !detail_.done);
  deleteRect_ = widgets::button(gfx, theme::kPadding + (w + 8) / 2, by, (w - 8) / 2, 52, "delete",
                                false);
}

void CalendarApp::renderConfirmDelete(Arduino_GFX& gfx) {
  char body[96];
  snprintf(body, sizeof(body), "\"%s\" is removed from %s on the card.", detail_.title,
           services_.calendar != nullptr ? services_.calendar->loadedDate() : "this day");
  confirmRect_ = widgets::modalConfirm(gfx, "Delete event?", body, cancelRect_);
}

bool CalendarApp::handleAgenda(const InputEvent& event) {
  CalendarService* cal = services_.calendar;
  const bool clockKnown = cal != nullptr && cal->clockKnown();

  if (!clockKnown) {
    if (event.action == InputAction::Tap && setupRect_.contains(event.x, event.y) &&
        services_.router != nullptr) {
      // Wi-Fi is the no-typing route to a correct clock; Settings is where it
      // lives. The serial route is printed on the same screen.
      services_.router->open(AppId::Settings);
      return true;
    }
    if (event.action == InputAction::Tap) {
      // The clock may have arrived (SNTP) since this screen was drawn.
      refresh();
      return true;
    }
    return false;  // Back/Home fall through to the router
  }

  switch (event.action) {
    case InputAction::SwipeLeft:
      loadDay(dayOffset_ + 1);
      return true;
    case InputAction::SwipeRight:
      loadDay(dayOffset_ - 1);
      return true;
    case InputAction::SwipeUp:
      if (pageStart_ + kPageSize < eventCount()) {
        pageStart_ += kPageSize;
        dirty_ = true;
      }
      return true;
    case InputAction::SwipeDown:
      pageStart_ = pageStart_ >= kPageSize ? pageStart_ - kPageSize : 0;
      dirty_ = true;
      return true;
    case InputAction::Tap:
      if (prevRect_.contains(event.x, event.y)) {
        loadDay(dayOffset_ - 1);
        return true;
      }
      if (nextRect_.contains(event.x, event.y)) {
        loadDay(dayOffset_ + 1);
        return true;
      }
      if (todayRect_.contains(event.x, event.y)) {
        loadDay(0);  // also the manual refresh
        return true;
      }
      for (size_t i = 0; i < kPageSize; i++) {
        if (rowRects_[i].w > 0 && rowRects_[i].contains(event.x, event.y)) {
          openDetail(pageStart_ + i);
          return true;
        }
      }
      return true;
    default:
      return false;
  }
}

bool CalendarApp::handleDetail(const InputEvent& event) {
  switch (event.action) {
    case InputAction::Tap: {
      CalendarService* cal = services_.calendar;
      if (doneRect_.contains(event.x, event.y)) {
        if (cal != nullptr) {
          // Re-read rather than flipping the local copy: a refused mutation
          // (corrupt day file, read-only card) must not leave the screen
          // claiming a state the card does not hold.
          cal->setDone(detailIndex_, !detail_.done);
          cal->reload();
          const CalendarEvent* e = cal->event(detailIndex_);
          if (e != nullptr) {
            detail_ = *e;
          }
        }
        dirty_ = true;
        return true;
      }
      if (deleteRect_.contains(event.x, event.y)) {
        go(Screen::ConfirmDelete);
        return true;
      }
      return true;
    }
    case InputAction::SwipeRight:
    case InputAction::Back:
    case InputAction::Cancel:
      detailLoaded_ = false;
      go(Screen::Agenda);
      return true;
    default:
      return false;
  }
}

bool CalendarApp::handleConfirmDelete(const InputEvent& event) {
  if (event.action == InputAction::Tap) {
    if (confirmRect_.contains(event.x, event.y)) {
      if (services_.calendar != nullptr) {
        services_.calendar->removeEvent(detailIndex_);
      }
      // Everything after the hole renumbers, so the agenda is the only safe
      // place to land.
      refresh();
      detailLoaded_ = false;
      go(Screen::Agenda);
      return true;
    }
    if (cancelRect_.contains(event.x, event.y)) {
      go(Screen::Detail);
    }
    return true;  // absorb every other tap while the modal is up
  }
  if (event.action == InputAction::Back || event.action == InputAction::Cancel) {
    go(Screen::Detail);
    return true;
  }
  return true;
}

bool CalendarApp::handleInput(const InputEvent& event) {
  switch (screen_) {
    case Screen::Agenda:
      return handleAgenda(event);
    case Screen::Detail:
      return handleDetail(event);
    case Screen::ConfirmDelete:
      return handleConfirmDelete(event);
  }
  return false;
}
