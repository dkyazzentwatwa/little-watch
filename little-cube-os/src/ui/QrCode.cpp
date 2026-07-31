#include "QrCode.h"

#include <Arduino_GFX_Library.h>
#include <qrcode.h>

namespace qrcode {

namespace {

// Espressif's encoder supports QR versions 2-40, but article URLs are short;
// version 10 at ECC_LOW holds roughly 271 alphanumeric characters, far more
// than any URL this app will show. Capping it bounds the allocation
// esp_qrcode_generate() makes internally — it documents ESP_ERR_NO_MEM as a
// real failure mode — to a small, known size. That allocation is transient
// and tied to page entry, not per-frame: NewsApp::render() returns early
// when the frame isn't dirty, so a static QR page encodes exactly once.
constexpr int kMaxVersion = 10;

// Below 2px a module cannot be reliably resolved by a phone camera on this
// panel. Returning true for a smaller module would report a successful draw
// for a QR that nothing can actually scan, so 2px is the floor, not 1px.
constexpr int16_t kMinModulePx = 2;

// esp_qrcode_generate() is callback-based: display_func receives only the
// finished QR handle, with no user-data parameter, so the render target and
// geometry have to reach the callback through file scope. This is NOT
// reentrant — it is only safe because all rendering in this firmware runs on
// the single loop task (kernelLoop() -> router render, see
// core/Kernel.cpp). This is a real constraint of the API, not a shortcut:
// draw() must never be called from more than one task.
struct DrawState {
  Arduino_GFX* gfx = nullptr;
  int16_t x = 0;
  int16_t y = 0;
  int16_t maxSizePx = 0;
  uint16_t fg = 0;
  uint16_t bg = 0;
  bool drawn = false;
};

DrawState gState;

void onQrGenerated(esp_qrcode_handle_t handle) {
  DrawState& s = gState;
  if (s.gfx == nullptr) {
    return;
  }
  const int size = esp_qrcode_get_size(handle);
  if (size <= 0) {
    return;
  }
  // Count the quiet zone (4 modules on every side) in the fit, so the
  // border is never sacrificed to squeeze in a larger code.
  const int16_t totalModules = static_cast<int16_t>(size) + 8;
  const int16_t module = s.maxSizePx / totalModules;
  if (module < kMinModulePx) {
    return;
  }
  const int16_t side = module * totalModules;
  s.gfx->fillRect(s.x, s.y, side, side, s.bg);
  const int16_t originX = s.x + module * 4;
  const int16_t originY = s.y + module * 4;
  for (int my = 0; my < size; my++) {
    for (int mx = 0; mx < size; mx++) {
      if (esp_qrcode_get_module(handle, mx, my)) {
        s.gfx->fillRect(originX + mx * module, originY + my * module, module, module, s.fg);
      }
    }
  }
  s.drawn = true;
}

}  // namespace

bool draw(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t maxSizePx, const char* text,
          uint16_t fg, uint16_t bg) {
  if (text == nullptr || text[0] == '\0' || maxSizePx <= 0) {
    return false;
  }

  gState = DrawState{};
  gState.gfx = &gfx;
  gState.x = x;
  gState.y = y;
  gState.maxSizePx = maxSizePx;
  gState.fg = fg;
  gState.bg = bg;

  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = onQrGenerated;
  cfg.max_qrcode_version = kMaxVersion;
  // ECC_LOW maximises payload per module, which is the right trade here: the
  // code is on a clean backlit surface, not a printed label that might be
  // damaged.
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;

  const esp_err_t err = esp_qrcode_generate(&cfg, text);
  const bool ok = (err == ESP_OK) && gState.drawn;
  gState = DrawState{};  // drop the Arduino_GFX& reference promptly
  return ok;
}

}  // namespace qrcode
