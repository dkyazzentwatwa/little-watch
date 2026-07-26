#include "Theme.h"

namespace theme {

namespace {
// 8-bit-per-channel RGB packed to RGB565, so the palettes below read as normal
// colors instead of hand-computed hex.
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

uint8_t g_current = 0;
}  // namespace

// Active palette — initialized to theme 0 (Midnight) so the splash and first
// frame have colors before applyTheme() is called at boot.
uint16_t kBg = rgb(0, 0, 0);
uint16_t kPanel = rgb(16, 18, 24);
uint16_t kPanelAlt = rgb(30, 34, 44);
uint16_t kText = rgb(255, 255, 255);
uint16_t kTextDim = rgb(150, 160, 176);
uint16_t kAccent = rgb(0, 229, 255);
uint16_t kGood = rgb(0, 220, 130);
uint16_t kWarn = rgb(255, 180, 40);
uint16_t kBad = rgb(255, 74, 74);

// Ten palettes: five dark-background, five light-background. Order alternates
// loosely so the picker shows variety immediately. Every text/dim pair was
// chosen for contrast against its own bg.
const ThemeDef kThemes[kThemeCount] = {
    // ---- Dark ----
    {"Midnight", false, rgb(0, 0, 0), rgb(16, 18, 24), rgb(30, 34, 44), rgb(255, 255, 255),
     rgb(150, 160, 176), rgb(0, 229, 255), rgb(0, 220, 130), rgb(255, 180, 40), rgb(255, 74, 74)},
    {"Amber CRT", false, rgb(0, 0, 0), rgb(26, 18, 4), rgb(42, 30, 8), rgb(255, 176, 0),
     rgb(150, 104, 24), rgb(255, 198, 64), rgb(180, 220, 60), rgb(255, 140, 0), rgb(255, 92, 48)},
    {"Matrix", false, rgb(0, 4, 0), rgb(4, 22, 6), rgb(10, 38, 12), rgb(122, 255, 122),
     rgb(44, 124, 54), rgb(0, 255, 96), rgb(120, 255, 120), rgb(200, 255, 90), rgb(255, 96, 96)},
    {"Synthwave", false, rgb(12, 6, 22), rgb(30, 14, 46), rgb(50, 24, 70), rgb(240, 234, 255),
     rgb(156, 132, 186), rgb(255, 46, 182), rgb(80, 232, 200), rgb(255, 172, 62), rgb(255, 70, 112)},
    {"Deep Ocean", false, rgb(0, 6, 14), rgb(8, 24, 40), rgb(16, 38, 60), rgb(230, 240, 255),
     rgb(120, 152, 182), rgb(0, 190, 255), rgb(30, 220, 182), rgb(255, 190, 72), rgb(255, 92, 92)},
    // ---- Light ----
    {"Daylight", true, rgb(255, 255, 255), rgb(238, 240, 244), rgb(222, 226, 232), rgb(16, 18, 24),
     rgb(110, 118, 132), rgb(0, 110, 230), rgb(20, 160, 92), rgb(210, 138, 0), rgb(210, 52, 52)},
    {"Paper", true, rgb(244, 236, 220), rgb(232, 222, 202), rgb(214, 202, 178), rgb(60, 48, 32),
     rgb(132, 116, 92), rgb(170, 90, 40), rgb(92, 140, 60), rgb(200, 130, 30), rgb(182, 62, 40)},
    {"Mint", true, rgb(238, 250, 244), rgb(220, 240, 230), rgb(200, 228, 214), rgb(18, 60, 48),
     rgb(92, 142, 122), rgb(0, 170, 120), rgb(0, 170, 120), rgb(210, 150, 0), rgb(200, 70, 70)},
    {"Rose", true, rgb(252, 244, 246), rgb(246, 228, 232), rgb(236, 210, 216), rgb(70, 30, 44),
     rgb(150, 110, 124), rgb(220, 50, 120), rgb(60, 160, 110), rgb(210, 140, 30), rgb(200, 52, 62)},
    {"Slate", true, rgb(236, 238, 242), rgb(220, 224, 232), rgb(202, 208, 220), rgb(28, 34, 48),
     rgb(104, 114, 134), rgb(80, 70, 220), rgb(30, 150, 100), rgb(200, 140, 20), rgb(200, 60, 70)},
};

void applyTheme(uint8_t index) {
  if (index >= kThemeCount) {
    index = 0;
  }
  const ThemeDef& t = kThemes[index];
  kBg = t.bg;
  kPanel = t.panel;
  kPanelAlt = t.panelAlt;
  kText = t.text;
  kTextDim = t.textDim;
  kAccent = t.accent;
  kGood = t.good;
  kWarn = t.warn;
  kBad = t.bad;
  g_current = index;
}

uint8_t currentTheme() { return g_current; }

const char* themeName(uint8_t index) {
  return index < kThemeCount ? kThemes[index].name : "?";
}

bool themeIsLight(uint8_t index) {
  return index < kThemeCount && kThemes[index].light;
}

}  // namespace theme
