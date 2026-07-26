#include "FilesApp.h"

#include <Arduino_GFX_Library.h>
#include <SD_MMC.h>
#include <ctype.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/SdCardAdapter.h"
#include "../storage/SdStorage.h"
#include "../storage/StoragePaths.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"

namespace {

constexpr int16_t kTop = theme::kStatusBarHeight + 6;
constexpr int16_t kToolbarY = theme::kStatusBarHeight + 28;
constexpr int16_t kToolbarH = theme::kTouchTargetMin;
constexpr int16_t kListTop = kToolbarY + kToolbarH + 12;
constexpr int16_t kRowStep = 62;
constexpr int16_t kFooterY = DISPLAY_HEIGHT - 56;
constexpr int16_t kFooter2Y = DISPLAY_HEIGHT - 30;
constexpr int16_t kContentW = DISPLAY_WIDTH - 2 * theme::kPadding;
constexpr uint32_t kToastMs = 2500;

// Characters of the base font that fit across a width at a given size.
constexpr int16_t charsAcross(int16_t width, uint8_t size) {
  return width / (6 * size);
}

// Copies at most maxChars of src, marking a cut with a trailing '~' so a
// truncated name never reads like the whole one.
void fitLabel(const char* src, char* out, size_t outSize, int16_t maxChars) {
  if (maxChars < 1) {
    maxChars = 1;
  }
  size_t limit = static_cast<size_t>(maxChars);
  if (limit > outSize - 1) {
    limit = outSize - 1;
  }
  const size_t len = strlen(src);
  if (len <= limit) {
    memcpy(out, src, len + 1);
    return;
  }
  memcpy(out, src, limit - 1);
  out[limit - 1] = '~';
  out[limit] = '\0';
}

// Paths are most informative at the tail, so long ones lose their head.
void fitPathTail(const char* src, char* out, size_t outSize, int16_t maxChars) {
  size_t limit = static_cast<size_t>(maxChars < 4 ? 4 : maxChars);
  if (limit > outSize - 1) {
    limit = outSize - 1;
  }
  const size_t len = strlen(src);
  if (len <= limit) {
    memcpy(out, src, len + 1);
    return;
  }
  out[0] = '.';
  out[1] = '.';
  memcpy(out + 2, src + (len - (limit - 2)), limit - 2);
  out[limit] = '\0';
}

// Integer-only byte formatting: float conversions in snprintf are not worth
// pulling in for three labels.
void formatBytes(uint64_t bytes, char* out, size_t outSize) {
  const char* unit = "B";
  uint64_t div = 1;
  if (bytes >= 1024ULL * 1024 * 1024) {
    unit = "GB";
    div = 1024ULL * 1024 * 1024;
  } else if (bytes >= 1024ULL * 1024) {
    unit = "MB";
    div = 1024ULL * 1024;
  } else if (bytes >= 1024ULL) {
    unit = "KB";
    div = 1024ULL;
  }
  // Format at full width first, then copy under the caller's bound: an
  // integer conversion has no maximum-width specifier, so formatting straight
  // into a short buffer is what the truncation warning is about.
  char tmp[32];
  if (div == 1) {
    snprintf(tmp, sizeof(tmp), "%llu B", (unsigned long long)bytes);
  } else {
    snprintf(tmp, sizeof(tmp), "%llu.%llu %s", (unsigned long long)(bytes / div),
             (unsigned long long)((bytes % div) * 10 / div), unit);
  }
  strncpy(out, tmp, outSize - 1);
  out[outSize - 1] = '\0';
}

bool nameLess(const char* a, const char* b) {
  while (*a != '\0' && *b != '\0') {
    const int ca = tolower(static_cast<unsigned char>(*a));
    const int cb = tolower(static_cast<unsigned char>(*b));
    if (ca != cb) {
      return ca < cb;
    }
    a++;
    b++;
  }
  return *a == '\0' && *b != '\0';
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

void FilesApp::onOpen() {
  strncpy(cwd_, paths::kRoot, sizeof(cwd_) - 1);
  cwd_[sizeof(cwd_) - 1] = '\0';
  pageStart_ = 0;
  selected_ = 0;
  screen_ = Screen::Browse;
  confirmDelete_ = false;
  confirmEject_ = false;
  ejectPhase_ = EjectPhase::Idle;
  toast_[0] = '\0';
  toastMs_ = 0;
  lastSdState_ =
      services_.sdCard != nullptr ? services_.sdCard->state() : SdCardState::NotPresent;
  clearRects();
  refresh();
}

// Navigation survives, but nothing armed does: an unattended confirm sits
// under the user's first tap when they come back.
void FilesApp::onPause() {
  confirmDelete_ = false;
  confirmEject_ = false;
  laidOut_ = false;
  dirty_ = true;
}

void FilesApp::onResume() {
  // Serial `files delete`, a recording finishing, or a card swap all rewrite
  // this directory while the app is on the back stack.
  lastSdState_ =
      services_.sdCard != nullptr ? services_.sdCard->state() : SdCardState::NotPresent;
  refresh();
  if (selected_ >= entryCount_) {
    selected_ = 0;
    if (screen_ == Screen::Details) {
      screen_ = Screen::Browse;  // the file we were showing may be gone
    }
  }
  clearRects();
}

void FilesApp::onClose() {
  // Drop the listing: ~3 KB of names describing a directory the user has
  // left, on a card that may not be here next time.
  entryCount_ = 0;
  totalEntries_ = 0;
  countCapped_ = false;
  listOk_ = false;
  pageStart_ = 0;
  selected_ = 0;
  cwd_[0] = '\0';
  screen_ = Screen::Browse;
  confirmDelete_ = false;
  confirmEject_ = false;
  toast_[0] = '\0';
  toastMs_ = 0;
  clearRects();
}

// ------------------------------------------------------------------ helpers

bool FilesApp::sdUsable() const {
  return services_.sdCard != nullptr && services_.sdCard->mounted();
}

void FilesApp::go(Screen screen) {
  screen_ = screen;
  clearRects();
  dirty_ = true;
}

// Rects belong to one screen and one page. Clearing them on every transition
// is what stops a tap landing on a row that scrolled away two frames ago.
void FilesApp::clearRects() {
  for (size_t i = 0; i < kPageSize; i++) {
    rowRects_[i] = widgets::Rect{};
  }
  upRect_ = widgets::Rect{};
  usageRect_ = widgets::Rect{};
  ejectRect_ = widgets::Rect{};
  backRect_ = widgets::Rect{};
  deleteRect_ = widgets::Rect{};
  confirmRect_ = widgets::Rect{};
  cancelRect_ = widgets::Rect{};
  laidOut_ = false;
}

void FilesApp::showToast(const char* message) {
  strncpy(toast_, message, sizeof(toast_) - 1);
  toast_[sizeof(toast_) - 1] = '\0';
  toastMs_ = kToastMs;
  dirty_ = true;
}

bool FilesApp::atRoot() const {
  return strcmp(cwd_, paths::kRoot) == 0;
}

// Entry names come off the card, which is untrusted input like any other:
// the joined path goes through sanitizePath before it reaches SD_MMC.
bool FilesApp::joinChild(const char* name, char* out, size_t outSize) const {
  if (services_.storage == nullptr || name == nullptr || name[0] == '\0') {
    return false;
  }
  char raw[256];
  snprintf(raw, sizeof(raw), "%s/%s", cwd_, name);
  String safe;
  if (!services_.storage->sanitizePath(raw, safe)) {
    return false;
  }
  if (safe.length() + 1 > outSize) {
    return false;
  }
  strncpy(out, safe.c_str(), outSize - 1);
  out[outSize - 1] = '\0';
  return true;
}

void FilesApp::navigateTo(const char* path) {
  strncpy(cwd_, path, sizeof(cwd_) - 1);
  cwd_[sizeof(cwd_) - 1] = '\0';
  pageStart_ = 0;
  selected_ = 0;
  clearRects();
  refresh();
}

void FilesApp::navigateUp() {
  if (atRoot()) {
    return;
  }
  char parent[sizeof(cwd_)];
  strncpy(parent, cwd_, sizeof(parent) - 1);
  parent[sizeof(parent) - 1] = '\0';
  char* slash = strrchr(parent, '/');
  if (slash == nullptr || slash == parent) {
    navigateTo(paths::kRoot);
    return;
  }
  *slash = '\0';
  // Never climb out of the app's root, whatever the path looked like.
  if (strncmp(parent, paths::kRoot, strlen(paths::kRoot)) != 0) {
    navigateTo(paths::kRoot);
    return;
  }
  navigateTo(parent);
}

void FilesApp::sortEntries() {
  // Directories first, then case-insensitive by name. Insertion sort: the
  // cache is 48 entries, and this runs once per directory, not per frame.
  for (size_t i = 1; i < entryCount_; i++) {
    const Entry key = entries_[i];
    size_t j = i;
    while (j > 0) {
      const Entry& prev = entries_[j - 1];
      const bool keyFirst = (key.isDir != prev.isDir) ? key.isDir
                                                      : nameLess(key.name, prev.name);
      if (!keyFirst) {
        break;
      }
      entries_[j] = prev;
      j--;
    }
    entries_[j] = key;
  }
}

// The only place that touches the filesystem. Never called from render().
void FilesApp::refresh() {
  entryCount_ = 0;
  totalEntries_ = 0;
  countCapped_ = false;
  listOk_ = false;
  dirty_ = true;

  if (!sdUsable() || services_.storage == nullptr || cwd_[0] == '\0') {
    return;
  }
  String safe;
  if (!services_.storage->sanitizePath(cwd_, safe)) {
    return;
  }

  fs::File dir = SD_MMC.open(safe);
  if (!dir) {
    return;  // card pulled, or the directory vanished — both are just "empty"
  }
  if (!dir.isDirectory()) {
    dir.close();
    return;
  }

  for (fs::File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (totalEntries_ >= kScanLimit) {
      countCapped_ = true;
      entry.close();
      break;
    }
    totalEntries_++;
    if (entryCount_ < kMaxEntries) {
      Entry& out = entries_[entryCount_];
      // fs::File::name() is the basename on this core, but older cores
      // returned a full path and the difference would silently double the
      // directory in every joined path. Take the tail either way.
      const char* name = entry.name();
      if (name != nullptr) {
        const char* slash = strrchr(name, '/');
        if (slash != nullptr) {
          name = slash + 1;
        }
      }
      if (name == nullptr || name[0] == '\0') {
        name = "?";
      }
      // A name we cannot store is a name we cannot rebuild a path from, so
      // the row is shown but refuses to open rather than opening the wrong
      // thing.
      out.nameTooLong = strlen(name) >= sizeof(out.name);
      strncpy(out.name, name, sizeof(out.name) - 1);
      out.name[sizeof(out.name) - 1] = '\0';
      out.isDir = entry.isDirectory();
      out.sizeBytes = out.isDir ? 0u : static_cast<uint32_t>(entry.size());
      entryCount_++;
    }
    entry.close();
  }
  dir.close();

  // The card can go away mid-walk; a half-read directory must not be
  // presented as the directory.
  if (!sdUsable()) {
    entryCount_ = 0;
    totalEntries_ = 0;
    countCapped_ = false;
    return;
  }

  listOk_ = true;
  sortEntries();
  if (pageStart_ >= entryCount_) {
    pageStart_ = 0;
  }
}

void FilesApp::openEntry(size_t index) {
  if (index >= entryCount_) {
    return;
  }
  const Entry& entry = entries_[index];
  if (entry.nameTooLong) {
    showToast("name too long to open");
    return;
  }
  if (entry.isDir) {
    char path[sizeof(cwd_)];
    if (!joinChild(entry.name, path, sizeof(path))) {
      showToast("path not usable");
      return;
    }
    navigateTo(path);
    return;
  }
  selected_ = index;
  go(Screen::Details);
}

void FilesApp::deleteSelected() {
  if (selected_ >= entryCount_) {
    go(Screen::Browse);
    return;
  }
  const bool wasDir = entries_[selected_].isDir;
  char path[256];
  bool ok = false;
  if (joinChild(entries_[selected_].name, path, sizeof(path)) && services_.storage != nullptr &&
      sdUsable()) {
    ok = services_.storage->removeFile(path);
  }
  if (ok) {
    showToast("deleted");
  } else if (!sdUsable()) {
    showToast(sdCardStateName(services_.sdCard != nullptr ? services_.sdCard->state()
                                                         : SdCardState::NotPresent));
  } else {
    showToast(wasDir ? "delete failed: not empty?" : "delete failed");
  }
  refresh();
  if (selected_ >= entryCount_) {
    selected_ = 0;
  }
  go(Screen::Browse);
}

// Eject is a request, not a call: requestEject() returns immediately and the
// unmount lands some frames later. update() watches for the result.
void FilesApp::beginEject() {
  if (services_.sdCard == nullptr || !services_.sdCard->requestEject()) {
    ejectPhase_ = EjectPhase::Failed;
    showToast("nothing mounted");
    go(Screen::Eject);
    return;
  }
  ejectPhase_ = EjectPhase::Stopping;
  go(Screen::Eject);
}

// -------------------------------------------------------------------- frame

void FilesApp::update(uint32_t deltaMs) {
  if (toastMs_ > 0) {
    toastMs_ = deltaMs >= toastMs_ ? 0 : toastMs_ - deltaMs;
    if (toastMs_ == 0) {
      toast_[0] = '\0';
      dirty_ = true;
    }
  }

  // Card state is the app's only external input. When it moves, everything
  // cached about the card is suspect.
  const SdCardState sd =
      services_.sdCard != nullptr ? services_.sdCard->state() : SdCardState::NotPresent;
  if (sd != lastSdState_) {
    lastSdState_ = sd;
    dirty_ = true;
    confirmDelete_ = false;
    if (!sdUsable()) {
      entryCount_ = 0;
      totalEntries_ = 0;
      countCapped_ = false;
      listOk_ = false;
      pageStart_ = 0;
      selected_ = 0;
      // Every cached path referred to a card that is no longer there.
      strncpy(cwd_, paths::kRoot, sizeof(cwd_) - 1);
      cwd_[sizeof(cwd_) - 1] = '\0';
      if (screen_ == Screen::Details) {
        screen_ = Screen::Browse;
      }
      clearRects();
    } else {
      if (ejectPhase_ != EjectPhase::Stopping) {
        ejectPhase_ = EjectPhase::Idle;  // a card came back
      }
      refresh();
    }
  }

  if (ejectPhase_ == EjectPhase::Stopping && services_.sdCard != nullptr &&
      !services_.sdCard->ejecting()) {
    // Completion is observed, never returned: the adapter reports through
    // its state, so trust that rather than the request's return value.
    ejectPhase_ = services_.sdCard->mounted() ? EjectPhase::Failed : EjectPhase::Done;
    dirty_ = true;
  }
}

void FilesApp::render() {
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
    case Screen::Browse: renderBrowse(gfx); break;
    case Screen::Details: renderDetails(gfx); break;
    case Screen::Usage: renderUsage(gfx); break;
    case Screen::Eject: renderEject(gfx); break;
  }

  if (toastMs_ > 0 && toast_[0] != '\0') {
    widgets::toast(gfx, toast_);
  }

  renderedScreen_ = screen_;
  laidOut_ = true;
  display->markDirty();
}

void FilesApp::renderBrowse(Arduino_GFX& gfx) {
  char path[40];
  fitPathTail(cwd_, path, sizeof(path), charsAcross(kContentW, theme::kTextSizeSmall));
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kAccent);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print(path);

