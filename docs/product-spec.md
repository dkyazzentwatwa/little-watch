# Codex Specification: Little Cube OS

## 1. Product Summary

Build a compact, calm, Wi-Fi-enabled personal information device for the Waveshare 1.8-inch AMOLED cube-style hardware. The device should provide a small set of useful everyday tools without behaving like a miniature smartphone.

Core uses:

- clock and alarms
- timers and focus sessions
- weather
- calendar
- note reading
- USB serial note entry and editing
- voice recording
- music, podcasts, and radio
- SD card file access
- basic contacts
- device status and settings

The cube should work primarily as a glanceable desk, bedside, and pocket companion. The cube is the calm interface. USB serial is the keyboard.

## 2. Product Goals

Primary goals:

- Boot quickly into a useful home screen.
- Present information clearly on a 1.8-inch display.
- Remain useful without Wi-Fi.
- Store user content on the SD card.
- Use USB serial for text-heavy input and configuration.
- Support normal Wi-Fi network scanning and connection.
- Provide phone-assisted Wi-Fi provisioning when password entry would be awkward.
- Keep navigation shallow and predictable.
- Protect the AMOLED screen from excessive static content.
- Compile and upload using Arduino CLI only.

Secondary goals:

- Share notes and files with the larger CrowPanel writer deck.
- Support modular hardware adapters.
- Allow features to be disabled through compile-time flags.
- Preserve data after unexpected restarts.
- Provide useful diagnostic commands over USB serial.

Non-goals for v1 (do not implement): cellular calling, SMS, general-purpose web browsing, social media, an app store, full Android-style multitasking, a touchscreen QWERTY keyboard, long-form editing directly on the cube, live VoIP calling, DRM streaming services, third-party OAuth flows, enterprise Wi-Fi, a full captive-portal browser, cloud dependence, mascot or decorative character animations.

## 3. Target Hardware

Waveshare 1.8-inch AMOLED cube-style ESP32 device.

> **Resolved for this repo:** the exact board is the Waveshare
> ESP32-S3-Touch-AMOLED-1.8 (ESP32-S3R8, 8 MB OPI PSRAM, 16 MB flash).
> Every pin, controller, and init sequence is verified from working
> firmware and recorded in `little-cube-os/src/board_config.h`. The spec's
> original "keep configurable until verified" clause is satisfied — values
> live in board_config.h and the hardware adapters, never in app code.

Capabilities: ESP32-S3 processor, 1.8-inch AMOLED (SH8601, 368×448, QSPI), FT3168 touch, Wi-Fi, microphone + speaker (ES8311 codec), microSD (SD_MMC 1-bit), internal flash, USB serial (HW CDC), RTC (PCF85063), IMU (QMI8658, unused in v1), battery monitoring (AXP2101).

Feature flags live in `little-cube-os/src/feature_flags.h`. Display geometry comes from `board_config.h` / the display adapter — never hardcoded in apps.

## 4. Toolchain Requirements

Use: Arduino CLI, Arduino-compatible C++, `.ino`/`.cpp`/`.h`, Arduino board packages and libraries.

Do not use: PlatformIO, CMake, desktop mock executables, ESP-IDF-only project structure, `platformio.ini`.

```bash
export LITTLECUBE_FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=custom"
export LITTLECUBE_PORT="<SERIAL_PORT>"

arduino-cli core update-index
arduino-cli board list
./scripts/build.sh
./scripts/upload.sh
./scripts/monitor.sh
```

## 5. Product Architecture

Modular architecture: application router, semantic input layer, hardware adapters, service layer, storage layer, serial command layer, UI component library, system event bus.

Only one foreground app renders at a time. Apps may preserve lightweight state when closed. No blocking network, audio, storage, or serial operations inside the main UI loop.

## 6. First-Version Application Set

Home, Today, Clock, Weather, Calendar, Notes, Recorder, Audio, Files, Contacts, Calculator, Settings.

Primary carousel order: Today, Clock, Weather, Notes, Recorder, Audio, Calendar, Settings. Files, Contacts, and Calculator live under a secondary Tools screen.

## 7. Home Interface

Default layout: card carousel — one application card at a time.

Interactions: swipe left = next card; swipe right = previous card; tap = open; swipe down = system status; swipe up = quick actions; long press = Home; visible back control inside apps.

