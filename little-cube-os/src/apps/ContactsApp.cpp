#include "ContactsApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"

namespace {
constexpr int16_t kTop = theme::kStatusBarHeight + 12;
constexpr int16_t kListTop = theme::kStatusBarHeight + 44;
constexpr int16_t kRowStep = 62;

// Mirrors of widgets::textBlock's geometry — keep in step with
// ui/widgets/Widgets.cpp.
constexpr int16_t kCharW = 6;
constexpr int16_t kCharH = 8;
constexpr int16_t kWrapLineMax = 95;  // its internal line[96], minus the NUL

constexpr int16_t kSmallLineH = kCharH * theme::kTextSizeSmall + 2;
constexpr int16_t kBodyLineH = kCharH * theme::kTextSizeBody + 2;
constexpr int16_t kNoteLines = 5;

int16_t charsPerLine(uint8_t textSize) {
  return (DISPLAY_WIDTH - 2 * theme::kPadding) / (kCharW * textSize);
}

// Byte offset just past the first maxLines lines widgets::textBlock would
// draw for `text`. Equal to strlen(text) when the whole string fits, so the
// caller can tell truncation from a clean fit and say so on screen.
size_t wrapPrefix(const char* text, int16_t perLine, int16_t maxLines) {
  if (text == nullptr || perLine <= 0) {
    return 0;
  }
  const char* p = text;
  int16_t lines = 0;
  while (*p != '\0' && lines < maxLines) {
    int16_t take = 0;
    int16_t lastSpace = -1;
    while (p[take] != '\0' && p[take] != '\n' && take < perLine && take < kWrapLineMax) {
      if (p[take] == ' ') {
        lastSpace = take;
      }
      take++;
    }
    int16_t lineLen = take;
    if (p[take] != '\0' && p[take] != '\n' && lastSpace > 0) {
      lineLen = lastSpace;
    }
    lines++;
    p += lineLen;
    while (*p == ' ') {
      p++;
    }
    if (*p == '\n') {
      p++;
    }
  }
  return static_cast<size_t>(p - text);
}

// Single-line fit for a list row. The full value is always one tap away on
// the detail screen, and the ".." says the row is short of the whole thing.
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

// Characters that fit inside a listItem row (it insets text by 8 px).
constexpr size_t kRowChars = (DISPLAY_WIDTH - 2 * theme::kPadding - 16) /
                             (kCharW * theme::kTextSizeSmall);
}  // namespace

void ContactsApp::onOpen() {
  screen_ = Screen::List;
  pageStart_ = 0;
  detailLoaded_ = false;
  refresh();
  dirty_ = true;
}

void ContactsApp::onPause() {
  // An armed delete confirm must never be sitting under the user's first tap
  // when they come back to this app.
  if (screen_ == Screen::ConfirmDelete) {
    screen_ = Screen::Detail;
    dirty_ = true;
  }
}

void ContactsApp::onResume() {
  // `contacts add`/`delete`/`import` over serial and an SD remount all move
  // the address book while this app is backgrounded, and the open record may
  // be gone entirely.
  refresh();
  if (screen_ != Screen::List && detailLoaded_) {
    if (services_.contacts == nullptr || !services_.contacts->get(detail_.id, detail_)) {
      backToList();
    }
  } else if (screen_ != Screen::List) {
    backToList();
  }
  dirty_ = true;
}

void ContactsApp::onClose() {
  screen_ = Screen::List;
  detailLoaded_ = false;
  detail_ = ContactDetail();
  pageStart_ = 0;
}

size_t ContactsApp::listCount() const {
  return services_.contacts != nullptr ? services_.contacts->cached() : 0;
}

void ContactsApp::refresh() {
  if (services_.contacts != nullptr) {
    services_.contacts->reload();
  }
  if (pageStart_ >= listCount()) {
    pageStart_ = 0;
  }
}

bool ContactsApp::openDetail(size_t index) {
  ContactsService* contacts = services_.contacts;
  if (contacts == nullptr || index >= contacts->cached()) {
    return false;
  }
  const ContactInfo* row = contacts->at(index);
  if (row == nullptr) {
    return false;
  }
  if (!contacts->get(row->id, detail_)) {
    return false;
  }
  detailLoaded_ = true;
  screen_ = Screen::Detail;
  dirty_ = true;
  return true;
}

void ContactsApp::backToList() {
  screen_ = Screen::List;
  detailLoaded_ = false;
  dirty_ = true;
}