  const int16_t bw = (kContentW - 12) / 3;
  upRect_ = widgets::button(gfx, theme::kPadding, kToolbarY, bw, kToolbarH,
                            atRoot() ? "root" : "up", false);
  usageRect_ = widgets::button(gfx, theme::kPadding + bw + 6, kToolbarY, bw, kToolbarH, "usage",
                               false);
  ejectRect_ = widgets::button(gfx, theme::kPadding + 2 * (bw + 6), kToolbarY, bw, kToolbarH,
                               "eject", false);

  if (!sdUsable()) {
    char line[64];
    snprintf(line, sizeof(line), "SD card: %s",
             services_.sdCard != nullptr ? sdCardStateName(services_.sdCard->state())
                                         : "unavailable");
    widgets::textBlock(gfx, theme::kPadding, kListTop + 20, kContentW, line,
                       theme::kTextSizeSmall, theme::kWarn);
    widgets::textBlock(gfx, theme::kPadding, kListTop + 70, kContentW,
                       "Insert a card to browse files.", theme::kTextSizeSmall,
                       theme::kTextDim);
    return;
  }

  if (entryCount_ == 0) {
    widgets::textBlock(gfx, theme::kPadding, kListTop + 20, kContentW,
                       listOk_ ? "This folder is empty." : "Could not read this folder.",
                       theme::kTextSizeSmall, theme::kTextDim);
    return;
  }

