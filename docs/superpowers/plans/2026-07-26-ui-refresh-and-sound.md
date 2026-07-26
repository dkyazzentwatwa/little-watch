# UI Refresh + Sound Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline, this session — the cube and serial port are live here). Steps use checkbox (`- [ ]`) syntax.

**Goal:** Proportional-font UI across all 15 apps, icon + live-fact home cards, a louder speaker, and a Settings → Sound screen.

**Architecture:** A text/style helper layer in `ui/widgets/` absorbs the GFXfont baseline shift so the 154 existing call sites keep top-left coordinates. Icons are GFX primitives in `ui/Icons.{h,cpp}`. Volume gains headroom by extending ES8311 REG32 past unity. Audio prefs move to NVS.

**Tech Stack:** Arduino CLI only; Adafruit_GFX `Fonts/Free*` (already linked, ~18 KB); Arduino_GFX; ES8311 over I2C.

**Plan adaptations (CLAUDE.md overrides skill defaults):** no unit-test suite exists — every task verifies with `./scripts/build.sh` plus on-device serial checks. No git commits; the user keeps this tree uncommitted. Nothing is checked off `docs/hardware-validation.md` without physically running it.

Spec: `docs/superpowers/specs/2026-07-26-ui-refresh-and-sound-design.md`

---

## Phase 1 — Type system + widget layer

### Task 1.1: Text styles and helpers

**Files:** Modify `little-cube-os/src/ui/widgets/Widgets.h`, `Widgets.cpp`

- [ ] Add to `Widgets.h`, inside `namespace widgets`:

```cpp
// Four type roles (see the UI refresh design doc). Adafruit_GFX Free* faces;
// Arduino_GFX takes the same GFXfont struct, so no new dependency.
enum class TextStyle : uint8_t { Display, Title, Body, Caption };

// x/topY are the text's TOP-LEFT — matching every pre-font call site. The
// helpers convert to the GFXfont baseline internally, so existing layout
// arithmetic keeps working.
void text(Arduino_GFX& gfx, int16_t x, int16_t topY, const char* s, TextStyle style,
          uint16_t color);
void textCentered(Arduino_GFX& gfx, int16_t x, int16_t topY, int16_t w, const char* s,
                  TextStyle style, uint16_t color);
void textRight(Arduino_GFX& gfx, int16_t rightX, int16_t topY, const char* s,
               TextStyle style, uint16_t color);
int16_t textWidth(const char* s, TextStyle style);
int16_t lineHeight(TextStyle style);
int16_t ascent(TextStyle style);
```

- [ ] In `Widgets.cpp`, include the faces and implement. `applyStyle` is the only
      place `setFont` is called; `ascent` is the face's cap height, measured once
      via `getTextBounds` on a reference string so the values cannot drift from
      the font data:

```cpp
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

const GFXfont* fontFor(TextStyle s) {
  switch (s) {
    case TextStyle::Display: return &FreeSansBold24pt7b;
    case TextStyle::Title:   return &FreeSansBold18pt7b;
    case TextStyle::Body:    return &FreeSans12pt7b;
    case TextStyle::Caption: return &FreeSans9pt7b;
  }
  return &FreeSans12pt7b;
}
```

`applyStyle(gfx, style)` calls `gfx.setFont(fontFor(style)); gfx.setTextSize(1);`.
`ascent(style)` returns `fontFor(style)->yAdvance * 3 / 4` (cap height is ~3/4 of
line advance for these faces); `text()` draws at `topY + ascent(style)`.
Every helper restores `gfx.setFont(nullptr)` before returning, so any not-yet-
migrated call site still renders with the old built-in font and correct
top-left coordinates.

- [ ] `textWidth` uses `gfx.getTextBounds` — proportional glyphs make the old
      `strlen * 6 * size` formula wrong, and it is what centering depends on.
      It needs a `Arduino_GFX&`; add the overload
      `int16_t textWidth(Arduino_GFX&, const char*, TextStyle)` and keep the
      no-gfx form only if a canvas-free estimate is genuinely needed.

