#include "QrCode.h"

#include <Arduino_GFX_Library.h>
#include <qrcode.h>
#include <string.h>

namespace qrcode {

namespace {

// Espressif's encoder supports QR versions up to 40, but this panel's pixel
// budget is the real ceiling: past roughly version 27 the module size drops
// below kMinModulePx even at the widest box this UI offers, so codes beyond
// that could never scan regardless of how much text they hold. 17 leaves
// headroom for a URL that keeps a real query parameter after tracker
// stripping (Task 10 only strips keys it recognizes) while keeping the
// transient calloc small — 905 B here vs. 408 B at the upstream default of
// 10 (see ESP_QRCODE_CONFIG_DEFAULT() below); that lopsided cost-to-capacity
// spread is the tell that memory was never the binding constraint, pixels
// were. This explicitly overrides the default rather than tightening it, so
// a future core bump changing that default can't silently move our ceiling.
// Raising it costs nothing for short URLs either: the IDF wrapper pins
// minVersion to 1, so max_qrcode_version is a ceiling, not a target — the
// encoder still picks the smallest version that fits.
constexpr int kMaxVersion = 17;

// Below 2px a module cannot be reliably resolved by a phone camera on this
// panel. Returning true for a smaller module would report a successful draw
// for a QR that nothing can actually scan, so 2px is the floor, not 1px.
constexpr int16_t kMinModulePx = 2;

// qrcodegen's bit-packed buffer size for a given version: side = v*4+17
// modules per edge, one bit per module, rounded up to a byte, plus one
// spare byte (the encoder's own BUFFER_LEN_FOR_VERSION macro).
constexpr size_t bufferLenForVersion(int version) {
  const int side = version * 4 + 17;
  return static_cast<size_t>((side * side + 7) / 8 + 1);
}

// 905 bytes at kMaxVersion == 17.
constexpr size_t kCacheBufferLen = bufferLenForVersion(kMaxVersion);

// Version 17 byte-mode capacity is 619 characters at ECC LOW; +1 for the
// null terminator. URLs almost always encode in byte mode here (mixed case,
// '.', '/', '%'-escapes all fall outside the QR alphanumeric set), so byte
// capacity is the number that matters, not the larger alphanumeric/numeric
// limits — and it means any text that successfully encodes is guaranteed to
// fit this cache slot too.
constexpr size_t kCacheTextCap = 620;

// Encoded-QR cache: a static page (e.g. a news article's QR) gets repainted
// every time SystemState::version ticks — clock-minute rollover, the AMOLED
// burn-in shift every 60 s, battery/Wi-Fi/SD changes — which happens
// roughly once or twice a minute, indefinitely, for as long as the page
// stays open. Re-encoding on every one of those repaints was a measured
// 8-35 ms against a ~33 ms frame budget: a recurring stutter, not a
// one-time entry cost. Caching the raw encode keeps a repaint on unchanged
// text down to just the fillRect loop (~1-2 ms): if the incoming text
// matches gCacheText, draw() skips esp_qrcode_generate() entirely and
// renders straight from gCache.
uint8_t gCache[kCacheBufferLen];
char gCacheText[kCacheTextCap] = {0};
bool gCacheValid = false;

// Set by onQrGenerated() when it successfully copies a fresh encode into
// gCache; read by draw() immediately after esp_qrcode_generate() returns.
// esp_qrcode_generate()'s callback receives only the finished handle — no
// user-data parameter — so this is the only channel back to draw(). It is
// reset to 0 before every encode attempt (see draw()) because it is
// file-scope and persists between calls: without that reset, a failed
// encode following a successful one would still read the previous call's
// size and report success for text that never actually encoded.
int gPendingCopiedSize = 0;

// Draws `buffer` (a qrcodegen bit-packed QR of `size` x `size` modules) into
// the maxSizePx box, including the mandatory 4-module quiet zone. Shared by
// both the fresh-encode and cache-hit paths so there is exactly one place
// that does the fit math and the fillRect loop.
bool renderBuffer(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t maxSizePx, uint16_t fg,
                   uint16_t bg, const uint8_t* buffer, int size) {
  const int16_t totalModules = static_cast<int16_t>(size) + 8;
  const int16_t module = maxSizePx / totalModules;
  if (module < kMinModulePx) {
    return false;
  }
  const int16_t side = module * totalModules;
  gfx.fillRect(x, y, side, side, bg);
  const int16_t originX = x + module * 4;
  const int16_t originY = y + module * 4;
  for (int my = 0; my < size; my++) {
    for (int mx = 0; mx < size; mx++) {
      if (esp_qrcode_get_module(buffer, mx, my)) {
        gfx.fillRect(originX + mx * module, originY + my * module, module, module, fg);
      }
    }
  }
  return true;
}

// esp_qrcode_generate() is callback-based: display_func receives only the
// finished QR handle, with no user-data parameter. This is NOT reentrant —
// it is only safe because all rendering in this firmware runs on the single
// loop task (kernelLoop() -> router render, see core/Kernel.cpp; the same
// file-scope-callback pattern VideoPlayer.cpp's jpegDrawBlock() uses for
// JPEGDEC). That is a real constraint of the API, not a shortcut: draw()
// must never be called from more than one task.
//
// esp_qrcode_handle_t is just a const uint8_t* into the encoder's own
// buffer, which it frees once this callback returns, so all this does is
// copy it into gCache while it is still valid — the actual rendering
// happens back in draw(), after esp_qrcode_generate() returns, so the
// fresh-encode and cache-hit paths share the same renderBuffer() call.
void onQrGenerated(esp_qrcode_handle_t handle) {
  const int size = esp_qrcode_get_size(handle);
  if (size <= 0) {
    return;
  }
  const size_t bytes = static_cast<size_t>((size * size + 7) / 8 + 1);
  if (bytes > sizeof(gCache)) {
    return;  // Can't happen while kMaxVersion bounds size; guarded anyway.
  }
  memcpy(gCache, handle, bytes);
  gPendingCopiedSize = size;
}

// RGB565: 5 bits R, 6 bits G, 5 bits B, MSB first (see theme::rgb() in
// Theme.cpp for the packing). A dark `bg` produces a code that looks
// perfectly drawn but never scans, because the quiet zone and the "white"
// modules no longer read as white to a phone camera — the same silent,
// unscannable failure kMinModulePx exists to catch, just from the color
// side instead of the size side.
bool isLightEnoughForQuietZone(uint16_t color565) {
  const uint8_t r5 = (color565 >> 11) & 0x1F;
  const uint8_t g6 = (color565 >> 5) & 0x3F;
  const uint8_t b5 = color565 & 0x1F;
  const uint16_t r8 = static_cast<uint16_t>(r5 * 255 / 31);
  const uint16_t g8 = static_cast<uint16_t>(g6 * 255 / 63);
  const uint16_t b8 = static_cast<uint16_t>(b5 * 255 / 31);
  const uint16_t luma = static_cast<uint16_t>((r8 * 299 + g8 * 587 + b8 * 114) / 1000);
  // Comfortably brighter than mid-gray — a QR quiet zone needs to read as
  // white, not just "not dark".
  constexpr uint16_t kMinLuma = 180;
  return luma >= kMinLuma;
}

}  // namespace

bool draw(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t maxSizePx, const char* text,
          uint16_t fg, uint16_t bg) {
  if (text == nullptr || text[0] == '\0' || maxSizePx <= 0) {
    return false;
  }
  if (!isLightEnoughForQuietZone(bg)) {
    return false;
  }

  if (gCacheValid && strcmp(gCacheText, text) == 0) {
    const int size = esp_qrcode_get_size(gCache);
    return renderBuffer(gfx, x, y, maxSizePx, fg, bg, gCache, size);
  }

  // Load-bearing reset — see gPendingCopiedSize's declaration comment.
  gPendingCopiedSize = 0;

  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = onQrGenerated;
  cfg.max_qrcode_version = kMaxVersion;
  // ECC_LOW sets the floor for version selection, not the delivered level:
  // the IDF wrapper calls qrcodegen_encodeText with boostEcl = true, which
  // raises the actual ECC to the highest level that still fits the chosen
  // version at no extra size cost — a short URL landing at a low version
  // can render at MEDIUM or higher even though LOW is requested here. LOW
  // is still the right level to *request*: it maximises how much text fits
  // before a bigger version (and smaller modules) becomes necessary.
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;

  const esp_err_t err = esp_qrcode_generate(&cfg, text);
  if (err != ESP_OK || gPendingCopiedSize <= 0) {
    // A failed encode must not leave a stale cache entry drawable.
    gCacheValid = false;
    return false;
  }

  strncpy(gCacheText, text, kCacheTextCap - 1);
  gCacheText[kCacheTextCap - 1] = '\0';
  gCacheValid = true;

  return renderBuffer(gfx, x, y, maxSizePx, fg, bg, gCache, gPendingCopiedSize);
}

}  // namespace qrcode