  const int16_t nameChars = charsAcross(kContentW - 16, theme::kTextSizeSmall);
  int16_t y = kListTop;
  for (size_t i = 0; i < kPageSize; i++) {
    const size_t idx = pageStart_ + i;
    if (idx >= entryCount_) {
      rowRects_[i] = widgets::Rect{};
      continue;
    }
    const Entry& entry = entries_[idx];
    char label[48];
    fitLabel(entry.name, label, sizeof(label), nameChars);

    char secondary[48];
    if (entry.nameTooLong) {
      snprintf(secondary, sizeof(secondary), "name too long to open");
    } else if (entry.isDir) {
      snprintf(secondary, sizeof(secondary), "folder");
    } else {
      char size[24];
      formatBytes(entry.sizeBytes, size, sizeof(size));
      snprintf(secondary, sizeof(secondary), "%s", size);
    }

    rowRects_[i] = widgets::listItem(gfx, theme::kPadding, y, kContentW, label, secondary,
                                     false);
    if (entry.isDir) {
      gfx.fillCircle(DISPLAY_WIDTH - theme::kPadding - 10, y + 28, 5, theme::kAccent);
    }
    y += kRowStep;
  }

  char pager[56];
  const size_t last = pageStart_ + kPageSize < entryCount_ ? pageStart_ + kPageSize : entryCount_;
  snprintf(pager, sizeof(pager), "%u-%u of %u shown  ^v", (unsigned)(pageStart_ + 1),
           (unsigned)last, (unsigned)entryCount_);
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, kFooterY);
  gfx.print(pager);

  // A capped cache always says so, with the real total.
  if (totalEntries_ > entryCount_) {
    char more[56];
    snprintf(more, sizeof(more), "+%u more in %u%s entries",
             (unsigned)(totalEntries_ - entryCount_), (unsigned)totalEntries_,
             countCapped_ ? "+" : "");
    gfx.setTextColor(theme::kWarn);
    gfx.setCursor(theme::kPadding, kFooter2Y);
    gfx.print(more);
  } else {
    gfx.setTextColor(theme::kPanelAlt);
    gfx.setCursor(theme::kPadding, kFooter2Y);
    gfx.print("tap = open, hold = details");
  }
}

