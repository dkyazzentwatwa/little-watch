# Screen Polish Design — Clock, Today, Weather, News, Recorder, Assistant

**Date:** 2026-07-31
**Status:** approved

## Goal

Bring the six screens that were never migrated to the 2026-07-26 type system up to
the standard of the rest of the device, fix the layout bugs that cut text off at
the panel's rounded corners, and give the Clock a set of selectable faces —
three typographic and four ASCII-art.

## Why these six

The user's complaint list ("looks bland", "stuff cutoff") maps exactly onto two
mechanical causes found in the code, not onto taste:

**Cause 1 — the type system was never applied here.** The 2026-07-26 UI refresh
introduced `widgets::TextStyle` (proportional FreeSans faces) and
`widgets::header()`. FilesApp, ContactsApp, NotesApp, SettingsApp, CalendarApp,
ReaderApp, AudioApp and VideoApp all use it. ClockApp, TodayApp, WeatherApp,
AssistantApp and RecorderApp use **none** of it — they still draw with the
built-in 6x8 bitmap font and hardcoded `setTextSize()`. NewsApp is partially
migrated (3 call sites). The user is looking at bitmap-font screens sitting
beside proportional-font screens.

**Cause 2 — footers ignore the safe inset.** `theme::kSafeInset = 20` exists
because the AMOLED is a rounded square whose corners are eaten by the bezel
radius. `ReaderApp.cpp:383` gets this right (`footX = theme::kSafeInset`). Every
footer on the six screens draws at `x = theme::kPadding` (12) — inside the corner
radius. That is the reported "cut off on bottom left" in the News detail view.

**Cause 2b — footers also ignore the burn-in shift.** No footer on the device
passes `AmoledProtection::shiftX()/shiftY()`. Bottom chrome is as persistent as
the status bar, so this is a standing spec §37 violation that the same fix
closes.

## Decisions

| Decision | Choice |
|---|---|
| Scope | Polish pass now; timers/stopwatch/alarms get their own spec |
| QR codes | Yes — add a QR encoder library |
| Weather views | Condition icon on the hero, tap-cycled Now/Forecast/Details, per-day icons |
| Clock faces | Big digital, Stacked typographic, Word clock, **Block, Prompt, Segment, Binary** |
| Face blanking | Faces respect the screen timeout like every other screen |
| Today content | Weather with icon, battery as a real row |

⚠️ **Face list revised 2026-07-31, after seeing Task 5 on hardware.** The
original three character faces — Blinky, Big Eyes and Mood Cube — were
**scrapped before being built**. With the first three faces on the actual
panel, the user's reaction was that character faces were not what this device
wanted, and asked instead for "minimalistic ASCII hacker art" faces. Four
replace the three, for seven total:

- **Block** — figlet-style digits built from block characters
- **Prompt** — a fake shell session with a blinking cursor
- **Segment** — classic `_` and `|` ASCII seven-segment digits
- **Binary** — filled/hollow dots encoding hours and minutes

This is what flashing early bought: three unbuilt renderers changed direction
at the cost of an edit rather than after they existed. It also cost one line
to widen `kFaceCount` from 6 to 7, because an earlier review had already
removed the duplicated count from `SettingsService`.

Three choices were made on the user's behalf during design and explicitly
confirmed:

1. **Weather tap is reassigned.** Tap currently means *refresh*; it now means
   *next view*. Refresh becomes a visible button in the Details view plus a
   long-press anywhere, so it stays discoverable rather than becoming a hidden
   gesture.
2. **Today loses two rows.** `Next` and `Alarm` are deleted rather than
   restyled. Nothing in the codebase ever writes `SystemState::alarmArmed`, and
   `"- nothing scheduled"` is a hardcoded string — `CalendarService` holds real
   events but `TodayApp` never calls it. Permanent placeholder dashes are the
   screen stating something it cannot know.
3. **Tracking parameters are stripped before QR encoding.** A shorter URL means
   a lower QR version and larger modules, which materially improves scan
   reliability off an AMOLED. The link *text* stays complete on its own page.

## Out of scope

Timers, stopwatch and alarms (own spec — they need a service, persistence, and a
sound that fires from other apps). Hourly weather data. Wiring `CalendarService`
into Today.

### Known and deliberately deferred

The corner-clipping footer bug (cause 2) is **not** limited to the six screens in
this spec. The same `setCursor(theme::kPadding, DISPLAY_HEIGHT - 28)` pattern
appears in ContactsApp, AudioApp, CalendarApp, HomeApp, SettingsApp, NotesApp and
FilesApp. Once `widgets::footer()` exists, converting them is close to mechanical.