void ContactsApp::render() {
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

  switch (screen_) {
    case Screen::List:
      renderList(gfx);
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

void ContactsApp::renderList(Arduino_GFX& gfx) {
  ContactsService* contacts = services_.contacts;
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;

  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Contacts");

  for (size_t i = 0; i < kPageSize; i++) {
    rowRects_[i] = widgets::Rect{};
  }

  if (contacts == nullptr) {
    widgets::textBlock(gfx, theme::kPadding, kListTop + 20, w, "Contacts are unavailable.",
                       theme::kTextSizeSmall, theme::kTextDim);
    return;
  }

  // A corrupt address book is a state, not an absence: showing an empty list
  // would read as "you have no contacts" and invite the user to add one over
  // a file that every mutation is refusing to touch.
  if (contacts->corrupt()) {
    widgets::textBlock(gfx, theme::kPadding, kListTop + 12, w,
                       "contacts.json could not be read. It was left untouched - nothing was "
                       "deleted. Fix or remove /littlecube/contacts/contacts.json, then reopen "
                       "this app.",
                       theme::kTextSizeSmall, theme::kWarn);
    return;
  }

  if (!contacts->loaded()) {
    const bool sdOk = services_.state->sd == SdCardState::Mounted ||
                      services_.state->sd == SdCardState::ReadOnly;
    widgets::textBlock(gfx, theme::kPadding, kListTop + 20, w,
                       sdOk ? "No address book on this card yet. Add one over USB serial: "
                              "contacts add \"Ada Lovelace\" \"+44 20 7946 0000\""
                            : "Insert an SD card to read contacts.",
                       theme::kTextSizeSmall, theme::kTextDim);
    return;
  }

  if (contacts->total() == 0) {
    widgets::textBlock(gfx, theme::kPadding, kListTop + 20, w,
                       "No contacts yet. Add one over USB serial: "
                       "contacts add \"Ada Lovelace\" \"+44 20 7946 0000\"",
                       theme::kTextSizeSmall, theme::kTextDim);
    return;
  }

  const size_t count = contacts->cached();
  int16_t y = kListTop;
  for (size_t i = 0; i < kPageSize; i++) {
    const size_t idx = pageStart_ + i;
    if (idx >= count) {
      continue;
    }
    const ContactInfo* c = contacts->at(idx);
    if (c == nullptr) {
      continue;
    }
    char primary[48];
    char raw[80];
    snprintf(raw, sizeof(raw), "%s%s", c->favorite ? "* " : "", c->name);
    fitLine(primary, sizeof(primary), raw, kRowChars);

    // Phone first: it is what a reference-first address book is usually
    // consulted for. Never a call affordance — the cube cannot dial.
    const char* detail = c->phone[0] != '\0' ? c->phone : c->email;
    char secondary[48];
    snprintf(raw, sizeof(raw), "%s%s", detail[0] != '\0' ? detail : "no phone or email",
             c->hasVoice ? "  [voice]" : "");
    fitLine(secondary, sizeof(secondary), raw, kRowChars);

    rowRects_[i] = widgets::listItem(gfx, theme::kPadding, y, w, primary, secondary, false);
    y += kRowStep;
  }

  char footer[64];
  const size_t last = pageStart_ + kPageSize < count ? pageStart_ + kPageSize : count;
  if (contacts->hiddenCount() > 0) {
    // The cache is smaller than the card. Saying so — and naming the way to
    // reach the rest — beats a list that quietly stops.
    snprintf(footer, sizeof(footer), "%u-%u of %u  +%u more: contacts search",
             (unsigned)(pageStart_ + 1), (unsigned)last, (unsigned)count,
             (unsigned)contacts->hiddenCount());
  } else {
    snprintf(footer, sizeof(footer), "%u-%u of %u   swipe up/down", (unsigned)(pageStart_ + 1),
             (unsigned)last, (unsigned)count);
  }
  // Searching needs text entry and this device has no keyboard, so the app
  // names the route that does exist instead of offering a search box that
  // could not be typed into.
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kPanelAlt);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 56);
  gfx.print("USB serial: contacts search");

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, DISPLAY_HEIGHT - 28);
  gfx.print(footer);
}