- [ ] Build: `./scripts/build.sh` → clean.

### Task 1.2: Restyle the shared widgets

**Files:** Modify `little-cube-os/src/ui/widgets/Widgets.cpp`

- [ ] `button()`: label via `textCentered(..., TextStyle::Body, ...)`, vertical
      centering from `lineHeight(Body)` instead of `kCharH * size`.
- [ ] `listItem()`: primary in `Body`, secondary in `Caption`; heights become
      `56` (two-line) / `44` (single) to suit the new metrics.
- [ ] `textBlock()`: rewrite the greedy wrap to measure candidate substrings with
      `textWidth()` rather than assuming a fixed character cell. Signature takes
      a `TextStyle` in place of `uint8_t textSize`.
- [ ] `toast()`, `modalConfirm()`: same substitution, `Body` for titles/buttons,
      `Caption` for body copy.
- [ ] Add `header()`: draws a screen title in `Title` at `theme::kPadding` plus a
      hairline rule, returns the y below it, so all 15 apps share one header.

```cpp
int16_t header(Arduino_GFX& gfx, const char* title, int16_t shiftX, int16_t shiftY);
```

- [ ] Update the four in-tree callers of `textBlock` (grep first) to pass a
      `TextStyle`. Build: `./scripts/build.sh` → clean.

### Task 1.3: Flash and confirm nothing regressed

- [ ] `./scripts/upload.sh`; confirm `boot: ready`.
- [ ] Over serial, `open` each of the 15 apps in turn and confirm no crash, no
      blank screen, no watchdog reset (`status` still answers after each).
- [ ] User confirms the new type renders correctly on at least Home and Settings.

## Phase 2 — Carousel icons + live facts

### Task 2.1: Icon primitives

**Files:** Create `little-cube-os/src/ui/Icons.h`, `Icons.cpp`

- [ ] `enum class IconId : uint8_t { Today, Clock, Weather, News, Notes, Reader,
      Recorder, Assistant, Audio, Calendar, Settings, Tools };`
- [ ] `void draw(Arduino_GFX&, IconId, int16_t x, int16_t y, int16_t size, uint16_t color);`
      — every glyph drawn from GFX primitives inside the `size × size` box, no
      bitmaps. Keep each glyph under ~12 primitive calls so a full carousel
      frame stays inside the 30 fps budget.
- [ ] Build → clean.

### Task 2.2: Card model and glance line

**Files:** Modify `little-cube-os/src/ui/Carousel.h`, `Carousel.cpp`

- [ ] Add `IconId icon;` to `Carousel::Card`; fill it in for all 12 entries.
- [ ] Add, in the `Carousel.cpp` anonymous namespace:

```cpp
// Cache-only: no SD walk, no network, no I2C — this runs every carousel frame.
// Returns nullptr when nothing live is known, and the card falls back to hint.
const char* glanceFor(AppId id, Services& services, char* buf, size_t cap);
```

      Clock → `state.clockHhMm`; Weather → cached temp/condition; Recorder →
      cached take count; Assistant → key-missing / offline / ready; Audio →
      now-playing title; Files → cached SD free MB; everything else `nullptr`.
- [ ] `renderCard()`: icon at 72 px centered, name in `Title`, glance-or-hint in
      `Caption`, page dots replaced by a progress pill.
- [ ] Build → flash → verify each carousel position renders and the live values
      match what `status` reports.

## Phase 3 — Louder speaker + Sound screen

### Task 3.1: Volume headroom

**Files:** Modify `little-cube-os/src/hardware/audio/Es8311.cpp`

- [ ] Replace `setVolume` with the piecewise map (0 = mute; 1–80% → −40…0 dB;
      81–100% → 0…+10 dB; register = `0xBF + dB*2`, clamped to 0x00–0xFF).
      Comment must state that above 80% the gain is digital and clips hot
      content — the next reader needs to know it is a deliberate trade.
