# Repository Guidelines

## Project Structure & Module Organization

Little Cube OS is Arduino/C++ firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.8.
The sketch lives in `little-cube-os/`; its code is under `little-cube-os/src/`:
`core/` contains the kernel and router, `hardware/` contains board adapters,
`services/` contains application services, `storage/` owns SD persistence,
`serial/` contains command families, `ui/` contains rendering, and `apps/`
contains the user-facing apps. `libraries/Arduino_DriveBus` is vendored board
support. Use `docs/product-spec.md` as the behavior contract and
`docs/hardware-validation.md` as the record of physical verification. Build
outputs go to `dist/` and should not be treated as source.

## Build, Upload, and Development Commands

This project uses Arduino CLI only. Set the board FQBN and connected USB port:

```bash
export LITTLECUBE_FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=custom"
export LITTLECUBE_PORT="/dev/cu.usbmodem101"
./scripts/install-libraries.sh  # one-time toolchain and dependency setup
./scripts/build.sh              # compile with all warnings into dist/
./scripts/upload.sh             # upload, including the 1200-baud reset
./scripts/monitor.sh            # serial monitor at 115200 baud
```

There is no PlatformIO, CMake, or ESP-IDF project structure. A successful build
does not prove the feature works on hardware; use the validation document and
serial commands such as `status`, `version`, and `uptime` during device checks.

## Coding Style & Naming Conventions

Follow the existing C++ style: four-space indentation, braces on the same line,
`PascalCase` types, `camelCase` methods/locals, and `kConstantName` constants.
Keep hardware truth in `src/board_config.h`; apps consume semantic input and
services rather than raw coordinates or direct hardware access. Keep the main
loop non-blocking and use services, state machines, or FreeRTOS tasks for slow
work. Sanitize user paths through `SdStorage` and use `AtomicFile` for small
writes. Match existing `Feature: description` commit titles.

## Testing Guidelines

No automated unit-test or host harness exists. For every behavioral change,
compile with `./scripts/build.sh`, then test on the physical cube when possible
and record only observed results in `docs/hardware-validation.md`. Report
compile-ready, uploaded, and device-proven states separately.

## Commit & Pull Request Guidelines

Use focused commits with imperative, subsystem-prefixed subjects, for example
`Weather: preserve freshness state` or `Storage: reject unsafe paths`. Pull
requests should explain the user-visible or hardware impact, list validation
commands and device checks, call out unverified areas, and include screenshots
or serial logs when UI or runtime behavior changes.
