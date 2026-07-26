# AMOLED Protection

Spec §37 as implemented in `little-cube-os/src/ui/AmoledProtection.{h,cpp}`,
with its settings in `src/services/SettingsService.{h,cpp}`.

> **Verification status:** implemented and compiling, **not yet validated on
> hardware.** Timings, the wake-swallow behaviour and the visible extent of the
> pixel shift all need physical testing — see the AMOLED protection section of
> `docs/hardware-validation.md`. Nothing here may be treated as confirmed.

## Why it exists

The panel is a real AMOLED (SH8601, 368×448). There is no backlight —
"brightness" is a panel command, and 0 genuinely turns emission off. Organic
emitters age in proportion to how long each subpixel is driven and how hard,
so a bright, static, high-contrast element burns a permanent ghost into the
display. A bedside cube showing a fixed white clock is close to the worst case
the technology has.

Spec §37 therefore names the failure modes to avoid: a fixed white
max-brightness clock, permanently static icons, long-lived high-contrast
borders, unchanging nav bars. This module is the defense, and it is a hard
requirement, not a nicety.

## State machine

Three states, driven purely by an idle clock. Always-on, the bedtime window
and pixel shift are **modifiers layered on top**, not states of their own.

| State | Entered when | Panel |
|---|---|---|
| `Active` | any input, or idle < timeout | stored brightness (bedtime cap applied) |
| `Dim` | idle ≥ `screenTimeoutSec` | brightness ÷ 4, floor 8 |
| `Off` | idle ≥ `screenTimeoutSec` + 10 s, and `alwaysOn` is off | 0 (emission off) |

Timers and constants:

| Constant | Value | Role |
|---|---|---|
| `screenTimeoutSec` | setting, default 60 s | idle before `Dim` |
| `kDimToOffMs` | 10 000 ms | extra idle before `Off` |
| `kDimDivisor` / `kDimFloor` | 4 / 8 | how far `Dim` pulls brightness down, and its floor |
| `kShiftPeriodMs` | 60 000 ms | one pixel-shift step per minute |
| `kSwallowExpiryMs` | 1 000 ms | how long a wake stays armed to eat its own gesture |
| `kBedtimeCheckMs` | 30 000 ms | bedtime window re-evaluation interval |
| `kIdleCeilingMs` | 24 h | idle counter stops here so it cannot wrap |

The state is **recomputed every frame from the idle clock**, never edged, so
there is no path that leaves it stale after a settings change.

The 10 s dim step is deliberate: `Dim` is a visible warning that the screen is
about to go dark, and it also *is* the spec's "reduced brightness for
persistent content" — it stays lit enough to read a clock across a room.

## Ownership of brightness

`AmoledProtection` is the **sole caller of `DisplayAdapter::setBrightness()`
after boot.** Apps and the Settings UI change `SettingsService::setBrightness()`
and the panel follows within a frame. A second writer would fight this one over
every dim and blank transition and win at random.

The brightness baseline is always the *stored setting*, never
`display->brightness()` — reading back the panel would return whatever this
class last wrote, which is legitimately 0 while blanked, and would ratchet the
device permanently dark.

Order of application each frame:

1. `target = settings->brightness()` (NVS `bright`, clamped 16–255, default 220)
2. if bedtime is active: `target = min(target, bedtimeBrightness)` — bedtime only ever lowers
3. if `Off`: `target = 0`; else if `Dim`: `target = max(target / 4, 8)`
4. write only if the value changed (a `0xFF` sentinel forces the first write)

## Wake and input

`onActivity()` is called for every input event from the kernel loop and
**returns whether the event should reach the app**:

```cpp
if (amoledProtection.onActivity()) {
  appRouter.handleInput(event);
}
```

The verdict is returned rather than the sleep state being exposed for callers
to test, so there is no way to observe the latch without also clearing it —
that is what previously let wake taps leak through into whatever control
happened to sit under the finger.

Two wake paths:

- **From `Off`, via `onActivity()`** — the event has already been produced;
  it wakes the screen and is consumed. Returns `false`.
- **From `Off`, via the touch-down edge in `update()`** — `InputAdapter::touchDown()`
  is true before the gesture resolves. The screen comes back immediately and a
  *swallow* is armed, so the semantic event the release eventually produces is
  also discarded. The swallow only ages while the glass is clear (a long press
  still produces its event whenever it lands) and expires after 1 s so a wake
  with no follow-up cannot strand the flag.

A **finger resting on the glass** emits no semantic event between touch-down
and release, so `update()` resets the idle clock directly from `touchDown()`.
Without this the screen would blank under someone reading a long note.

While `Off`, `update()` keeps running — recordings, Wi-Fi and timers do not
pause because nobody is looking — but `render()` is skipped. Apps never clear
their `lastStateVersion_` while blanked, so the first frame after a wake is a
full redraw.

## Settings

| Setting | NVS key | Default | Range / notes |
|---|---|---|---|
| Brightness | `bright` | 220 | clamped 16–255 |
| Screen timeout | `scrTimeout` | 60 s | `0` = never; otherwise clamped 5–3600 s |
| Always-on | `alwaysOn` | off | prevents `Off`, **not** `Dim` |
| Bedtime enabled | `bedOn` | off | |
| Bedtime start | `bedStart` | 1320 (22:00) | minutes since midnight, < 1440 |
| Bedtime end | `bedEnd` | 420 (07:00) | minutes since midnight, < 1440 |
| Bedtime brightness | `bedBright` | 40 | clamped 16–255 |

