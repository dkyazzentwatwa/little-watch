# UI Refresh + Sound — Design

Date: 2026-07-26 · Status: approved (chat) · Owner: cypher

## Goal

Make Little Cube OS look like a finished product instead of a debug console,
and give the speaker real volume plus an on-device Sound screen.

Three things the user asked for:

1. Better home cards and better app screens (all 15 apps hand-tuned).
2. A Sound section in Settings covering the whole audio path.
3. A louder speaker, now that the microphone works.

## Decisions (user-confirmed)

- **Cards**: icon + live fact. Drawn icon, bold name, and one live line where a
  service already knows something cheap; static hint otherwise.
- **Screens**: all 15 apps hand-tuned, on top of a shared style layer.
- **Sound screen**: everything audio — speaker volume, test tone, mic ADC gain,
  normalize, noise gate.
- **Phasing**: 4 phases, each independently flashable and hardware-verifiable.

## 1. Type system

Adafruit_GFX_Library is already passed to `arduino-cli` via `scripts/build.sh`,
and its `Fonts/Free*` headers use the same `GFXfont` struct Arduino_GFX's
`setFont(const GFXfont*)` expects. Cost measured: ~18 KB of bitmap data for all
five faces, against a firmware currently using 10% of 16 MB. No new dependency,
so `build.sh` / `install-libraries.sh` / `sketch.yaml` are untouched.

Four roles:

| Style | Font | Use |
|---|---|---|
| `Display` | FreeSansBold24pt7b | Clock face, one hero number per screen |
| `Title` | FreeSansBold18pt7b | Screen headers, card names |
| `Body` | FreeSans12pt7b | List rows, buttons, running text |
| `Caption` | FreeSans9pt7b | Hints, timestamps, secondary lines |

**The baseline problem.** With `setFont(nullptr)` (today) `setCursor` takes the
text's top-left. With a `GFXfont` it takes the *baseline*, so every one of the
154 existing `setCursor` sites would shift by roughly a line height. Rather than
rewrite coordinates, `widgets/` absorbs the difference:

```cpp
namespace widgets {
enum class TextStyle : uint8_t { Display, Title, Body, Caption };

// x/topY are the TEXT'S TOP-LEFT, matching every existing call site.
void text(Arduino_GFX&, int16_t x, int16_t topY, const char* s, TextStyle, uint16_t color);
void textCentered(Arduino_GFX&, int16_t x, int16_t topY, int16_t w, const char* s,
                  TextStyle, uint16_t color);
void textRight(Arduino_GFX&, int16_t rightX, int16_t topY, const char* s,
               TextStyle, uint16_t color);
int16_t textWidth(const char* s, TextStyle);
int16_t lineHeight(TextStyle);
}
```

Each applies `setFont`, then offsets `topY` by that face's ascent before
`setCursor`. Migration is therefore a mechanical replacement of
`setTextSize/setTextColor/setCursor/print` quadruples with one call — existing
layout arithmetic keeps working.

`theme::kTextSize*` constants remain defined until the last app is migrated, so
the tree compiles at every step. `widgets::textBlock` re-implements its word wrap
against `textWidth()` because proportional glyphs break the `w / (6 * size)`
character-count assumption.

Non-ASCII (>0x7E) is not covered by the Free* faces; those strings fall back to
the built-in font via `setFont(nullptr)`. The bundled u8g2 CJK faces stay unused
— they would cost ~200 KB for a device with no CJK content.

## 2. Cards: icon + live fact

`Carousel::Card` gains `IconId icon`. A new `ui/Icons.{h,cpp}` exposes

```cpp
void icons::draw(Arduino_GFX&, IconId, int16_t x, int16_t y, int16_t size, uint16_t color);
```

drawn entirely with GFX primitives (`fillRoundRect`, `fillCircle`, `fillTriangle`,
`drawLine`, arcs) — no bitmap assets, no new library, and every icon inherits the
active theme color.

