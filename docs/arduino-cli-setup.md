# Arduino CLI Setup

Little Cube OS builds with **Arduino CLI only** — no PlatformIO, no CMake,
no ESP-IDF project structure.

## One-time setup

```bash
brew install arduino-cli          # macOS
./scripts/install-libraries.sh    # esp32 core 3.3.8 + all libraries
```

## Environment

```bash
export LITTLECUBE_FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=custom"
export LITTLECUBE_PORT="/dev/cu.usbmodem101"   # find it: arduino-cli board list
```

The FQBN options are load-bearing — verified against working firmware for
this exact board:

| Option | Value | Why |
|---|---|---|
| `FlashSize` | `16M` | ESP32-S3R8 module on this board has 16 MB flash |
| `PSRAM` | `opi` | 8 MB octal PSRAM. **Required** — `qspi`/`disabled` misbehave; the 322 KB frame canvas lives here |
| `USBMode` | `hwcdc` | hardware USB-serial (not TinyUSB) |
| `CDCOnBoot` | `cdc` | serial console available from boot |
| `PartitionScheme` | `custom` | uses `little-cube-os/partitions.csv` (dual 5 MB OTA + ~5.9 MB LittleFS) |

## Build / flash / monitor

```bash
./scripts/build.sh     # arduino-cli compile --warnings all → dist/
./scripts/upload.sh    # flash (see quirk below)
./scripts/monitor.sh   # serial console @ 115200
```

`scripts/build.sh` passes every library explicitly via `--library` because
passing any `--library` flag disables automatic library discovery. The
vendored FT3168 touch driver lives at `libraries/Arduino_DriveBus`.

CI / clean machines can instead use the pinned profile:

```bash
arduino-cli compile --profile littlecube little-cube-os
```

## The 1200-baud upload quirk

With `USBMode=hwcdc` + `CDCOnBoot=cdc`, entering the bootloader requires
"touching" the CDC port at 1200 baud, after which the port **disappears and
re-enumerates** (sometimes with a new name). `scripts/upload.sh` does this
automatically: `stty -f $PORT 1200` → wait 2 s → re-resolve
`/dev/cu.usbmodem*` → `arduino-cli upload`. If an upload fails mid-way,
unplug/replug and retry; holding BOOT (GPIO 0) while plugging in forces the
ROM bootloader.

## Library set

| Library | Version | Used for |
|---|---|---|
| GFX Library for Arduino | 1.6.6 | SH8601 QSPI panel + `Arduino_Canvas` framebuffer |
| Adafruit XCA9554 (+BusIO, +GFX core dep) | 1.0.0 | I/O expander that resets/powers the AMOLED panel |
| XPowersLib | 0.3.3 | AXP2101 PMU (battery %, charging, VBUS) |
| SensorLib | 0.4.1 | PCF85063 RTC (and QMI8658 IMU, unused in v1) |
| ArduinoJson | 7.4.3 | weather / calendar / contacts JSON |
| Arduino_DriveBus (vendored) | 1.0.1 | FT3168 touch controller |

In-core (no install): WiFi, WebServer, DNSServer, HTTPClient,
WiFiClientSecure, SD_MMC, FS, LittleFS, Preferences, ESP_I2S, esp_sntp.
