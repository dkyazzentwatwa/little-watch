# Little Cube OS

A calm, Wi-Fi-enabled personal information device for the **Waveshare
ESP32-S3-Touch-AMOLED-1.8** cube. Clock, weather, notes, voice recorder,
audio, calendar — glanceable on the 1.8″ AMOLED, with USB serial as the
keyboard and the SD card as the content store. Not a tiny smartphone.

- The display is the calm interface.
- USB serial handles typing.
- Wi-Fi handles connected information.
- SD stores your content.

## Hardware

Waveshare ESP32-S3-Touch-AMOLED-1.8 — ESP32-S3R8, 8 MB OPI PSRAM, 16 MB
flash, SH8601 AMOLED 368×448 (QSPI), FT3168 touch, ES8311 audio codec with
mic + speaker, microSD (SD_MMC 1-bit), AXP2101 PMU, PCF85063 RTC, QMI8658
IMU. All pins and init sequences in this repo are verified against working
firmware for this exact board — see `little-cube-os/src/board_config.h`.

## Build (Arduino CLI only)

```bash
export LITTLECUBE_FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=custom"
export LITTLECUBE_PORT="/dev/cu.usbmodem101"   # arduino-cli board list

./scripts/install-libraries.sh   # esp32 core 3.3.8 + libraries (one-time)
./scripts/build.sh               # compile
./scripts/upload.sh              # flash (handles the 1200-baud reset dance)
./scripts/monitor.sh             # serial console @115200
```

No PlatformIO, no CMake, no ESP-IDF project structure.

## Repository layout

```
little-cube-os/        Arduino sketch (little-cube-os.ino + src/**)
libraries/             vendored Arduino_DriveBus (FT3168 touch)
scripts/               build / upload / monitor / install-libraries
docs/                  product spec + design docs + hardware validation
```

## Status

Under active development. See `docs/product-spec.md` for the full
specification and `docs/hardware-validation.md` for what has actually been
verified on hardware (nothing is marked done without physical testing).