Optional compile-time 2×2 grid layout (`HomeLayout::Grid2x2`).

## 8. Shared Status Area

Show when space permits: time, Wi-Fi status, SD status, recording indicator, alarm indicator, battery. Icons with clear fallback text in status screens. Don't clutter; don't leave every state permanently on screen.

## 9. Input Architecture

Apps consume semantic actions (`InputAction`: None, Tap, DoubleTap, LongPress, SwipeLeft/Right/Up/Down, Back, Home, Confirm, Cancel) via `InputEvent{action, x, y, timestampMs}` — never raw coordinates. The input adapter supports future sources (buttons, rotary, IMU). Critical actions (delete, cancel, stop recording, dismiss alarm, safe eject, format) always have a visible control, never gesture-only.

## 10. App Router

`AppId` enum for the 12 apps. `App` base: onOpen/onClose/onPause/onResume, update(deltaMs), render, handleInput(InputEvent). Router: open/close apps, back stack, preserve state, protect critical operations, restore Home, status display, system interruptions.

## 11. Today App

Default practical dashboard: time, date, current weather, next event, active timer, next alarm, latest voice note, Wi-Fi status, SD status. Offline: local time, cached weather **with last-updated timestamp** (never as current), local calendar, alarms/timers, SD content.

## 12. Clock App

Digital clock (optional analog), alarms (create/enable/time/repeat/sound/snooze/dismiss/persist across reboot), countdown timer, stopwatch, focus timer, optional world clock. Preset labels: Wake up, Meeting, Leave, Break, Medication, Reminder; custom labels over USB serial. Time selection via pickers, never text entry.

## 13. Weather App

Current conditions, temperature, high/low, precipitation chance, next three days, last-updated time, location. Location via serial-configured city, setup portal, stored coordinates, optional IP approximation. Cache last response. Tolerate: no internet, API timeout, invalid response, missing location, rate limits. Provider isolated behind a service interface (**Open-Meteo** in this repo — no API key).

## 14. Calendar App

Agenda viewing over editing: today's agenda, next event, upcoming, details, dismiss/complete local reminders, manual refresh. Local creation presets (in 15/30/60 min, tomorrow morning/afternoon, custom). Full titles over USB serial or imported files. Storage `/littlecube/calendar/` (JSON internal; ICS import/export where practical).

## 15. Notes App

Primarily a note reader and voice-note capture device. On-device: browse, open, scroll, prev/next, favorite, pin to Home, archive, delete with confirmation, font size, metadata, record a voice note, timestamped quick-note placeholders (Idea, Task, Build, Reminder, Journal, Shopping, Personal). Formats: `.txt`, `.md`. Storage `/littlecube/notes/text/` and `/littlecube/notes/audio/`.

**No on-screen QWERTY keyboard.** Long text entry via USB serial, the local provisioning page, SD transfer, or a future companion interface.

> **This repo additionally interops with the CrowPanel writer deck:** the
> Notes app also lists/reads/writes clean `.md`/`.txt` under
> `/cypher-puter/desk/notes/` (no frontmatter, `.tmp`/`.bak` siblings
> tolerated, notebooks one level deep) so a swapped SD card round-trips.

## 16.–18. USB Serial Interface

Line-based commands, readable responses, clear errors, `help`, no Wi-Fi password logging, non-blocking parsing, bounded buffers, multiline input, timeout/cancel, shared service APIs (commands never manipulate hardware/files directly).

Command families: general (help, status, version, reboot, uptime); notes (list/new/show/write/append/rename/delete/favorite/pin/import/export); files (list/tree/cat/mkdir/copy/move/rename/delete); storage (status/mount/eject/usage/index/backup); wifi (scan/connect/status/disconnect/forget/offline); recordings (list/rename/delete/info); audio (list/play/pause/resume/stop/next/previous, volume); calendar (list/show/add/delete/import/export); contacts (list/show/add/edit/delete/import/export); settings (list/get/set/reset).

Multiline entry: `notes write <file>` then text; control lines `.END` (save), `.CANCEL`, `.PREVIEW`, `.CLEAR` — terminator never stored; bounded in-memory buffer; stream to temp file for large input; atomic rename on save; temp deleted on cancel.