void FilesApp::renderDetails(Arduino_GFX& gfx) {
  if (selected_ >= entryCount_) {
    // The row was deleted, renamed or unmounted out from under us. Say so and
    // make sure no delete control survives pointing at nothing.
    widgets::textBlock(gfx, theme::kPadding, kTop + 40, kContentW, "This item is gone.",
                       theme::kTextSizeSmall, theme::kWarn);
    deleteRect_ = widgets::Rect{};
    confirmRect_ = widgets::Rect{};
    cancelRect_ = widgets::Rect{};
    backRect_ = widgets::button(gfx, theme::kPadding, DISPLAY_HEIGHT - theme::kPadding - 56,
                                kContentW, 56, "back", false);
    return;
  }
  const Entry& entry = entries_[selected_];

  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Details");

  // The name wraps; reserve the three lines a 63-char name can need rather
  // than measuring text in the render path.
  int16_t y = kTop + 44;
  widgets::textBlock(gfx, theme::kPadding, y, kContentW, entry.name, theme::kTextSizeSmall,
                     theme::kAccent);
  y += 3 * 18 + 16;

  char line[64];
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print(entry.isDir ? "folder" : "file");
  y += 30;

  if (!entry.isDir) {
    char size[24];
    formatBytes(entry.sizeBytes, size, sizeof(size));
    snprintf(line, sizeof(line), "size  %s", size);
    gfx.setTextColor(theme::kText);
    gfx.setCursor(theme::kPadding, y);
    gfx.print(line);
    y += 30;
  }

  gfx.setTextColor(theme::kTextDim);
  gfx.setCursor(theme::kPadding, y);
  gfx.print("in");
  widgets::textBlock(gfx, theme::kPadding + 40, y, kContentW - 40, cwd_, theme::kTextSizeSmall,
                     theme::kTextDim);

  // Delete is a visible control behind a modal — never a swipe (spec §9).
  const int16_t bw = (kContentW - 6) / 2;
  const int16_t by = DISPLAY_HEIGHT - theme::kPadding - 56;
  backRect_ = widgets::button(gfx, theme::kPadding, by, bw, 56, "back", false);
  deleteRect_ = widgets::Rect{static_cast<int16_t>(theme::kPadding + bw + 6), by, bw, 56};
  gfx.drawRoundRect(deleteRect_.x, deleteRect_.y, deleteRect_.w, deleteRect_.h,
                    theme::kCardRadius / 2, theme::kBad);
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kBad);
  gfx.setCursor(deleteRect_.x + (bw - 6 * 6 * theme::kTextSizeBody) / 2, deleteRect_.y + 16);
  gfx.print("delete");

  if (confirmDelete_) {
    char body[96];
    char shortName[40];
    fitLabel(entry.name, shortName, sizeof(shortName), 32);
    // cwd_ can be far longer than the modal body; bound both fields so the
    // filename never gets pushed out by a deep path.
    snprintf(body, sizeof(body), "%.36s in %.52s", shortName, cwd_);
    confirmRect_ = widgets::modalConfirm(
        gfx, entry.isDir ? "Delete folder?" : "Delete file?", body, cancelRect_);
  }
}

