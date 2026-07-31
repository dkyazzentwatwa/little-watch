# Little Cube OS Technical Guide

## Target hardware

The supported board is the Waveshare ESP32-S3-Touch-AMOLED-1.8: ESP32-S3R8,
8 MB OPI PSRAM, 16 MB flash, SH8601 368×448 AMOLED, FT3168 touch, ES8311
audio codec, SD_MMC 1-bit storage, AXP2101 PMU, PCF85063 RTC, and QMI8658 IMU.
Board pins, addresses, geometry, and input thresholds are defined in
`little-cube-os/src/board_config.h`.

## Toolchain and commands

Arduino CLI is required. PlatformIO, CMake, and ESP-IDF are not supported.

```bash
export LITTLECUBE_FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=custom"
export LITTLECUBE_PORT="/dev/cu.usbmodem101"  # check: arduino-cli board list

./scripts/install-libraries.sh  # ESP32 core 3.3.8 and required libraries
./scripts/build.sh               # compile with all warnings into dist/
./scripts/upload.sh              # upload and handle USB re-enumeration
./scripts/monitor.sh              # serial monitor at 115200 baud
```

`LITTLECUBE_FQBN` and `LITTLECUBE_PORT` are required by the build and upload
scripts. `PSRAM=opi` and the custom partition scheme are required settings.
The upload script performs the 1200-baud reset used by this board's USB CDC
configuration.

## Repository layout

```text
little-cube-os/        Arduino sketch and firmware
little-cube-os/src/    core, hardware, services, storage, serial, UI, apps, video
libraries/             vendored Arduino_DriveBus touch library
scripts/               setup, build, upload, monitor, and pack-video.sh helpers
docs/                  product contract, interfaces, designs, and validation
dist/                  generated build output
```

The firmware currently registers 16 apps: Home, Today, Clock, Weather,
Calendar, Notes, Recorder, Assistant, Audio, Files, Contacts, Calculator,
Settings, Reader, News, and Video. Services handle Wi-Fi provisioning, weather,
time, notes, recordings, audio, contacts, calendar, podcasts, news, and
assistant requests, and live MP3 radio streams. Video playback decodes `.lcv`
(MJPEG + PCM) files from the SD card — see `docs/lcv-format.md`.

## Runtime and architecture

`core/Kernel.cpp` owns the adapters and services, then runs the non-blocking
update and render loop. Apps receive semantic touch actions through the router.
Services own network, storage, and other slow work. Serial command families in
`serial/commands/` call services rather than manipulating hardware directly.

User content is stored under the approved roots in `storage/StoragePaths.h`.
Paths must pass `SdStorage::sanitizePath()`. Small writes use `AtomicFile`;
long-running writes use temporary `.partial` files and rename only after clean
completion. Wi-Fi passwords remain in NVS and are never written to the SD card
or serial logs.

## Verification

There is no host-side unit-test suite. A clean compile proves only that the
firmware builds. Physical claims must be checked on the actual cube and recorded
in [docs/hardware-validation.md](docs/hardware-validation.md). Keep compile,
upload, and device-proven states distinct. Use `./scripts/monitor.sh` and
commands such as `status`, `version`, `uptime`, `open <app>`, and `back` for
runtime checks.

See [docs/product-spec.md](docs/product-spec.md) for the behavior contract,
[docs/serial-interface.md](docs/serial-interface.md) for command syntax, and
[AGENTS.md](AGENTS.md) for contribution conventions.
