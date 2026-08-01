# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Little Cube OS — Arduino/C++ firmware for the **Waveshare ESP32-S3-Touch-AMOLED-1.8**
cube (ESP32-S3R8, 8 MB OPI PSRAM, 16 MB flash, SH8601 368×448 AMOLED, FT3168 touch,
ES8311 audio, SD_MMC 1-bit, AXP2101 PMU, PCF85063 RTC).

`docs/product-spec.md` is the contract. Code comments cite it as "spec §N" — when
changing behavior, check the referenced section first.

## Build / flash / monitor

Arduino CLI **only**. No PlatformIO, no CMake, no ESP-IDF project structure, no
`platformio.ini`, no desktop mock executables.

```bash
export LITTLECUBE_FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=custom"
export LITTLECUBE_PORT="/dev/cu.usbmodem101"   # arduino-cli board list
```

```bash
./scripts/install-libraries.sh   # one-time: esp32 core 3.3.8 + libraries
./scripts/build.sh               # arduino-cli compile --warnings all -> dist/
./scripts/upload.sh              # flash (handles the 1200-baud reset dance)
./scripts/monitor.sh             # serial console @115200
```

The scripts exit if `LITTLECUBE_FQBN` / `LITTLECUBE_PORT` are unset. Every FQBN
option is load-bearing (`PSRAM=opi` especially — the 322 KB frame canvas lives
there); see `docs/arduino-cli-setup.md`.

Build gotchas:

- `scripts/build.sh` passes **every** library explicitly via `--library`, because
  passing any `--library` disables automatic discovery. Adding a dependency means
  editing three places: `scripts/build.sh`, `scripts/install-libraries.sh`, and the
  `littlecube` profile in `little-cube-os/sketch.yaml`.
- `sketch.yaml` deliberately has **no** `default_profile` — one would hijack the
  `--fqbn` builds and break the vendored-library path. CI can use
  `arduino-cli compile --profile littlecube little-cube-os`.
- Arduino CLI compiles only the sketch root and `src/**` recursively, which is why
  all module folders live under `little-cube-os/src/`.
- `little-cube-os.ino` is intentionally code-free. `setup()`/`loop()` are ordinary
  C++ definitions in `src/core/Kernel.cpp` — the sketch preprocessor mangles
  prototypes for `.ino`-defined functions on some toolchains, producing firmware
  that recurses in `setup()`. Do not move them back.
- Upload quirk: `USBMode=hwcdc` + `CDCOnBoot=cdc` requires touching the port at
  1200 baud, after which it disappears and re-enumerates (possibly under a new
  name). `upload.sh` handles this. On failure: unplug/replug, or hold BOOT (GPIO 0)
  while plugging in.

## Testing

There is no unit-test suite and no host-side harness — this is on-device firmware.
Verification is `docs/hardware-validation.md`, whose rule is absolute: **no item may
be checked without physically testing on the cube.** A clean compile is not
validation. Never mark a checklist item, or claim a feature works, based on
compilation alone; say what was compiled and what remains unverified on hardware.

Runtime verification happens over serial (`./scripts/monitor.sh`): `status`,
`version`, `uptime`, `open <app>`, `back`, plus per-family commands.

## Architecture

Boot path: `Kernel.cpp` owns every adapter/service as a file-scope singleton, wires
them into a `Services` struct (service locator, non-owning pointers, Kernel
lifetime), then `registerApps(router, services)` and `router.begin(AppId::Home)`.

`kernelLoop()` runs a strictly non-blocking frame: pump serial → drain input events
→ refresh `SystemState` (~1 Hz) → `update(deltaMs)` on every service → router
update/render → `display.present()`. **Nothing in the loop may block** on network,
storage, audio, or serial. Long work goes on FreeRTOS tasks (see `AudioAdapter`
record/play tasks) or in `update(deltaMs)` state machines.

Layers under `little-cube-os/src/`:

| Dir | Role |
|---|---|
| `core/` | Kernel, AppRouter + `App` base, EventBus, `Services`, `SystemState`, `InputAction` |
| `hardware/` | Adapters: display, input, SD, RTC, battery, `audio/` (ES8311 + I2S) |
| `services/` | Settings, Wifi, Provisioning, Time, Weather, Notes, Recorder, Music, Podcasts, Radio, News, Video |
| `storage/` | `SdStorage` (path sanitizing, tree), `AtomicFile`, `StoragePaths` |
| `serial/` | `SerialCommandService` dispatcher + `commands/` families |
| `ui/` | `Theme`, `StatusBar`, `Carousel`, `AmoledProtection`, `widgets/` |
| `video/` | LcvReader container parsing + VideoPlayer engine |
| `apps/` | The 16 apps + `AppRegistry` |

Key contracts:

- **`board_config.h` is the single source of hardware truth.** Pins, I2C addresses,
  geometry, gesture thresholds, and the RGB565 palette live there (re-exported by
  `ui/Theme.h`). Apps must never hardcode any of them. Values are verified against
  working firmware for this exact board — don't "fix" them speculatively.