void FilesApp::renderUsage(Arduino_GFX& gfx) {
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Storage");

  const SdCardState sd =
      services_.sdCard != nullptr ? services_.sdCard->state() : SdCardState::NotPresent;
  gfx.setTextSize(theme::kTextSizeSmall);
  gfx.setTextColor(sdUsable() ? theme::kGood : theme::kWarn);
  gfx.setCursor(theme::kPadding, kTop + 40);
  gfx.print(sdCardStateName(sd));

  // totalBytes()/freeBytes() are the adapter's cached values — O(1) and safe
  // from a frame. A FAT walk here would stall for seconds.
  const uint64_t total = services_.sdCard != nullptr ? services_.sdCard->totalBytes() : 0;
  const uint64_t freeB = services_.sdCard != nullptr ? services_.sdCard->freeBytes() : 0;
  const uint64_t used = total > freeB ? total - freeB : 0;

  int16_t y = kTop + 80;
  char line[64];
  char value[24];
  const char* labels[3] = {"total", "used", "free"};
  const uint64_t values[3] = {total, used, freeB};
  for (uint8_t i = 0; i < 3; i++) {
    formatBytes(values[i], value, sizeof(value));
    snprintf(line, sizeof(line), "%-6s %s", labels[i], total == 0 ? "unknown" : value);
    gfx.setTextColor(i == 2 ? theme::kText : theme::kTextDim);
    gfx.setCursor(theme::kPadding, y);
    gfx.print(line);
    y += 32;
  }

  // Usage bar.
  const int16_t barY = y + 12;
  gfx.drawRoundRect(theme::kPadding, barY, kContentW, 24, 6, theme::kPanelAlt);
  if (total > 0) {
    const int16_t fill = static_cast<int16_t>((used * (kContentW - 4)) / total);
    const uint16_t color = used * 10 > total * 9 ? theme::kBad : theme::kAccent;
    if (fill > 0) {
      gfx.fillRoundRect(theme::kPadding + 2, barY + 2, fill, 20, 4, color);
    }
  }

  widgets::textBlock(gfx, theme::kPadding, barY + 40, kContentW,
                     "Capacity is a cached reading, refreshed in the background.",
                     theme::kTextSizeSmall, theme::kPanelAlt);

  const int16_t bw = (kContentW - 6) / 2;
  const int16_t by = DISPLAY_HEIGHT - theme::kPadding - 56;
  backRect_ = widgets::button(gfx, theme::kPadding, by, bw, 56, "back", false);
  ejectRect_ = widgets::button(gfx, theme::kPadding + bw + 6, by, bw, 56, "eject", false);
}