## 19.–22. Wi-Fi

Serial password entry: never print, never log, never store on SD; secure credential storage (NVS); clear temp buffers. Terminal echo may not be controllable — documented honestly.

`WifiState`: Disabled, Idle, Scanning, NetworksFound, Connecting, Connected, ConnectedNoInternet, AuthenticationFailed, NetworkNotFound, CaptivePortalSuspected, Disconnected, Error.

Features: scan, SSID list, signal strength, security indicator, saved-network indicator, auto-connect, reconnect, forget, hidden SSID, internet test, offline mode, captive-portal hint, setup hotspot provisioning, serial provisioning.

On-device flow: Settings → Wi-Fi → scan → select → method (Phone Setup / USB Serial / optional manual carousel) → progress → internet test → save → back.

Phone-assisted provisioning: cube starts AP `LittleCube-XXXX`; user connects and opens 192.168.4.1; portal offers network list, SSID, password (show/hide), hidden network, device name, timezone, weather location, connection test, save. Security: never log passwords, shut down after success, timeout on inactivity, device-specific protection when practical, never expose while on an untrusted network unless explicitly enabled.

## 23. Recorder App

Start/pause/resume/stop/playback/delete/favorite/rename-over-serial, duration, remaining-space estimate. Storage `/littlecube/recordings/`, naming `REC_YYYYMMDD_HHMMSS.wav`, `.partial` during recording. Preflight: SD mounted, writable, free space, estimated time. During: incremental writes, periodic flush, elapsed time, block safe eject, low-space warning. Full card: stop cleanly, preserve valid audio, explain. Card removed: stop, recovery metadata internally, no crash, warn incomplete.

## 24. Audio App

Categories: Music (`/littlecube/music/`), Podcasts (`/littlecube/podcasts/`), Radio (`/littlecube/radio/`), Recordings. Controls: play/pause/stop/prev/next/seek/volume/sleep timer/favorite/resume position. Only claim formats the chosen decoders support. Podcasts: manual RSS, episode list, stream or download (`.partial`), resume, delete; **no recommendation feed**. Radio: manual station URLs / preset file.

## 25. Files App

Browse, open supported files, details, rename, move, copy, delete, create folder, sort, storage usage, safe eject. Text: .txt/.md/.json (+.csv optional); audio per decoder support; images optional. Unknown files visible but not openable.

## 26. Contacts App

Reference-first: list, search by initial/scroll, details, favorites, associate voice note. Fields: id, name, nickname, phone, email, notes (JSON). Editing over serial or imports. No implied calling.

## 27. Calculator App

Add, subtract, multiply, divide, decimal, percentage, clear, backspace. Large touch layout. No scientific functions in v1.

## 28. Settings App

Sections: Wi-Fi, Display (brightness, auto-dim, timeout, bedtime, always-on, clock layout, pixel shifting, orientation), Sound (volume, alarm volume, notification sounds, output, mic level), Date & Time, Storage (SD status/capacity/used/free/filesystem, safe eject, rebuild index, backup, format), Serial, Weather Location, Input, About, Restart, Reset.

## 29.–36. Storage

Internal flash (NVS/LittleFS): device config, UI prefs, secured Wi-Fi credentials, alarms, last screen, recovery metadata, small indexes, boot state. SD: notes, recordings, music, podcasts, radio presets, calendar, contacts, documents, exports, backups, caches. **SD not required to boot.**

Tree: `/littlecube/{notes/{text,audio},recordings,music,podcasts/{feeds,downloads},radio,calendar,contacts,documents,exports,backups,cache,system/{indexes,recovery}}`.

`SdCardState`: NotPresent, Mounting, Mounted, ReadOnly, UnsupportedFilesystem, Corrupted, Full, RemovedUnexpectedly, Error — always specific, never generic.

Insertion: detect → mount → validate → capacity → writable → locate tree → create missing → load indexes → scan media (non-blocking). Removal idle: close, unmount, mark unavailable, update apps. Removal during write: stop, flush when possible, recovery metadata internally, mark incomplete, no crash, clear error.

Safe eject (Settings, Files, `storage eject`): stop playback → stop recording → cancel downloads → finish writes → flush → close → unmount → confirm.