- **Apps consume semantic input**, never raw coordinates: `InputEvent{action, x, y,
  timestampMs}` with `InputAction::{Tap, DoubleTap, LongPress, Swipe*, Back, Home,
  Confirm, Cancel}`. `handleInput` returns true when consumed; unconsumed
  Back/Home fall through to the router. Critical actions (delete, stop recording,
  eject) always need a visible control, never gesture-only.
- **Rendering** is immediate-mode into a full-frame PSRAM canvas (`display.canvas()`,
  an `Arduino_GFX`) every frame; `present()` flushes only when dirty, capped ~30 fps.
  Brightness is an AMOLED command (0 = panel off) — there is no backlight.
  Persistent chrome must offset by `AmoledProtection::shiftX()/shiftY()` (burn-in
  defense is a hard requirement, spec §37).
- **Text rendering has three rules that are not obvious and have each caused a
  visible bug:**
  - **On-screen strings must be pure ASCII (0x20–0x7E).** The `FreeSans*` faces
    contain nothing else, so a `·` or `°` draws as a garbage glyph. Serial
    output is exempt — a terminal handles UTF-8 fine.
  - **The built-in 6x8 font is a 256-glyph CP437 set** and *does* have a degree
    at `0xF8` — which is why the monospace screens can use it and the
    proportional ones cannot. But its `|` (`0x7C`) is a **broken bar** (rows 3
    and 7 blank), so it cannot form a continuous vertical across stacked rows;
    `0xB3` is the only full-height solid column.
  - **`widgets::textWidth()` measures with wrapping disabled.** `getTextBounds()`
    otherwise folds long strings and silently under-reports, which is wrong in
    the unsafe direction for every centering and clamping decision.
- **Bottom chrome goes through `widgets::footer()`**, never a hand-placed
  `setCursor(kPadding, DISPLAY_HEIGHT - N)`. `kPadding` (12) sits inside the
  bezel's corner radius at the bottom of the panel and clips; `kSafeInset` (20)
  clears it, confirmed on device. `footer()` also applies the burn-in offsets
  that every hand-placed footer was missing.
- **`SystemState`** is the shared status snapshot with a `version` counter; apps
  compare `lastStateVersion_` to decide whether to redraw. Bump `version` only when
  something actually changed.
  - ⚠️ **`render()` is NOT called once per screen entry.** `version` moves on a
    timer — every 60 s from the AMOLED pixel shift, and again each clock minute —
    so a screen left open re-renders 1–2×/min forever. **`render()` may draw and
    nothing else**: layout, pagination, encoding and I/O belong in `onOpen()`,
    `onResume()` or `update(deltaMs)`.
- **`EventBus`** is synchronous — handlers run inline on the publisher's call, so
  they must be fast. Services publish `SystemEvent`s instead of poking unrelated UI.
- **Everything degrades**: boot must succeed with no Wi-Fi and no SD card. SD state
  is always specific (`SdCardState::{NotPresent, ReadOnly, Corrupted, Full,
  RemovedUnexpectedly, ...}`), never a generic error.

### Storage rules

- Settings and secrets → NVS (`Preferences`, namespace `littlecube`) and LittleFS.
  **Wi-Fi passwords never touch the SD card and are never printed or logged**; the
  serial password prompt zeroes its buffers after use.
- User content → SD, under the `paths::` constants in `storage/StoragePaths.h`
  (`/littlecube/...`, plus the `/cypher-puter/desk` CrowPanel deck interop root).
- Every user-supplied path goes through `SdStorage::sanitizePath()` — `..`,
  backslashes, control characters, and anything outside the allowed roots are
  refused. Never call `SD_MMC` file mutations directly from a command handler.
- Small writes use the `AtomicFile` protocol (`.tmp` → verify → `.bak` → rename →
  drop `.bak`); long writers (recordings, downloads) write `.partial` and rename
  only on clean completion.

### Adding things

- **An app**: add to the `AppId` enum, bump `kAppCount`, extend `appName()` in
  `AppRouter.cpp`, subclass `App`, register it in `AppRegistry.cpp`. Apps are
  `static` locals constructed with `Services&`; keep state lightweight across
  close/reopen.
- **A serial command family**: add `serial/commands/XxxCommands.{h,cpp}` exposing
  `handleXxxCommand(Services&, verb, char* args)` + `printXxxHelp()`, then wire the
  family into `SerialCommandService::handleLine()` and the `help` output. Dispatch
  is `family verb args...` parsed with `cmdargs::nextToken`/`rest`; multiline input
  (notes) arms the shared `MultilineBuffer`. Commands act through services, never
  directly on hardware. Input length is capped (255 chars/line) — keep it that way.
- **A feature toggle**: `src/feature_flags.h` (`FEATURE_IMU` and `FEATURE_CAMERA`
  are off in v1; the IMU's absolute axis signs proved unreliable on this module).