void FilesApp::renderEject(Arduino_GFX& gfx) {
  gfx.setTextSize(theme::kTextSizeBody);
  gfx.setTextColor(theme::kText);
  gfx.setCursor(theme::kPadding, kTop);
  gfx.print("Eject card");

  const int16_t by = DISPLAY_HEIGHT - theme::kPadding - 56;
  const int16_t bw = (kContentW - 6) / 2;

  switch (ejectPhase_) {
    case EjectPhase::Idle:
      widgets::textBlock(gfx, theme::kPadding, kTop + 50, kContentW,
                         "Stops recording and playback, closes every open file, then "
                         "unmounts the card. Wait for the confirmation before pulling it.",
                         theme::kTextSizeSmall, theme::kTextDim);
      backRect_ = widgets::button(gfx, theme::kPadding, by, bw, 56, "back", false);
      ejectRect_ = widgets::button(gfx, theme::kPadding + bw + 6, by, bw, 56, "eject", true);
      break;

    case EjectPhase::Stopping:
      gfx.setTextSize(theme::kTextSizeSmall);
      gfx.setTextColor(theme::kWarn);
      gfx.setCursor(theme::kPadding, kTop + 50);
      gfx.print("stopping writers...");
      widgets::textBlock(gfx, theme::kPadding, kTop + 90, kContentW,
                         "Waiting for open files to close. Do not remove the card yet.",
                         theme::kTextSizeSmall, theme::kTextDim);
      // No controls on purpose: the teardown owns the card until it finishes.
      backRect_ = widgets::Rect{};
      ejectRect_ = widgets::Rect{};
      break;

    case EjectPhase::Done:
      gfx.setTextSize(theme::kTextSizeSmall);
      gfx.setTextColor(theme::kGood);
      gfx.setCursor(theme::kPadding, kTop + 50);
      gfx.print("safe to remove the card");
      widgets::textBlock(gfx, theme::kPadding, kTop + 90, kContentW,
                         "Reinsert it at any time; the cube remounts on its own.",
                         theme::kTextSizeSmall, theme::kTextDim);
      backRect_ = widgets::button(gfx, theme::kPadding, by, kContentW, 56, "back", false);
      ejectRect_ = widgets::Rect{};
      break;

    case EjectPhase::Failed: {
      char line[64];
      snprintf(line, sizeof(line), "still mounted: %s",
               services_.sdCard != nullptr ? sdCardStateName(services_.sdCard->state())
                                           : "unavailable");
      widgets::textBlock(gfx, theme::kPadding, kTop + 50, kContentW, line,
                         theme::kTextSizeSmall, theme::kBad);
      widgets::textBlock(gfx, theme::kPadding, kTop + 100, kContentW,
                         "A writer did not release the card. Try again in a moment.",
                         theme::kTextSizeSmall, theme::kTextDim);
      backRect_ = widgets::button(gfx, theme::kPadding, by, bw, 56, "back", false);
      ejectRect_ = widgets::button(gfx, theme::kPadding + bw + 6, by, bw, 56, "retry", false);
      break;
    }
  }

  if (confirmEject_) {
    confirmRect_ = widgets::modalConfirm(gfx, "Eject card?",
                                         "Recording and playback stop, then the card unmounts.",
                                         cancelRect_);
  }
}