The live line comes from one function in `Carousel.cpp`:

```cpp
// Returns nullptr when nothing live is known; the card then shows Card::hint.
const char* glanceFor(AppId, Services&, char* buf, size_t cap);
```

Sources, all cache-only — no SD walk, no network, no I2C, so the carousel holds
30 fps: Clock → `SystemState::clockHhMm`; Weather → cached temp + condition;
Recorder → cached take count; Assistant → key/offline state; Audio → now-playing
title; Files → cached SD free space. Everything else returns `nullptr`.

Card layout: icon (72 px) centered above the name in `Title`, glance-or-hint
below in `Caption`, page indicator as a compact progress pill.

## 3. Sound

### Louder speaker

`Es8311::setVolume` currently maps 1–100% onto REG32 `0x5C…0xBF`, and **0xBF is
exactly 0 dB** — there is no headroom above unity today. REG32 runs to 0xFF at
0.5 dB/step, so the new mapping is piecewise:

- 0% → mute (register 0x00)
- 1–80% → −40 dB … 0 dB (0.5 dB per step)
- 81–100% → 0 dB … +10 dB (boost)

80% is the clean unity reference and is marked as such in the UI. Above it the
gain is digital, so hot content clips; the win is real for quiet TTS replies and
voice notes, and the UI says so rather than pretending otherwise.

### Settings → Sound screen

New `Screen::Sound` in `SettingsApp`: speaker volume stepper (unity marked), a
test-tone button (`AudioAdapter::playTone`, refused while recording), mic ADC
gain stepper (0–7, labelled in dB), and normalize / noise-gate toggles.

Persistence: volume already lives in NVS. Mic gain, normalize, and gate are
currently runtime-only and reset every boot — they move into `SettingsService`
(NVS) and are applied by `Kernel` at startup, so the Sound screen's state
survives a reboot.

## 4. Screens

Shared treatment, applied to every app: a header (`Title` + hairline rule at a
fixed y), rows via the restyled `widgets::listItem`, actions via the restyled
`widgets::button`, `Caption` for all secondary text, and `theme::kPadding` /
`kSafeInset` respected so nothing lands in the AMOLED's rounded corners.
`AmoledProtection::shiftX()/shiftY()` offsets stay on all persistent chrome
(spec §37) — a hard requirement, not a nicety.

Hand-tuning order, in batches, flashing between batches:

1. Home, Settings, Assistant, Recorder
2. Today, Clock, Weather, News
3. Notes, Audio, Calendar, Contacts
4. Files, Reader, Calculator

## Testing

Hardware rule from CLAUDE.md applies without exception: nothing is checked off
`docs/hardware-validation.md` without physically running it on the cube. A clean
compile is not validation.

Per phase: build clean → flash → `open <app>` over serial for every touched
screen → visual confirmation by the user. The agent verifies compile, boot,
navigation, and that no screen crashes or renders empty; the user judges
appearance and, for Phase 3, speaker loudness and distortion.

**Explicit limitation**: the agent cannot measure speaker loudness or distortion.
The cube is half-duplex — `AudioAdapter` refuses to record while playing — so the
microphone cannot capture the speaker. Phase 3 therefore ships the test tone and
the volume curve is tuned from the user's reported listening, not from a
measurement.

## Phases

| Phase | Contents | Independently shippable |
|---|---|---|
| 1 | Fonts, `widgets::text`, restyled widgets | Yes — every app improves at once |
| 2 | Carousel icons + live facts | Yes |
| 3 | Volume remap + Settings → Sound + NVS persistence | Yes |
| 4 | 14 remaining screens, 4 batches | Yes, per batch |

## Out of scope

Animated transitions beyond the existing carousel slide, per-app custom themes,
bitmap/PNG icon assets, CJK text, a settings search, and any change to the
gesture model (`InputAction` stays as-is).
