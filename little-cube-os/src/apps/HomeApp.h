#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../ui/Carousel.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

// Home (spec §7): card carousel plus three overlay modes —
//   swipe down  -> system status panel
//   swipe up    -> quick actions (brightness / volume)
//   tap Tools   -> Files / Contacts / Calculator picker
class HomeApp : public App {
 public:
  explicit HomeApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override {}
  void onPause() override {}
  void onResume() override { forceRedraw_ = true; }

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  enum class Mode {
    Cards,
    Tools,
    Status,
    QuickActions,
  };

  void setMode(Mode mode);
  void renderTools(Arduino_GFX& gfx);
  void renderStatus(Arduino_GFX& gfx);
  void renderQuickActions(Arduino_GFX& gfx);
  bool handleCards(const InputEvent& event);
  bool handleTools(const InputEvent& event);
  bool handleQuickActions(const InputEvent& event);

  Services& services_;
  Carousel carousel_;
  StatusBar statusBar_;
  Mode mode_ = Mode::Cards;
  bool began_ = false;
  bool forceRedraw_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  // Hit rects captured during the last render of the active mode.
  widgets::Rect toolsRects_[3];
  widgets::Rect brightnessDown_;
  widgets::Rect brightnessUp_;
  widgets::Rect volumeDown_;
  widgets::Rect volumeUp_;
};
