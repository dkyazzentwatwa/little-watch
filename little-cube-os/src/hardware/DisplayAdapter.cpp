#include "DisplayAdapter.h"

#include <Adafruit_XCA9554.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>

#include "../board_config.h"
#include "../ui/Theme.h"

namespace {
// Flush cap: a full 368x448 RGB565 blit is ~322 KB over QSPI; 30 fps is
// plenty for this UI and keeps the loop responsive.
constexpr uint32_t kMinFlushIntervalMs = 33;
}  // namespace

bool DisplayAdapter::begin() {
  // Shared I2C bus first: panel reset runs through the XCA9554 expander
  // (P0-P2 low -> P7 high -> 20 ms -> P0-P2 high), not a GPIO. Sequence
  // proven on this exact board.
  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);

  static Adafruit_XCA9554 expander;
  expander_ = &expander;
  if (!expander_->begin(I2C_ADDR_EXPANDER, &Wire)) {
    Serial.println("[display] XCA9554 not found; trying AMOLED init anyway");
  } else {
    for (uint8_t pin = 0; pin < 3; pin++) {
      expander_->pinMode(pin, OUTPUT);
      expander_->digitalWrite(pin, LOW);
    }
    expander_->pinMode(7, OUTPUT);
    expander_->digitalWrite(7, HIGH);
    delay(20);
    for (uint8_t pin = 0; pin < 3; pin++) {
      expander_->digitalWrite(pin, HIGH);
    }
  }

  bus_ = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_SDIO0, PIN_LCD_SDIO1,
                               PIN_LCD_SDIO2, PIN_LCD_SDIO3);
  panel_ = new Arduino_SH8601(bus_, PIN_LCD_RST, DISPLAY_ROTATION, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  if (!panel_->begin()) {
    Serial.println("[display] SH8601 init failed");
    return false;
  }

  // Frame canvas in PSRAM. Risk gate: with PSRAM=opi the ~322 KB
  // aligned_alloc must land in SPIRAM — the numbers below prove it on
  // hardware (DRAM could never absorb it).
  const uint32_t psramBefore = ESP.getFreePsram();
  const uint32_t heapBefore = ESP.getFreeHeap();
  canvas_ = new Arduino_Canvas(DISPLAY_WIDTH, DISPLAY_HEIGHT, panel_);
  if (!canvas_->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("[display] canvas alloc FAILED; falling back to direct panel draw");
    delete canvas_;
    canvas_ = nullptr;
  }
  const uint32_t psramAfter = ESP.getFreePsram();
  const uint32_t heapAfter = ESP.getFreeHeap();
  Serial.printf("[display] canvas: PSRAM %u -> %u (-%u KB), heap %u -> %u (-%u KB)\n",
                (unsigned)psramBefore, (unsigned)psramAfter,
                (unsigned)((psramBefore - psramAfter) / 1024), (unsigned)heapBefore,
                (unsigned)heapAfter, (unsigned)((heapBefore - heapAfter) / 1024));

  Arduino_GFX* gfx = canvas();
  gfx->fillScreen(theme::kBg);
  gfx->setTextWrap(false);
  if (canvas_ != nullptr) {
    canvas_->flush(true);
  }

  setBrightness(DEFAULT_BRIGHTNESS);
  ready_ = true;
  return true;
}

Arduino_GFX* DisplayAdapter::canvas() {
  if (canvas_ != nullptr) {
    return canvas_;
  }
  return panel_;
}

void DisplayAdapter::present() {
  if (!ready_ || !dirty_ || canvas_ == nullptr) {
    // Direct-draw fallback needs no flush; without dirty content there is
    // nothing to push.
    dirty_ = canvas_ == nullptr ? false : dirty_;
    return;
  }
  const uint32_t now = millis();
  if (now - lastFlushMs_ < kMinFlushIntervalMs) {
    return;  // stays dirty; flushed on a later tick
  }
  canvas_->flush(true);
  lastFlushMs_ = now;
  dirty_ = false;
}

void DisplayAdapter::setBrightness(uint8_t value) {
  brightness_ = value;
  if (panel_ != nullptr) {
    panel_->setBrightness(value);
  }
}

void DisplayAdapter::splash(const char* title, const char* subtitle) {
  if (!ready_ && panel_ == nullptr) {
    return;
  }
  Arduino_GFX* gfx = canvas();
  gfx->fillScreen(theme::kBg);

  // Centered title with the 6x8 base font scaled up.
  const int16_t titleLen = strlen(title);
  const int16_t titleW = titleLen * 6 * theme::kTextSizeTitle;
  gfx->setTextSize(theme::kTextSizeTitle);
  gfx->setTextColor(theme::kText);
  gfx->setCursor((DISPLAY_WIDTH - titleW) / 2, DISPLAY_HEIGHT / 2 - 40);
  gfx->print(title);

  gfx->fillRect((DISPLAY_WIDTH - 80) / 2, DISPLAY_HEIGHT / 2 + 4, 80, 4, theme::kAccent);

  if (subtitle != nullptr) {
    const int16_t subLen = strlen(subtitle);
    const int16_t subW = subLen * 6 * theme::kTextSizeSmall;
    gfx->setTextSize(theme::kTextSizeSmall);
    gfx->setTextColor(theme::kTextDim);
    gfx->setCursor((DISPLAY_WIDTH - subW) / 2, DISPLAY_HEIGHT / 2 + 24);
    gfx->print(subtitle);
  }

  markDirty();
  if (canvas_ != nullptr) {
    canvas_->flush(true);
    dirty_ = false;
  }
}