All settings are typed, clamped on read *and* write, and written through
immediately — a value corrupted by a power cut or written by older firmware
cannot brick the device (spec §43).

### `screenTimeoutSec`

Seconds of idle before `Dim`. `Off` follows 10 s later. **`0` means "never"** —
the state is pinned to `Active` and the panel never dims or blanks. `0` is the
"never" step in the Settings UI, not a bad value, so it survives clamping;
every other value is clamped into 5–3600 s.

The Settings → Display screen steps through `15 · 30 · 60 · 120 · 300 · never`
and wraps at both ends.

### `alwaysOn`

Blocks the `Off` transition only. **Always-on still dims.** Spec §37 asks for
reduced brightness on persistent content; it only forbids going fully dark, so
the always-on device settles into `Dim` and stays there.

### Bedtime window

A nightly brightness *ceiling*, never a floor: it lowers `target` and can never
raise it. Times are minutes since midnight; the window is inclusive of start,
exclusive of end, and **flips its comparison when it wraps midnight** — which
the 22:00 → 07:00 default does.

```
start < end : start <= now < end
start > end : now >= start || now < end     (wraps midnight)
start == end: disabled (zero-length window)
```

The check reads the clock, so it runs on a 30 s timer rather than every frame.
It is primed to fire on the very first `update()`, so a device powered on at
2 a.m. comes up already dimmed instead of 30 s later.

If the clock is not valid the window is simply inactive. This is deliberate:
`TimeService::now()` falls back to a blocking I²C read of the RTC when the
clock is unvalidated, and that must never happen anywhere near the frame.

Only the on/off toggle is exposed in Settings → Display today; the window and
its brightness are shown as a read-only line (`22:00-07:00 at 40`) so the
toggle is never a mystery. Changing them requires firmware defaults — there is
no `settings set` serial verb yet.

## Pixel shift

A four-step rectangular walk of ±2 px, one step per minute:

```
(-2,-2) → (2,-2) → (2,2) → (-2,2) → ...
```

Fast enough that no glyph sits on one pixel long enough to matter; slow enough
that the drift is never visible in use. Every wake also advances the step for
free — the screen is being repainted from scratch anyway.

The ±2 px envelope was checked against the 368×448 status-bar layout: the
leftmost glyph starts at `kPadding` (12), the rightmost battery tip ends 10 px
short of the right edge, and the tallest status glyph reaches y=24 inside a
28 px bar. Two pixels in any direction clips nothing.

Apps early-out of `render()` unless `SystemState::version` moved, so the kernel
turns `consumeShiftChanged()` into a version bump and reuses that documented
dirty check rather than teaching all 12 apps a new `invalidate()` hook.

### What shifts

| Element | Shifts |
|---|---|
| Status bar (clock, battery, Wi-Fi, SD, alarm, recording glyphs) | yes — offset applied in `StatusBar::render()` |
| Clock app big digits `HH:MM` | yes |
| Clock app date line | yes |
| App body content | no |

The status bar is drawn with the shift by Home, Today, Clock, Weather, Notes,
Recorder, Audio and Settings. Calendar, Contacts, Calculator and Files do not
render a status bar at all today, so there is nothing persistent to shift in
them.

**Known gap:** the Clock app's "time not set" warning line is drawn *without*
the shift offset. It only appears while the clock is invalid, but it is a
static high-contrast string for as long as that lasts.

App body content is not shifted. The reasoning is that only genuinely
persistent chrome burns in — but this has not been validated against a
long-running app such as Clock or Today, and spec §37 also asks for alternate
clock layouts, which are **not implemented**.

### Why the clock is dim, not white

Both the status-bar clock and the Clock app's big digits render in
`theme::kTextDim` (`0xBDF7`, roughly 72% grey) rather than `theme::kText`
(`0xFFFF`, pure white) on a true-black background.

Spec §37 names "a fixed white max-brightness clock" as exactly the thing that
burns a panel in, and the big digits are the largest-area, most persistent
element the device ever shows. Dropping them to dim costs nothing legible on
an emissive panel in a dark room and removes the worst burn-in load.

One consequence: the Clock app no longer distinguishes valid from invalid time
by brightness. That is fine — an unset clock is called out explicitly by the
warning line beneath it. The status bar still does use full white for a valid
clock and dim for an unvalidated one, since that glyph is small.

## Spec §37 coverage

| §37 requirement | State |
|---|---|
| Screen timeout | implemented (`screenTimeoutSec`, `Dim` → `Off`) |
| Automatic dimming | implemented (`Dim` at timeout) |
| Bedside low-brightness mode | implemented (bedtime window, on/off in Settings) |
| Pixel shifting at safe intervals | implemented (±2 px, 60 s) |
| Periodic movement of static elements | implemented for status bar + clock digits |
| Configurable always-on | implemented (Settings → Display) |
| Automatic screen-off | implemented (`Off`, suppressed by always-on) |
| Reduced brightness for persistent content | implemented (dim colours + `Dim` state) |
| Alternate clock layouts | **not implemented** |
| Orientation / pixel-shift toggles in Settings (§28) | **not implemented** — pixel shift is always on |