// -------------------------------------------------------------------- input

bool FilesApp::handleBrowse(const InputEvent& event) {
  switch (event.action) {
    case InputAction::SwipeUp:
      if (pageStart_ + kPageSize < entryCount_) {
        pageStart_ += kPageSize;
        clearRects();  // the rows under the finger are about to change
        dirty_ = true;
      }
      return true;
    case InputAction::SwipeDown:
      if (pageStart_ > 0) {
        pageStart_ = pageStart_ >= kPageSize ? pageStart_ - kPageSize : 0;
        clearRects();
        dirty_ = true;
      }
      return true;
    case InputAction::SwipeRight:
      if (!atRoot()) {
        navigateUp();
        return true;
      }
      return false;
    case InputAction::Tap:
      if (upRect_.contains(event.x, event.y)) {
        navigateUp();
        return true;
      }
      if (usageRect_.contains(event.x, event.y)) {
        go(Screen::Usage);
        return true;
      }
      if (ejectRect_.contains(event.x, event.y)) {
        confirmEject_ = ejectPhase_ == EjectPhase::Idle;
        go(Screen::Eject);
        return true;
      }
      for (size_t i = 0; i < kPageSize; i++) {
        if (rowRects_[i].contains(event.x, event.y)) {
          openEntry(pageStart_ + i);
          return true;
        }
      }
      return true;
    case InputAction::LongPress:
      // Details for anything, including a folder, so an empty folder can be
      // deleted without a way to "open" it first.
      for (size_t i = 0; i < kPageSize; i++) {
        if (rowRects_[i].contains(event.x, event.y) && pageStart_ + i < entryCount_) {
          selected_ = pageStart_ + i;
          go(Screen::Details);
          return true;
        }
      }
      return true;
    case InputAction::Back:
    case InputAction::Cancel:
      if (!atRoot()) {
        navigateUp();
        return true;
      }
      return false;  // at the root, Back belongs to the router
    default:
      return false;
  }
}

