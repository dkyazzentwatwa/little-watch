#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../hardware/SdCardState.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

// Files (spec §25): an on-device browser over the SD tree rooted at
// paths::kRoot. Browse a directory, descend, inspect a file's details,
// delete behind a confirm, read storage usage, and safely eject.
//
// Two rules shape the whole app:
//
//  1. The card can be pulled between any two instructions. Every SD call is
//     attempted, never assumed: mounted() is checked before and the result
//     checked after, and a failure degrades to an empty listing plus the
//     specific SdCardState — never a generic error and never a crash.
//  2. render() runs every frame, so it never touches the filesystem. The
//     directory is read in refresh() (open/resume/navigate/card-state edge)
//     into a fixed cache, and the frame only draws that cache.
class FilesApp : public App {
 public:
  explicit FilesApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override;
  void onResume() override;

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  enum class Screen : uint8_t { Browse, Details, Usage, Eject };
  enum class EjectPhase : uint8_t { Idle, Stopping, Done, Failed };

  struct Entry {
    char name[64] = "";
    uint32_t sizeBytes = 0;
    bool isDir = false;
    bool nameTooLong = false;  // real name did not fit; path joins would be wrong
  };

  // The cache is bounded on purpose (a media folder can hold thousands).
  // totalEntries_ keeps the honest count so the UI can say "+N more"
  // instead of quietly pretending the directory ends at 48.
  static constexpr size_t kMaxEntries = 48;
  static constexpr size_t kPageSize = 4;
  // Even counting has to stop somewhere; when this trips the total is shown
  // as a floor ("512+") rather than a number we cannot stand behind.
  static constexpr size_t kScanLimit = 2048;

  void go(Screen screen);
  void clearRects();
  void refresh();
  void sortEntries();
  bool joinChild(const char* name, char* out, size_t outSize) const;
  bool atRoot() const;
  void navigateTo(const char* path);
  void navigateUp();
  void openEntry(size_t index);
  void deleteSelected();
  void beginEject();
  void showToast(const char* message);
  bool sdUsable() const;

  void renderBrowse(Arduino_GFX& gfx);
  void renderDetails(Arduino_GFX& gfx);
  void renderUsage(Arduino_GFX& gfx);
  void renderEject(Arduino_GFX& gfx);
  bool handleBrowse(const InputEvent& event);
  bool handleDetails(const InputEvent& event);
  bool handleUsage(const InputEvent& event);
  bool handleEject(const InputEvent& event);

  Services& services_;
  StatusBar statusBar_;
  Screen screen_ = Screen::Browse;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  // Taps are only matched against rects the last frame actually drew for
  // the screen the user is looking at.
  Screen renderedScreen_ = Screen::Browse;
  bool laidOut_ = false;

  char cwd_[160] = "";
  Entry entries_[kMaxEntries];
  size_t entryCount_ = 0;    // cached, <= kMaxEntries
  size_t totalEntries_ = 0;  // actually present in the directory
  bool countCapped_ = false;
  bool listOk_ = false;
  size_t pageStart_ = 0;

  size_t selected_ = 0;
  bool confirmDelete_ = false;
  bool confirmEject_ = false;

  EjectPhase ejectPhase_ = EjectPhase::Idle;
  SdCardState lastSdState_ = SdCardState::NotPresent;

  char toast_[48] = "";
  uint32_t toastMs_ = 0;

  widgets::Rect rowRects_[kPageSize];
  widgets::Rect upRect_;
  widgets::Rect usageRect_;
  widgets::Rect ejectRect_;
  widgets::Rect backRect_;
  widgets::Rect deleteRect_;
  widgets::Rect confirmRect_;
  widgets::Rect cancelRect_;
};