- [ ] Add `constexpr uint8_t kUnityVolumePercent = 80;` to `Es8311.h` so the UI
      can mark unity without duplicating the number.
- [ ] Build → flash → user judges 80% vs 100% by ear on a TTS reply.
      **The agent cannot measure this** — the cube cannot record while playing.

### Task 3.2: Persist the audio prefs

**Files:** Modify `SettingsService.{h,cpp}`, `Kernel.cpp`

- [ ] Add `micGain()` / `setMicGain(uint8_t)`, `recordNormalize()` /
      `setRecordNormalize(bool)`, `recordGate()` / `setRecordGate(bool)`,
      backed by NVS keys `micgain`, `recnorm`, `recgate` (≤15 chars).
      Defaults: 7, true, true — matching today's runtime defaults.
- [ ] In `kernelSetup()`, after `audioAdapter.begin()`, apply all three from
      settings so a reboot no longer silently resets them.
- [ ] Make `RecordingsCommands`' `gain` / `normalize` / `gate` verbs write
      through `SettingsService` as well, so serial and UI cannot disagree.
- [ ] Build → flash → set each over serial, `reboot`, confirm they survived.

### Task 3.3: Settings → Sound screen

**Files:** Modify `little-cube-os/src/apps/SettingsApp.{h,cpp}`

- [ ] Add `Screen::Sound` to the enum, a "Sound" row on the root screen, and
      `renderSound(Arduino_GFX&)`.
- [ ] Controls: volume stepper (−/+, unity marked at 80%), "Test tone" button
      (`services_.audio->playTone(880, 400)`, refused while recording or
      playing), mic gain stepper 0–7 labelled in dB, normalize toggle, gate
      toggle. Every control writes through `SettingsService` and applies live.
- [ ] Build → flash → user exercises every control on-device.

## Phase 4 — Screen hand-tuning

Four batches; build + flash + serial-open every touched screen after each batch.
Per screen: `widgets::header()` for the title, `listItem`/`button` for rows and
actions, `Body`/`Caption` for text, `theme::kPadding`/`kSafeInset` respected, and
`AmoledProtection::shiftX()/shiftY()` still applied to persistent chrome
(spec §37 — burn-in defense is a hard requirement, not a nicety).

- [ ] **Batch A**: `HomeApp`, `SettingsApp`, `AssistantApp`, `RecorderApp`
- [ ] **Batch B**: `TodayApp`, `ClockApp`, `WeatherApp`, `NewsApp`
- [ ] **Batch C**: `NotesApp`, `AudioApp`, `CalendarApp`, `ContactsApp`
- [ ] **Batch D**: `FilesApp`, `ReaderApp`, `CalculatorApp`
- [ ] After Batch D: remove the now-unused `theme::kTextSize*` constants and
      confirm no call site references them (`grep -rn kTextSize src/`).

## Final verification

- [ ] `./scripts/build.sh` clean with no new warnings from `src/`.
- [ ] Every app opens, renders, and returns to Home over serial without a reset.
- [ ] `docs/hardware-validation.md` gains a UI section; only physically
      exercised items are checked.
- [ ] User signs off on appearance and on speaker loudness.

## Self-review notes (done at write time)

Spec coverage: type system → 1.1/1.2; cards → 2.1/2.2; louder speaker → 3.1;
Sound screen → 3.3; NVS persistence → 3.2; all-15 hand-tuning → Phase 4;
hardware-only validation → every phase's flash step. Type consistency checked:
`TextStyle`, `IconId`, `glanceFor`, `header`, `kUnityVolumePercent` are each
defined once and referenced with the same names throughout. Known risk recorded
in 1.1: `textWidth` needs a canvas for `getTextBounds`, so the signature takes
`Arduino_GFX&` — call sites in `toast()`/`textCentered()` already have one.