They are excluded here because each conversion needs its own on-device check and
this pass is already seven components wide — not because they are correct. This
is a cheap, well-defined follow-up, and it should be taken before the burn-in
finding goes stale.

## Constraints inherited from the codebase

- Arduino CLI only. A new library must be registered in **three** places:
  `scripts/build.sh`, `scripts/install-libraries.sh`, and the `littlecube`
  profile in `little-cube-os/sketch.yaml`.
- Nothing in `kernelLoop()` may block. `CalendarService`, `NotesService` and
  `RecorderService` reads touch SD and must be called from `onOpen()`/
  `onResume()`, never from `render()`.
- Persistent chrome must offset by `AmoledProtection::shiftX()/shiftY()`.
- Apps consume semantic `InputAction`, never raw coordinates.
- Critical actions need a visible control, never gesture-only.
- Everything degrades: every screen must render with no SD, no Wi-Fi and no
  valid time.
- Icons are drawn from `Arduino_GFX` primitives. There are no bitmap assets in
  this codebase and this design adds none.

---

## Component 1 — Shared foundation

### 1.1 `widgets::footer()`

Added to `little-cube-os/src/ui/widgets/Widgets.{h,cpp}`, mirroring the existing
`widgets::header()`.

```cpp
// Bottom band: a hairline rule with left- and optionally right-aligned caption
// text, inset by kSafeInset so nothing lands in the bezel's corner radius.
// This is persistent chrome, so it takes the AmoledProtection offsets
// (spec §37). `right` may be nullptr. Returns the y of the rule.
int16_t footer(Arduino_GFX& gfx, const char* left, const char* right,
               int16_t shiftX, int16_t shiftY);
```

Geometry **as shipped and confirmed on device (2026-07-31)**: rule at
`DISPLAY_HEIGHT - 44 + shiftY` spanning `kPadding` to `DISPLAY_WIDTH - kPadding`
so it aligns with `header()`'s rule; caption text at `ruleY + 8`, inset to
`kSafeInset` because the ink sits ~19 px off the bottom edge, inside the corner
zone the rule at 44 px up does not reach. Returns `ruleY - 12` — a padded
content budget, mirroring `header()`.

`right` holds action hints and wins the space; `left` holds status and
ellipsizes into what remains. Both are drawn in `kTextDim`: `kPanelAlt` is a
fill colour and measures 1.24–1.40:1 against `kBg` on all ten palettes, i.e.
invisible.

Every footer on the six screens routes through this helper. No screen keeps a
hand-placed `setCursor(theme::kPadding, DISPLAY_HEIGHT - 28)`.

### 1.2 Weather glyphs in `icons::`

Added to `little-cube-os/src/ui/Icons.{h,cpp}`. New `IconId` values:
`WxClear`, `WxPartlyCloudy`, `WxCloudy`, `WxRain`, `WxSnow`, `WxThunder`,
`WxFog`. Drawn from primitives in the same style as the existing home-card
glyphs, taking the active theme's colors.

```cpp
// Maps a WeatherSnapshot condition string to its glyph. The producing table
// (conditionFromWmo in the weather service) emits a closed set of 11 strings,
// so this is exact matching, not substring guessing. A cache file edited by
// hand can still contain anything, hence the fallback.
IconId forCondition(const char* condition);
```

