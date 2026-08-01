#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

class Arduino_GFX;
struct WeatherSnapshot;

// Weather (spec §13): current conditions + 3-day outlook, always labelled with
// freshness, drawn as wttr.in-style ASCII art in the built-in 6x8 font.
//
// TAP CYCLES VIEWS (Now -> Forecast -> Details -> Now). Refresh moved off the
// bare tap onto a long-press PLUS a visible button on the Details view, so the
// action stays discoverable instead of becoming a hidden gesture — this
// codebase requires a visible control for real actions.
//
// Offline shows the cached snapshot with its age and never pretends it is
// current. The two no-data states stay distinct on purpose: "offline - no
// cached weather yet" is fixed by walking into Wi-Fi range, "no location set -
// use phone setup" needs the phone. Collapsing them into one message would
// send the user to the wrong remedy.
class WeatherApp : public App {
 public:
  explicit WeatherApp(Services& services) : services_(services) {}

  void onOpen() override {
    view_ = View::Now;  // reopening always lands on the hero, never a stale view
    refreshRect_ = widgets::Rect{};
    dirty_ = true;
  }
  void onClose() override {}
  void onPause() override {}
  void onResume() override { dirty_ = true; }

  void update(uint32_t deltaMs) override { (void)deltaMs; }
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  enum class View : uint8_t { Now, Forecast, Details };

  void renderNow(Arduino_GFX& gfx, const WeatherSnapshot& wx, int16_t shiftX, int16_t shiftY);
  void renderForecast(Arduino_GFX& gfx, const WeatherSnapshot& wx, int16_t shiftX,
                      int16_t shiftY);
  void renderDetails(Arduino_GFX& gfx, const WeatherSnapshot& wx, int16_t shiftX,
                     int16_t shiftY);

  // One freshness string for all three views, so they can never disagree about
  // how old the data is.
  void formatFreshness(const WeatherSnapshot& wx, char* out, size_t outLen) const;

  void requestRefresh();

  Services& services_;
  StatusBar statusBar_;
  View view_ = View::Now;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;
  // Zero-width whenever the current view offers no button, which is what stops
  // a stale rect from swallowing taps on the other two views.
  widgets::Rect refreshRect_;
};