bool FilesApp::handleDetails(const InputEvent& event) {
  if (confirmDelete_) {
    if (event.action == InputAction::Tap) {
      if (confirmRect_.contains(event.x, event.y)) {
        confirmDelete_ = false;
        deleteSelected();
      } else if (cancelRect_.contains(event.x, event.y)) {
        confirmDelete_ = false;
        clearRects();
        dirty_ = true;
      }
      return true;
    }
    if (event.action == InputAction::Back || event.action == InputAction::Cancel) {
      confirmDelete_ = false;
      clearRects();
      dirty_ = true;
      return true;
    }
    return true;  // the modal absorbs everything else while it is up
  }

  switch (event.action) {
    case InputAction::Tap:
      if (deleteRect_.contains(event.x, event.y)) {
        confirmDelete_ = true;
        clearRects();  // taps are dropped until the modal has actually drawn
        dirty_ = true;
      } else if (backRect_.contains(event.x, event.y)) {
        go(Screen::Browse);
      }
      return true;
    case InputAction::Back:
    case InputAction::Cancel:
    case InputAction::SwipeRight:
      go(Screen::Browse);
      return true;
    default:
      return false;
  }
}

bool FilesApp::handleUsage(const InputEvent& event) {
  switch (event.action) {
    case InputAction::Tap:
      if (ejectRect_.contains(event.x, event.y)) {
        confirmEject_ = ejectPhase_ == EjectPhase::Idle;
        go(Screen::Eject);
      } else if (backRect_.contains(event.x, event.y)) {
        go(Screen::Browse);
      }
      return true;
    case InputAction::Back:
    case InputAction::Cancel:
    case InputAction::SwipeRight:
      go(Screen::Browse);
      return true;
    default:
      return false;
  }
}

bool FilesApp::handleEject(const InputEvent& event) {
  if (confirmEject_) {
    if (event.action == InputAction::Tap) {
      if (confirmRect_.contains(event.x, event.y)) {
        confirmEject_ = false;
        beginEject();
      } else if (cancelRect_.contains(event.x, event.y)) {
        confirmEject_ = false;
        go(Screen::Browse);
      }
      return true;
    }
    if (event.action == InputAction::Back || event.action == InputAction::Cancel) {
      confirmEject_ = false;
      go(Screen::Browse);
      return true;
    }
    return true;
  }

  // Nothing leaves this screen while the teardown is running — including
  // Back, which would otherwise hide the one message that says when the card
  // is actually safe to pull.
  if (ejectPhase_ == EjectPhase::Stopping) {
    return true;
  }

  switch (event.action) {
    case InputAction::Tap:
      if (ejectRect_.contains(event.x, event.y)) {
        confirmEject_ = true;
        clearRects();
        dirty_ = true;
      } else if (backRect_.contains(event.x, event.y)) {
        if (ejectPhase_ != EjectPhase::Idle) {
          ejectPhase_ = EjectPhase::Idle;
        }
        go(Screen::Browse);
      }
      return true;
    case InputAction::Back:
    case InputAction::Cancel:
    case InputAction::SwipeRight:
      ejectPhase_ = EjectPhase::Idle;
      go(Screen::Browse);
      return true;
    default:
      return false;
  }
}

bool FilesApp::handleInput(const InputEvent& event) {
  // A tap can only mean something if the frame it was aimed at is the frame
  // currently laid out for this screen.
  const bool positional = event.action == InputAction::Tap ||
                          event.action == InputAction::DoubleTap ||
                          event.action == InputAction::LongPress;
  if (positional && (!laidOut_ || renderedScreen_ != screen_)) {
    return true;
  }

  switch (screen_) {
    case Screen::Browse: return handleBrowse(event);
    case Screen::Details: return handleDetails(event);
    case Screen::Usage: return handleUsage(event);
    case Screen::Eject: return handleEject(event);
  }
  return false;
}