void ContactsApp::renderDetail(Arduino_GFX& gfx) {
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  const int16_t smallPerLine = charsPerLine(theme::kTextSizeSmall);

  widgets::textBlock(gfx, theme::kPadding, kTop, w, detail_.name, theme::kTextSizeBody,
                     theme::kText);
  int16_t y = kTop + 3 * kBodyLineH;

  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("phone");
  widgets::textBlock(gfx, theme::kPadding, y + kSmallLineH + 4, w,
                     detail_.phone[0] != '\0' ? detail_.phone : "-", theme::kTextSizeSmall,
                     theme::kText);
  y += 2 * kSmallLineH + 12;

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("email");
  widgets::textBlock(gfx, theme::kPadding, y + kSmallLineH + 4, w,
                     detail_.email[0] != '\0' ? detail_.email : "-", theme::kTextSizeSmall,
                     theme::kText);
  y += 3 * kSmallLineH + 12;

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("note");
  const size_t noteLen = strlen(detail_.note);
  if (noteLen == 0) {
    widgets::textBlock(gfx, theme::kPadding, y + kSmallLineH + 4, w, "-", theme::kTextSizeSmall,
                       theme::kText);
  } else {
    const size_t fits = wrapPrefix(detail_.note, smallPerLine, kNoteLines);
    char shown[161];
    snprintf(shown, sizeof(shown), "%.*s", (int)fits, detail_.note);
    widgets::textBlock(gfx, theme::kPadding, y + kSmallLineH + 4, w, shown,
                       theme::kTextSizeSmall, theme::kText);
    if (fits < noteLen) {
      gfx.setTextSize(theme::kTextSizeSmall);
      gfx.setTextColor(theme::kWarn);
      gfx.setCursor(theme::kPadding, y + kSmallLineH + 4 + kNoteLines * kSmallLineH);
      gfx.print("more: contacts show");
    }
  }
  y += kSmallLineH + 4 + (kNoteLines + 1) * kSmallLineH;

  if (detail_.voice[0] != '\0') {
    gfx.setTextSize(theme::kTextSizeSmall);
    gfx.setTextColor(theme::kAccent);
    gfx.setCursor(theme::kPadding, y);
    gfx.print("voice note attached");
  }

  const int16_t by = DISPLAY_HEIGHT - 12 - 52;
  const bool fav = services_.contacts != nullptr && services_.contacts->isFavorite(detail_.id);
  favRect_ = widgets::button(gfx, theme::kPadding, by, (w - 8) / 2, 52,
                             fav ? "* favourite" : "favourite", fav);
  deleteRect_ = widgets::button(gfx, theme::kPadding + (w + 8) / 2, by, (w - 8) / 2, 52, "delete",
                                false);
}

void ContactsApp::renderConfirmDelete(Arduino_GFX& gfx) {
  char body[96];
  // Bounded field widths: the fixed text alone is 47 bytes, so an unbounded
  // name would silently eat the rest of the sentence.
  snprintf(body, sizeof(body), "%.28s (%.10s) is removed from the address book on the card.",
           detail_.name, detail_.id);
  confirmRect_ = widgets::modalConfirm(gfx, "Delete contact?", body, cancelRect_);
}

bool ContactsApp::handleList(const InputEvent& event) {
  const size_t count = listCount();
  switch (event.action) {
    case InputAction::SwipeUp:
      if (pageStart_ + kPageSize < count) {
        pageStart_ += kPageSize;
        dirty_ = true;
      }
      return true;
    case InputAction::SwipeDown:
      pageStart_ = pageStart_ >= kPageSize ? pageStart_ - kPageSize : 0;
      dirty_ = true;
      return true;
    case InputAction::Tap:
      for (size_t i = 0; i < kPageSize; i++) {
        if (rowRects_[i].w > 0 && rowRects_[i].contains(event.x, event.y)) {
          openDetail(pageStart_ + i);
          return true;
        }
      }
      return true;
    default:
      return false;  // Back/Home fall through to the router
  }
}

bool ContactsApp::handleDetail(const InputEvent& event) {
  switch (event.action) {
    case InputAction::Tap:
      if (favRect_.contains(event.x, event.y)) {
        if (services_.contacts != nullptr) {
          const bool next = !services_.contacts->isFavorite(detail_.id);
          services_.contacts->setFavorite(detail_.id, next);
          detail_.favorite = services_.contacts->isFavorite(detail_.id);
          // Favourites sort first, so the list order this record came from is
          // now wrong; re-read it before the user can get back there.
          services_.contacts->reload();
          if (pageStart_ >= listCount()) {
            pageStart_ = 0;
          }
        }
        dirty_ = true;
        return true;
      }
      if (deleteRect_.contains(event.x, event.y)) {
        screen_ = Screen::ConfirmDelete;
        dirty_ = true;
        return true;
      }
      return true;
    case InputAction::SwipeRight:
    case InputAction::Back:
    case InputAction::Cancel:
      backToList();
      return true;
    default:
      return false;
  }
}

bool ContactsApp::handleConfirmDelete(const InputEvent& event) {
  if (event.action == InputAction::Tap) {
    if (confirmRect_.contains(event.x, event.y)) {
      if (services_.contacts != nullptr) {
        services_.contacts->remove(detail_.id);
      }
      refresh();
      backToList();
      return true;
    }
    if (cancelRect_.contains(event.x, event.y)) {
      screen_ = Screen::Detail;
      dirty_ = true;
    }
    return true;  // absorb every other tap while the modal is up
  }
  if (event.action == InputAction::Back || event.action == InputAction::Cancel) {
    screen_ = Screen::Detail;
    dirty_ = true;
    return true;
  }
  return true;
}

bool ContactsApp::handleInput(const InputEvent& event) {
  switch (screen_) {
    case Screen::List:
      return handleList(event);
    case Screen::Detail:
      return handleDetail(event);
    case Screen::ConfirmDelete:
      return handleConfirmDelete(event);
  }
  return false;
}