Atomic writes: `.tmp` → flush → close → verify → `.bak` old → rename → drop `.bak`. Recordings/downloads: `.partial` until complete.

Backups `/littlecube/backups/`: notes, calendar, contacts, alarms, safe settings, indexes. Never: Wi-Fi passwords, device credentials, caches, incomplete downloads, temp files. Exports: notes as Markdown, contacts JSON/CSV, calendar JSON/ICS, settings sanitized JSON.

## 37. AMOLED Protection (required)

Screen timeout, automatic dimming, bedside low-brightness mode, pixel shifting (a few px at safe intervals), alternate clock layouts, periodic movement of static elements, configurable always-on, automatic screen-off, reduced brightness for persistent content. Avoid: fixed white max-brightness clock, permanently static icons, long-lived high-contrast borders, unchanging nav bars.

## 38. Event Bus

`SystemEvent`: Wifi{ScanStarted, ScanCompleted, Connecting, Connected, Disconnected}, Internet{Available, Unavailable}, Sd{Inserted, Mounted, Removed, Full, Error}, Recording{Started, Paused, Stopped}, Audio{Started, Paused, Stopped}, AlarmTriggered, TimerCompleted, SerialCommandReceived, Backup{Started, Completed, Failed}. Services publish events; they don't poke unrelated UI.

## 39. Service Interfaces

NotesService, WifiService, ProvisioningService, SdCardService, AudioService, TimeService, SerialCommandService — responsibilities per the full spec; serial commands always go through these.

## 40.–42. Project Structure, Entry Point, Build Scripts

See the repository as built: `little-cube-os/` sketch with `src/{core,hardware,storage,services,serial,ui,apps}`, `libraries/Arduino_DriveBus` vendored, `scripts/{build,upload,monitor,install-libraries}.sh` (env-guarded), `docs/`. (Adjustment from the original spec: module folders live under `src/` because Arduino CLI only compiles the sketch root and `src/**` recursively.)

## 43. Reliability Requirements

Boot without Wi-Fi and without SD; preserve alarms and notes across reboot; reconnect to saved Wi-Fi; recover from failed auth; tolerate unavailable APIs; no blocking during scans/playback/serial; safe SD removal; no password exposure; no crash on corrupt files; recover from invalid settings; malformed serial input cannot exhaust memory.

## 44. Security Requirements

Never log Wi-Fi passwords; never store them on SD; clear temp credential buffers; sanitize serial file paths; block `../` traversal; restrict file ops to `/littlecube/` (plus the deck interop root in this repo); validate filenames; limit command length and multiline size; confirm destructive operations; time out the setup hotspot; disable the portal after provisioning; don't expose serial commands over the network by default.

## 45. Acceptance Criteria

Boot/nav: boots into Home; carousel swipes; apps open/close; Back/Home work; boots without Wi-Fi; boots without SD.
Notes: .txt/.md listed and read; created over serial; multiline works; survive reboot; atomic writes; voice notes to SD; pin/favorite.
Serial: help lists commands; invalid commands readable; long input safe; multiline save/cancel; passwords never printed; commands use services; serial responsive during UI.
Wi-Fi: scan; saved reconnect; phone provisioning; serial provisioning; auth failures specific; connected-no-internet distinct; captive-portal explained; offline mode.
SD: mounts; directories created; free space reported; idle removal handled; removal during recording handled; safe eject; full detected; read-only detected; corrupt files don't crash.
Audio: mic capture; incremental writes; .partial; playback; volume; stops safely on card removal.
AMOLED: brightness; timeout; pixel shift; bedside dim; always-on configurable; no indefinitely fixed static elements.

## 46. Hardware Validation Checklist

Lives in `docs/hardware-validation.md`. **Nothing may be checked without physical testing.**

## 47. Development Phases

1 hardware shell → 2 storage foundation → 3 Wi-Fi → 4 core apps → 5 audio → 6 reliability.

## 48. First Complete Milestone

Boot → swipe Home cards → mount SD → read a note → create a multiline note over USB serial → reboot → reopen the note → enter Wi-Fi setup mode → connect through a phone → update weather → record a voice note → play it back.

The display is the calm interface; USB serial handles typing; Wi-Fi handles connected information; SD stores the user's content.