| Condition string | Glyph |
|---|---|
| `Clear` | `WxClear` |
| `Mostly clear` | `WxPartlyCloudy` |
| `Overcast` | `WxCloudy` |
| `Fog` | `WxFog` |
| `Drizzle`, `Rain`, `Showers` | `WxRain` |
| `Snow`, `Snow showers` | `WxSnow` |
| `Thunderstorm` | `WxThunder` |
| `Weather` (service's own unknown) and anything unrecognised | `WxCloudy` |

The `icons::` header comment is updated — it currently says "Home-card glyphs",
which stops being the whole truth.

### 1.3 `ui/QrCode.{h,cpp}`

New file. Wraps `ricmoo/QRCode` so the dependency is included in exactly one
translation unit.

```cpp
namespace qrcode {
// Largest module size that fits maxSizePx is chosen automatically. Returns
// false when the text does not fit kMaxVersion, when maxSizePx is too small
// for one pixel per module, or when text is empty — the caller draws its own
// fallback rather than a corrupt code.
bool draw(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t maxSizePx,
          const char* text, uint16_t fg, uint16_t bg);
}  // namespace qrcode
```

Version selection walks ascending from 3 to `kMaxVersion = 8` (49x49 modules) at
ECC level `LOW`, taking the first that accepts the text. The working buffer is a
file-scope `static uint8_t` sized by `qrcode_getBufferSize(kMaxVersion)`; it is
too large for a comfortable stack allocation inside a render call.

A QR needs a quiet zone to scan. `draw()` fills a `bg` rectangle four modules
wider than the code on every side and never inverts — dark modules must be the
dark value against a light quiet zone, which on a dark theme means drawing the
code on an explicitly light background rather than inheriting `theme::kBg`.

### 1.4 Library registration

`ricmoo/QRCode` added to `scripts/install-libraries.sh`, passed via `--library`
in `scripts/build.sh`, and listed in the `littlecube` profile in
`little-cube-os/sketch.yaml`. Adding it to fewer than all three breaks either the
CLI build or the profile build.

---

## Component 2 — Clock faces

### 2.1 File structure

`ClockApp.cpp` is 96 lines today. Six faces inline would make it the opposite of
focused, so faces live in a new pair of files:

- **Create** `little-cube-os/src/apps/ClockFaces.{h,cpp}` — one render function
  per face, no app state.
- **Modify** `little-cube-os/src/apps/ClockApp.{h,cpp}` — face selection,
  persistence, animation clock, input.

### 2.2 Interface

```cpp
namespace clockfaces {

enum class FaceId : uint8_t {
  BigDigital,   // default
  Stacked,
  Word,
  Block,
  Prompt,
  Segment,
  Binary,
};
constexpr uint8_t kFaceCount = 7;

const char* name(FaceId id);   // shown briefly on switch, and in serial output

struct FaceContext {
  const struct tm* time;   // nullptr when the clock is not set
  const SystemState* state;  // shared device status, available to any face
  uint32_t animMs;         // monotonic ms since the app opened
  int16_t shiftX;
  int16_t shiftY;
};

// Draws the face into the canvas. Returns true when the face wants another
// frame soon (an animation is in flight), false when it is static until the
// minute rolls over. ClockApp uses this to keep the dirty model honest instead
// of redrawing unconditionally.
bool render(Arduino_GFX& gfx, FaceId id, const FaceContext& ctx);

}  // namespace clockfaces
```

The `bool` return is the load-bearing part of this design: it is what stops an
animated face from pinning the frame loop and the panel at full tilt.

### 2.3 The faces

**BigDigital** — the current `HH:MM`, restyled in `TextStyle::Display`, centered,
date in `Caption` below. Static. Retains today's deliberate `kTextDim` choice
rather than pure white (spec §37 names a fixed white max-brightness clock as
exactly the burn-in case).

**Stacked** — hour above minute in `Display`, left-aligned against the safe
inset, date small at the bottom. Static.

**Word** — the time as English text, wrapped, in `Title`. Static. Built from a
fixed phrase table: minute rounded to the nearest five, `"o'clock"`, `"five
past"`, `"ten past"`, `"quarter past"`, `"twenty past"`, `"twenty-five past"`,
`"half past"`, then the `to` forms, with the hour advanced past `"half past"`.
Hours name the 12-hour form. Renders `"time not set"` when `time` is nullptr.

**Block** — the time as figlet-style digits built from block characters, date
in small caps beneath. Static.

**Prompt** — a fake shell session: a prompt line, a `date +%H:%M` command, the
time in large type, the date, and a trailing prompt with a **blinking cursor**.
The cursor is the only animation among the ASCII faces; at ~1 Hz it asks for a
frame roughly every 500 ms rather than every tick.

**Segment** — digits drawn from `_` and `|` in the classic ASCII seven-segment
style, three text rows per digit. Static.

**Binary** — two rows of filled/hollow dots encoding hours and minutes, with a
small decimal readout beneath so the face is never unreadable. Static.

⚠️ **The four ASCII faces use the built-in 6x8 bitmap font, not the proportional
FreeSans faces.** This is deliberate and is the one place in the codebase where
the pre-migration font is the correct choice: ASCII art depends on every glyph
occupying the same cell, and proportional glyphs destroy the alignment that
makes `_` and `|` read as segments. Call `gfx.setFont(nullptr)` explicitly
rather than relying on the widget helpers having restored it.

### 2.4 Animation and the frame budget

`clockfaces::render()` returns **milliseconds until the face wants its next
frame** — `0` for as-soon-as-possible, `kFaceStatic` for nothing-moves-until-the
-minute-rolls. `ClockApp::update(deltaMs)` counts that down and marks dirty when
it reaches zero.

⚠️ **It is not a bool, and that matters.** A bool can only say "static forever"
or "redraw every tick". Prompt's cursor blinks at ~1 Hz: returning false between
blinks would starve it to the ~2 frames/min the minute roll and the pixel shift
produce, so it would never blink; returning true would repaint the full 322 KB
PSRAM canvas and flush QSPI at 30 fps continuously, on the screen users leave
open longest. Returning ~500 ms costs two frames per second instead of thirty.

Six of the seven faces are static and return `kFaceStatic`. Only Prompt
animates.

Faces respect the screen timeout. Nothing here calls
`AmoledProtection::keepAwake()` — that path exists for video playback and stays
there. An animated face is incidentally *good* for burn-in (moving pixels), but
it is exactly the screen a user leaves on, so blanking must still apply.

### 2.5 Selection and persistence

Tap cycles to the next face and persists immediately. `SettingsService` gains a
persisted `uint8_t`, cloning the existing `themeIndex()` pattern:

```cpp
uint8_t clockFace() const { return clockFace_; }
void setClockFace(uint8_t value);   // clamps to kFaceCount - 1
```

NVS key `clockface` in the existing `littlecube` namespace. An out-of-range
stored value clamps to `BigDigital` on load, so a downgrade after adding faces
cannot brick the screen.

The footer carries `"tap: next face"` on the left and the face name on the
right, replacing today's `"alarms & timers: coming soon"` string sitting in the
dead corner.

---

## Component 3 — Weather

**Modify** `little-cube-os/src/apps/WeatherApp.{h,cpp}`.

```cpp
enum class View : uint8_t { Now, Forecast, Details };
```

Tap cycles `Now -> Forecast -> Details -> Now`. Long-press refreshes from any
view. The Details view additionally carries a visible `Refresh` button, so the
action is never gesture-only.

**Now** — location and freshness at the top, the condition glyph at 96 px beside
the temperature in `Display`, condition text, then `H / L / rain %`.

**Forecast** — three rows, each: day label, condition glyph, high/low, rain %.
The glyph replaces the trailing condition text, which already truncates to
`Overcas` on the device.

**Details** — location, full condition text, rain chance, today's high/low, last
updated (absolute local time when `fetchedAtEpoch` is known, relative otherwise),
connection state from `state.internet`, and the `Refresh` button.

Freshness rules are unchanged and remain load-bearing: cached data always carries
its age and stale data is never presented as current (spec §11).
`fetchedAtUptimeMs == 0` renders as `cached (before restart)` in `kWarn`.

The no-data state keeps its current distinct messages — `offline - no cached
weather yet` versus `no location set - use phone setup` — restyled, with the
footer hint updated because tap no longer means refresh there either.

Footer per view: `"tap: forecast"` / `"tap: details"` / `"tap: now"` on the
left, `"hold: refresh"` on the right.

---

## Component 4 — Today

**Modify** `little-cube-os/src/apps/TodayApp.{h,cpp}`. Cards, not label/value
rows, using the existing `widgets::card()`.

1. **Hero** — time in `Display`, date in `Caption` beneath. `--:--` and
   `date pending time sync` when the clock is unset, as today.
2. **Weather card** — condition glyph, temperature, condition text, high/low,
   and the existing staleness warning. Reuses `icons::forCondition()`.
3. **Status card** — battery percent and charge state as a real row, storage
   state, and connection state. Battery currently appears only as a status-bar
   pip.
4. **Recording row** — drawn in `kBad` only while `state.recording`; absent
   otherwise, rather than showing a dash.

Deleted: the `Next` and `Alarm` rows, per the decision above.

No SD access is added. Everything on this screen comes from `SystemState` and the
weather snapshot, both pure RAM, so `render()` stays safe and `TodayApp` needs no
`onOpen()` load.

---

## Component 5 — Assistant

**Modify** `little-cube-os/src/apps/AssistantApp.{h,cpp}`.

- `widgets::header("Assistant", ...)` replaces the hand-placed title.
- **Bubbles.** The question renders right-aligned on `kPanelAlt`; the answer
  left-aligned on `kPanel`, both rounded. This replaces two runs of undifferen-
  tiated wrapped text. The file-local `printWrapped()` helper is dropped in
  favour of `widgets::textBlock()`, which measures real glyph widths — the
  existing helper assumes a fixed character cell and will wrap wrongly the
  moment the screen uses a proportional face.
- **Listening meter.** A horizontal level bar driven by
  `AudioAdapter::recordedPeak()`, drawn only in the `Listening` state, with a
  decay so it falls smoothly rather than flickering. The peak is already tracked
  as a `volatile uint16_t`; no change to the audio capture path.
- **Answer paging.** Long answers currently truncate silently at 11 lines. The
  answer paginates and swipe up/down moves between pages, matching NewsApp. The
  footer shows `pg n/m` when there is more than one page.
- Error text keeps stacking above the transcript so a failed question stays
  visible. `Back` during `Listening` still cancels the take and never sends.

---

## Component 6 — News

**Modify** `little-cube-os/src/apps/NewsApp.{h,cpp}`.

- The detail footer routes through `widgets::footer()`, which is the reported
  bottom-left cut-off. Left: `pg n/m  A<size>`. Right: `swipe: prev/next`.
- The **list** view footer (`NewsApp.cpp:300`) has the same corner-clipping bug
  and is converted in the same pass. Both footers on this screen, not just the
  one the user happened to photograph.
- **QR page.** The composed detail gains one final page carrying the code,
  centered, with a `scan to open` caption and the article's host name beneath it.
  Reaching it is the existing paging gesture — `pg 3/3` simply *is* the QR page.
  The complete link text keeps its own page before it.
- When `qrcode::draw()` returns false — a URL too long for version 8 — the page
  renders `link too long to encode` plus the URL text instead. A missing QR is a
  degraded page, never a blank one or a corrupt code.

Tracking parameters are stripped before encoding by a file-local helper in
`NewsApp.cpp`. The rule is deliberately conservative: the query string is dropped
**only when every key in it** is in the known-tracking set (`at_medium`,
`at_campaign`, any `at_*`, `utm_source`, `utm_medium`, `utm_campaign`,
`utm_term`, `utm_content`, `ref`, `fbclid`). If any other key is present the
query string is kept whole — never break a link that needs its parameters to
resolve.

---

## Component 7 — Recorder

**Modify** `little-cube-os/src/apps/RecorderApp.{h,cpp}`.

- Migrate to `widgets::header()` and the `TextStyle` faces.
- **Duration formatting.** `ready · ~16274 min left on card` overruns the line
  and is barely readable as a quantity. A helper renders the largest sensible
  unit: `~11 days left` at or above 48 h, `~4 h left` at or above 90 min,
  `~35 min left` below that.
- Row layout: the per-row delete button is positioned against
  `DISPLAY_WIDTH - kSafeInset` rather than a hardcoded offset, and the list item
  width is reduced to match, fixing the crowding.
- Footer routes through `widgets::footer()`: the `n-m of total` count on the
  left, `swipe up/down` on the right.

Behaviour that must not change: leaving the app does not stop an in-progress
recording; `Back`/`Home` while recording are refused with a message; the list
refreshes on the recording→idle edge rather than at the STOP tap; and no SD
access happens while the analog mic is live.

---

## Failure behaviour

| Condition | Result |
|---|---|
| No SD card | All six screens render. Recorder shows the specific `SdCardState`, never a generic error. |
| No Wi-Fi | Weather shows `offline - no cached weather yet`; Assistant surfaces its existing offline refusal. |
| Time not set | Every face renders `--:--` or `time not set` plus the recovery hint; all seven handle a null `tm`. |
| Weather cache from before restart | `cached (before restart)` in `kWarn`; never presented as current. |
| URL too long for QR | Page renders the reason plus the URL text. |
| Stored `clockFace` out of range | Clamps to `BigDigital`. |
| `batteryPercent < 0` (unknown) | Today's status card says unknown rather than showing a number. |

## Verification

There is no host-side test suite; this is on-device firmware and every change
here is visual. `docs/hardware-validation.md` gains a **Screen polish** section.
Per the standing rule, **no item may be checked without physically testing on the
cube**, and a clean compile is not validation.

The checklist must cover, at minimum:

- Each of the six clock faces renders, tap cycles them, and the choice survives a
  reboot.
- Animated faces blink; the screen still dims and blanks on the normal timeout.
- Weather tap cycles all three views; long-press and the Details button both
  refresh; per-day glyphs match the conditions.
- The News QR scans with a phone, and the opened page is the right article.
- The Recorder remaining-time line fits on one line and reads sensibly.
- Assistant: the meter moves while listening, a long answer pages, `Back`
  cancels.
- **Burn-in soak:** footers visibly drift over ~4 minutes on every one of the six
  screens. This is the regression check for the `shiftX/shiftY` fix, and it
  cannot be verified any other way.
- A flag-off / no-SD / no-Wi-Fi boot still renders all six screens.
