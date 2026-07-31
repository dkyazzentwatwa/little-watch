#pragma once

#include <Arduino.h>

// ============================================================================
// Little Cube OS — board configuration
// Waveshare ESP32-S3-Touch-AMOLED-1.8 (ESP32-S3R8, 8 MB OPI PSRAM, 16 MB flash)
//
// Every value below was verified against working firmware for this exact
// board (cypher-cube-os / waveshare-amoled-os). Apps must never hardcode any
// of these — this header and the hardware adapters are the single source of
// hardware truth.
// ============================================================================

constexpr uint32_t SERIAL_BAUD = 115200;

// --- Display: SH8601 AMOLED over QSPI --------------------------------------
// Panel reset is NOT a GPIO: it runs through the XCA9554 I/O expander
// (P0-P2 low -> P7 high -> 20 ms -> P0-P2 high) before SH8601 init.
// Brightness is an AMOLED command via Arduino_SH8601::setBrightness (0 = off).
constexpr uint16_t DISPLAY_WIDTH = 368;
constexpr uint16_t DISPLAY_HEIGHT = 448;
constexpr uint8_t DISPLAY_ROTATION = 0;

constexpr int PIN_LCD_SDIO0 = 4;
constexpr int PIN_LCD_SDIO1 = 5;
constexpr int PIN_LCD_SDIO2 = 6;
constexpr int PIN_LCD_SDIO3 = 7;
constexpr int PIN_LCD_SCLK = 11;
constexpr int PIN_LCD_CS = 12;
constexpr int PIN_LCD_RST = -1;  // reset handled by the expander, not a GPIO

constexpr uint8_t I2C_ADDR_EXPANDER = 0x20;  // XCA9554 (panel power/reset)
constexpr uint8_t DEFAULT_BRIGHTNESS = 220;  // 0..255

// Bounds for the *stored* brightness setting only. A corrupt or zero NVS
// record used to blank the panel at boot with no on-device way back, so
// SettingsService clamps into this range; 16 is the floor the Settings and
// Home steppers already enforce. DisplayAdapter::setBrightness() is
// deliberately NOT clamped — screen blanking depends on 0 reaching the panel.
constexpr uint8_t MIN_BRIGHTNESS = 16;
constexpr uint8_t MAX_BRIGHTNESS = 255;

// --- Touch: FT3168 (capacitive) ---------------------------------------------
// Shares the Wire bus. Raw coordinates map 1:1 to screen pixels at rotation 0
// (NO axis swap — Cardputer-derived ports that assumed one had to be patched).
constexpr int PIN_TOUCH_SDA = 15;
constexpr int PIN_TOUCH_SCL = 14;
constexpr int PIN_TOUCH_RST = -1;  // reset via expander
constexpr int PIN_TOUCH_INT = 21;

// --- Shared I2C bus (SDA 15 / SCL 14) — address map, deconflicted -----------
constexpr uint8_t I2C_ADDR_PMU_AXP2101 = 0x34;
constexpr uint8_t I2C_ADDR_RTC_PCF85063 = 0x51;
constexpr uint8_t I2C_ADDR_IMU_QMI8658 = 0x6B;
constexpr uint8_t I2C_ADDR_CODEC_ES8311 = 0x18;
// (touch FT3168 = 0x38, expander XCA9554 = 0x20)

// --- SD card: SD_MMC, 1-bit mode --------------------------------------------
// Mount with INPUT_PULLUP on all three pins first, then retry the clock
// ladder 25000 -> 20000 -> 10000 -> 4000 kHz. There is no card-detect pin.
constexpr int PIN_SD_CLK = 2;
constexpr int PIN_SD_CMD = 1;
constexpr int PIN_SD_D0 = 3;

// --- Audio: ES8311 codec + I2S ----------------------------------------------
// Codec is I2S slave, MCLK = 256 x sample rate, 16-bit, onboard analog mic.
constexpr int PIN_I2S_MCLK = 16;
constexpr int PIN_I2S_BCK = 9;
constexpr int PIN_I2S_WS = 45;
constexpr int PIN_I2S_DOUT = 8;   // ESP -> codec (speaker)
constexpr int PIN_I2S_DIN = 10;   // codec -> ESP (microphone)
constexpr int PIN_PA_ENABLE = 46; // power-amplifier enable, HIGH = on
constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;

// --- Button ------------------------------------------------------------------
constexpr int PIN_BOOT_BUTTON = 0;
constexpr bool BOOT_BUTTON_ACTIVE_LOW = true;

// --- Input tuning (values proven by the reference gesture engine) ------------
constexpr int16_t SWIPE_THRESHOLD_PX = 52;
constexpr uint32_t LONG_PRESS_MS = 650;
constexpr uint32_t DOUBLE_TAP_WINDOW_MS = 300;
constexpr uint32_t BOOT_LONG_PRESS_MS = 900;
// FT3168 polling is gated on PIN_TOUCH_INT because I2C traffic couples into
// the ES8311 analog-mic path (500 Hz/1 kHz comb in recordings). If a unit
// never raises INT, touch falls back to this cadence instead of going dead.
constexpr uint32_t TOUCH_INT_FALLBACK_POLL_MS = 20;

// --- Palette (RGB565) ---------------------------------------------------------
constexpr uint16_t COLOR_BG = 0x0000;
constexpr uint16_t COLOR_PANEL = 0x1082;
constexpr uint16_t COLOR_PANEL_2 = 0x2104;
constexpr uint16_t COLOR_TEXT = 0xFFFF;
constexpr uint16_t COLOR_DIM = 0xBDF7;
constexpr uint16_t COLOR_ACCENT = 0x07FF;
constexpr uint16_t COLOR_GOOD = 0x07E0;
constexpr uint16_t COLOR_WARN = 0xFD20;
constexpr uint16_t COLOR_BAD = 0xF800;

// --- Video player layout ------------------------------------------------------
// The player chrome occupies a strip on the panel's right edge (portrait
// space); video frames are centered in what remains so the picture and
// chrome never overlap. Shared by LcvReader (validates stored frame size)
// and VideoPlayer (centers the decoded frame) — kept here, not in either of
// those headers, because LcvReader must not depend on VideoPlayer (it is the
// lower layer: VideoPlayer includes LcvReader, not the reverse).
// VideoApp::kChromeH must equal kVideoChromeStripPx.
constexpr int16_t kVideoChromeStripPx = 58;
constexpr int16_t kVideoPictureAreaW = DISPLAY_WIDTH - kVideoChromeStripPx;  // 310

// --- Identity / persistence ----------------------------------------------------
constexpr const char* FIRMWARE_NAME = "Little Cube OS";
constexpr const char* FIRMWARE_VERSION = "0.1.0";
constexpr const char* PREF_NAMESPACE = "littlecube";
