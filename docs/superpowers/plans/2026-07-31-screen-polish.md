# Screen Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring Clock, Today, Weather, News, Recorder and Assistant up to the type system the rest of the device already uses, fix the footers that clip in the panel's rounded corners, and give the Clock six selectable faces including three animated character faces.

**Architecture:** Three shared pieces land first — a `widgets::footer()` helper, weather glyphs in `icons::`, and a QR wrapper isolating a new library — then each screen is migrated independently against them. Clock faces live in their own file so `ClockApp` keeps only selection, persistence and the animation clock.

**Tech Stack:** Arduino CLI, ESP32-S3, Arduino_GFX immediate-mode canvas, Adafruit_GFX Free* fonts, and the ESP-IDF QR encoder already bundled with the Arduino core. **No new libraries.**

**Spec:** `docs/superpowers/specs/2026-07-31-screen-polish-design.md`

---

## ⚠️ Geometry in this plan was written by eye — verify it

Task 2's review measured all seven new weather glyphs against the
`[x, x+size) x [y, y+size)` box contract and found **six of seven overflowed**,
by up to 11 px. The thirteen pre-existing glyphs were run through the same
model as a control and all fit, so the contract is real and the plan's code was
simply wrong. The same applies to every drawing block below — the clock faces
in Tasks 5 and 6 contain considerably more geometry than Task 2 did.

Two facts that caused most of those bugs, worth knowing before writing any
drawing code here:

- **`fillCircle` spans `2r+1` pixels**, `[c-r, c+r]` inclusive — not `2r`.
  Confirmed in the vendored `GFX_Library_for_Arduino` source. Every "off by one
  on the right edge" bug in Task 2 came from assuming otherwise.
- **`Arduino_GFX` clips at the canvas edge**, so an overflow can never corrupt
  the framebuffer or crash. It just collides with adjacent UI. This is why a
  clean compile *and* a clean boot both tell you nothing about it.

Do the algebra before committing drawing code, at every size the glyph is
actually used at (`t = max(2, size/16)` changes with size, so a shape that fits
at 56 can overflow at 96). A throwaway script that models the primitives'
extents settles it in a second and is cheaper than a flash cycle.

**But a bounds model is not enough.** Task 2's second review found a defect no
bounding-box check can see: thickening a line by drawing offset copies works
for axis-aligned lines and *fails* for 45° ones, because successive copies of a
diagonal Bresenham line offset by `(±1, ±1)` touch only at corners. Four of the
sun's eight rays rasterised as dotted chains while the box stayed correct.

To catch that class you have to **rasterise**, not measure: compile the drawing
code against a stub whose primitive bodies are taken verbatim from the vendored
`GFX_Library_for_Arduino` source, and print the result as ASCII. Anything built
from `drawLine` at a non-axis angle, or from repeated offset strokes, needs
this. `fillTriangle` fills solid at any orientation and is the reliable way to
draw a thick angled bar.

## ⚠️ `render()` is NOT called once per screen entry

Every app in this codebase early-outs with
`if (!dirty_ && state.version == lastStateVersion_) return;`, which reads like
"render only when something changed". It is easy to conclude from that a static
screen renders once and then stops. **It does not.**

`SystemState::version` moves on a timer, whether or not anything the user cares
about changed:

- `Kernel.cpp:497-499` — `amoledProtection.consumeShiftChanged()` bumps it every
  **60 s** (`kShiftPeriodMs`), because the burn-in offsets moved.
- `Kernel.cpp:141-146` — the `clockHhMm` string changes, so it bumps again every
  **minute**.
- Plus battery %, charging, Wi-Fi state, SD state, and service events.

So a screen left open re-renders roughly **once or twice a minute, forever**.
Anything expensive placed in `render()` becomes a recurring stall on that
cadence, not a one-time cost paid on entry.

This was found in Task 3's review, where the QR encoder — 8-35 ms of
Reed-Solomon and mask evaluation against a ~33 ms frame budget — was documented
as running "once per page entry" on exactly this mistaken reasoning. The fix is
to cache the expensive result and let `render()` do only the cheap repaint.

Applies to every task below: **`render()` may draw, and nothing else.** Layout,
encoding, pagination and any I/O belong in `onOpen()`, `onResume()` or
`update(deltaMs)`.

## How to verify in this repo

**There is no unit-test suite and no host-side harness.** `CLAUDE.md` is explicit:
this is on-device firmware, verification is `docs/hardware-validation.md`, and
**no item may be checked without physically testing on the cube.** A clean
compile is not validation.

So every task below ends with two gates, and they are not interchangeable:

**Compile gate** (the engineer runs this):

```bash
./scripts/build.sh
```

Expected: ends with `Sketch uses ... bytes` and no new warnings. `build.sh`
passes `--warnings all`; a new warning in a file you touched is a failure.

**Device gate** (recorded, not run blind): each task names what must be checked
on hardware. Do **not** tick anything in `docs/hardware-validation.md` from a
compile. Task 12 adds the checklist section; the items get ticked in a later
session with the cube in hand.

Prerequisites, once per shell:

```bash
export LITTLECUBE_FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=custom"
export LITTLECUBE_PORT="/dev/cu.usbmodem101"
```

---

## File structure

| File | Responsibility | Task |
|---|---|---|
| `little-cube-os/src/ui/widgets/Widgets.{h,cpp}` | **Modify** — add `footer()` | 1 |
| `little-cube-os/src/ui/Icons.{h,cpp}` | **Modify** — 7 weather glyphs + `forCondition()` | 2 |
| `little-cube-os/src/ui/QrCode.{h,cpp}` | **Create** — sole include site for the core's bundled QR encoder | 3 |
| `little-cube-os/src/services/SettingsService.{h,cpp}` | **Modify** — persisted `clockFace` | 4 |
| `little-cube-os/src/ui/ClockFaces.{h,cpp}` | **Create** — `FaceId`+`kFaceCount` in Task 4 (shared with SettingsService, like `ui/Theme.h`); six renderers in 5-6 | 4, 5, 6 |
| `little-cube-os/src/apps/ClockApp.{h,cpp}` | **Modify** — selection, persistence, animation clock | 5, 6 |
| `little-cube-os/src/apps/WeatherApp.{h,cpp}` | **Modify** — three views, glyphs, refresh reassignment | 7 |
| `little-cube-os/src/apps/TodayApp.{h,cpp}` | **Modify** — cards, glyph, battery row, delete dead rows | 8 |
| `little-cube-os/src/apps/AssistantApp.{h,cpp}` | **Modify** — bubbles, level meter, answer paging | 9 |
| `little-cube-os/src/apps/NewsApp.{h,cpp}` | **Modify** — both footers, QR page, tracking strip | 10 |
| `little-cube-os/src/apps/RecorderApp.{h,cpp}` | **Modify** — type system, duration format, row layout | 11 |
| `docs/hardware-validation.md`, `technical.md` | **Modify** — new checklist section | 12 |

Arduino CLI compiles the sketch root and `src/**` recursively, so no build file
needs to learn about the new `.cpp` files. It **does** need to learn about the
new library — see Task 3.

---

## Task 1: `widgets::footer()`

**Why first:** six screens depend on it, and it is the fix for both the reported
cut-off and the standing burn-in gap.

**Files:**
- Modify: `little-cube-os/src/ui/widgets/Widgets.h` (declaration beside `header()`, ~line 59)
- Modify: `little-cube-os/src/ui/widgets/Widgets.cpp` (definition after `header()`, ~line 143)

- [ ] **Step 1: Declare it in `Widgets.h`, directly below `header()`**

```cpp
// Shared bottom band, the mirror of header(): a hairline rule with left- and
// optionally right-aligned caption text, inset by kSafeInset so nothing lands
// in the bezel's corner radius. `right` may be nullptr.
//
// TWO BUGS THIS EXISTS TO KILL, both of which every hand-placed footer had:
//   1. x = kPadding (12) sits INSIDE the rounded corner and gets clipped.
//   2. Footers are persistent chrome and must drift with the burn-in offsets
//      (spec §37) — not one of them passed shiftX/shiftY.
// Returns the y of the rule, i.e. the bottom of the caller's content budget.
int16_t footer(Arduino_GFX& gfx, const char* left, const char* right, int16_t shiftX,
               int16_t shiftY);
```

- [ ] **Step 2: Define it in `Widgets.cpp`, immediately after `header()`**

```cpp
int16_t footer(Arduino_GFX& gfx, const char* left, const char* right, int16_t shiftX,
               int16_t shiftY) {
  // Rule at -44 rather than the -28 the old call sites used: caption ink then
  // ends ~19 px above the bottom edge. That is the working assumption for
  // clearing the corner radius the old -28 footers visibly clipped against —
  // kSafeInset = 20 is asserted in prose at Theme.h:61, not a measured
  // hardware constant, so treat this as pending on-device confirmation.
  //
  // shiftX/shiftY (burn-in drift, spec §37) apply on top of both insets below
  // rather than being clamped — clamping would defeat the drift. In the worst
  // quadrant (shiftX = -2, shiftY = +2) the effective inset is 18px/17px, not
  // the nominal 20/19 the comments reason about.
  const int16_t ruleY = DISPLAY_HEIGHT - 44 + shiftY;
  // The rule aligns with header()'s rule (kPadding .. width - kPadding) so the
  // two hairlines share an inset. The caption TEXT sits further in, at
  // kSafeInset, because it lives only 19px off the bottom edge — inside the
  // corner-radius zone the rule itself, 44px up, does not reach.
  const int16_t ruleLeftX = theme::kPadding + shiftX;
  const int16_t ruleRightX = DISPLAY_WIDTH - theme::kPadding + shiftX;
  const int16_t textLeftX = theme::kSafeInset + shiftX;
  const int16_t textRightX = DISPLAY_WIDTH - theme::kSafeInset + shiftX;
  gfx.drawFastHLine(ruleLeftX, ruleY, ruleRightX - ruleLeftX, theme::kPanelAlt);

  const int16_t textTop = ruleY + 8;
  int16_t leftW = 0;
  const bool hasLeft = left != nullptr && left[0] != '\0';
  if (hasLeft) {
    leftW = textWidth(gfx, left, TextStyle::Caption);
    text(gfx, textLeftX, textTop, left, TextStyle::Caption, theme::kTextDim);
  }
  if (right != nullptr && right[0] != '\0') {
    const int16_t rightW = textWidth(gfx, right, TextStyle::Caption);
    // Drop the hint rather than let it collide: a half-drawn hint reads as a
    // rendering fault, an absent one reads as nothing at all. The 12px
    // separation gap only applies when there is a left string to collide
    // with — a right-only hint should use the full band.
    const int16_t leftEdge = hasLeft ? textLeftX + leftW + 12 : textLeftX;
    if (leftEdge <= textRightX - rightW) {
      textRight(gfx, textRightX, textTop, right, TextStyle::Caption, theme::kTextDim);
    }
  }
  return ruleY - 12;
}
```

⚠️ **`theme::kPanelAlt` is a FILL color, never an ink color.** `Theme.h:17`
documents it as "secondary fill, dividers", and its measured contrast against
`kBg` is 1.24–1.40:1 across all ten palettes — text drawn in it is invisible on
every theme. The existing `ClockApp.cpp:86` footer does exactly this, and its
`"alarms & timers: coming soon"` string does not appear in device photographs
at all. Use `kTextDim` for quiet text. This applies to every task below.

⚠️ **`footer()` returns the bottom of the content budget, already padded 12 px
clear of the rule** — the mirror of `header()`, which returns a padded
content-start. Screens lay content out *down to* the returned value; they must
not subtract their own extra margin on top of it.

⚠️ **`right` wins the space; `left` yields.** `right` carries action hints the
user needs to operate the screen, so it is drawn whenever it fits the band at
all. `left` carries status text (filenames, `"3-8 of 24"`) and is ellipsized
into whatever space remains. Callers in Tasks 7-11 should therefore put the
*affordance* on the right and the *status* on the left — putting a long
filename on the right will get it dropped, not shortened.

**Note on the compile gate for Tasks 1-4:** the ESP32 linker garbage-collects
unreferenced functions, so `footer()`, `fitWithEllipsis()`, `icons::` weather
glyphs and `qrcode::draw()` contribute **zero** flash until a screen calls
them. Identical binary sizes across those commits are expected and prove
nothing about size. The first real size signal arrives in Task 5.

- [ ] **Step 3: Compile gate**

Run: `./scripts/build.sh`
Expected: clean, no new warnings. Nothing calls `footer()` yet, so flash size
moves only by the new function's own size.

- [ ] **Step 4: Commit**

```bash
git add little-cube-os/src/ui/widgets/Widgets.h little-cube-os/src/ui/widgets/Widgets.cpp
git commit -m "UI: shared widgets::footer() with safe inset and burn-in shift"
```

**Device gate:** none yet — no caller. Verified via Tasks 7-11.

---

## Task 2: Weather glyphs in `icons::`

**Files:**
- Modify: `little-cube-os/src/ui/Icons.h` (enum + new function)
- Modify: `little-cube-os/src/ui/Icons.cpp` (helpers + switch cases + `forCondition`)

- [ ] **Step 1: Extend the enum and declare the mapper in `Icons.h`**

Add to `IconId`, after `Tools`:

```cpp
  WxClear,
  WxPartlyCloudy,
  WxCloudy,
  WxRain,
  WxSnow,
  WxThunder,
  WxFog,
```

Add below `draw()`:

```cpp
// Maps a WeatherSnapshot condition string to its glyph.
//
// The producing table (conditionFromWmo() in the weather service) emits a
// CLOSED set of 11 strings, so this is exact matching, not substring guessing.
// A cache file edited by hand can still hold anything, hence the fallback.
IconId forCondition(const char* condition);
```

Also update the file's opening comment — it currently claims these are
"Home-card glyphs", which stops being the whole truth:

```cpp
// App and weather glyphs, drawn entirely from Arduino_GFX primitives — no
// bitmap assets, no extra library, and every glyph takes the active theme's
// colors so a palette switch repaints them for free.
```

- [ ] **Step 2: Add shared drawing helpers to the anonymous namespace in `Icons.cpp`**

Place these after the existing `strokeRect()`:

```cpp
// cos/sin scaled by 128 for the 8 compass directions. A table keeps the sun's
// rays symmetric and integer-only — no float trig in a render path.
constexpr int8_t kRayCos[8] = {127, 90, 0, -90, -127, -90, 0, 90};
constexpr int8_t kRaySin[8] = {0, 90, 127, 90, 0, -90, -127, -90};

void sunDisc(Arduino_GFX& gfx, int16_t cx, int16_t cy, int16_t r, int16_t t,
             uint16_t color) {
  gfx.fillCircle(cx, cy, r, color);
  const int16_t inner = r + t;
  const int16_t outer = r + t * 3;
  for (int i = 0; i < 8; i++) {
    const int16_t x0 = cx + static_cast<int16_t>(inner * kRayCos[i] / 128);
    const int16_t y0 = cy + static_cast<int16_t>(inner * kRaySin[i] / 128);
    const int16_t x1 = cx + static_cast<int16_t>(outer * kRayCos[i] / 128);
    const int16_t y1 = cy + static_cast<int16_t>(outer * kRaySin[i] / 128);
    // Each ray is a filled quad, not stacked drawLine strokes. Offsetting a
    // stroke to thicken it only works on axis-aligned lines: two copies of a
    // 45° Bresenham line one pixel apart touch at corners only, so the four
    // diagonal rays came out as dotted chains while the axis rays were solid.
    // fillTriangle fills solid at any orientation.
    //
    // The rendered band is 2*floor(127t/256) + 1 px, not t: t for odd t,
    // t-1 for even. Rounding half-up overshoots to t+1, which is worse.
    // Diagonals also render 15-25% heavier than the axis rays; stepping the
    // diagonal offset down one integer makes them ~24% too thin instead. Both
    // errors are symmetric and not fixable without subpixel coverage.
    int16_t hx = static_cast<int16_t>(-kRaySin[i] * t / 256);
    int16_t hy = static_cast<int16_t>(kRayCos[i] * t / 256);
    if (hx == 0 && hy == 0) {
      // t == 2 (sizes 21-47): both offsets truncate to 0 and the quad
      // collapses to a zero-area line. Force a +-1 perpendicular step so
      // the ray stays a filled shape instead of vanishing.
      hx = static_cast<int16_t>(kRaySin[i] > 0 ? -1 : (kRaySin[i] < 0 ? 1 : 0));
      hy = static_cast<int16_t>(kRayCos[i] > 0 ? 1 : (kRayCos[i] < 0 ? -1 : 0));
    }
    gfx.fillTriangle(x0 + hx, y0 + hy, x0 - hx, y0 - hy, x1 - hx, y1 - hy, color);
    gfx.fillTriangle(x0 + hx, y0 + hy, x1 + hx, y1 + hy, x1 - hx, y1 - hy, color);
  }
}

// Three overlapping circles on a flat base. Shared by the cloudy, rain, snow
// and thunder glyphs so they read as one family.
void cloudPuff(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t w, uint16_t color) {
  const int16_t r = w / 4;
  const int16_t baseY = y + r + r / 2;
  // Crown radius is r + r/4, not the more dramatic r + r/3 it might look
  // like it wants: centring the crown so its top edge lands exactly on y
  // (required to stop it overflowing above the box) pushes its bottom edge
  // down too, and r + r/3 pushes that bottom edge past the box in the
  // WxPartlyCloudy call, which starts this helper higher inside its box
  // than the other five callers do.
  const int16_t crownR = r + r / 4;
  gfx.fillCircle(x + r, baseY, r, color);
  gfx.fillCircle(x + w - r - 1, baseY, r, color);  // w - r would land one past the right edge
  gfx.fillCircle(x + w / 2, y + crownR, crownR, color);
  // Height r + 1, not r: the shoulder circles reach baseY + r, so a rect of
  // height r leaves the cloud's bottom row as two narrow nubs with a gap
  // between them.
  gfx.fillRect(x + r, baseY, w - 2 * r, r + 1, color);
}
```

⚠️ **This geometry is the reviewed and bounds-verified version.** The first
draft of it overflowed the box on six of seven glyphs. Three separate causes,
all worth knowing before writing similar code: `fillCircle` spans `2r+1`;
moving a circle's centre moves *both* its edges, so "anchor the top edge at
`y`" also pushes the bottom down; and a stagger anchored from the top grows
past the box as `t` scales with size, while one anchored from the bottom
cannot.

- [ ] **Step 3: Add the seven cases to the `switch (id)` in `draw()`**

Insert before the closing brace of the switch:

```cpp
    case IconId::WxClear: {
      // Measured floor: fits at every size >= 21. The naive bound q + 3t <=
      // size/2 says 28, but overstates the reach — kRayCos/kRaySin are scaled
      // by 128 rather than 127, so each ray truncates about a pixel short.
      sunDisc(gfx, cx, cy, q, t, color);
      break;
    }
    case IconId::WxPartlyCloudy: {
      // sunDisc reaches r + 3t from its centre; with r = q - t that's q + 2t,
      // so the centre needs to sit at least q + 2t from the top-left corner.
      // Moved rather than shrunk, or the disc reads as a dot at small sizes.
      sunDisc(gfx, x + q + t * 2, y + q + t * 2, q - t, t, color);
      // cy - q/4 fits with zero bottom margin at 56/64/96 (luck, not a
      // guarantee) and does overflow at other sizes in this app's range, so
      // clamp against the same shoulder-circle-bottom bound cloudPuff itself
      // has to satisfy. At our three sizes the clamp is a no-op.
      const int16_t cloudW = size - q / 2;
      const int16_t cloudR = cloudW / 4;
      const int16_t cloudYMax = y + size - 1 - (2 * cloudR + cloudR / 2);
      const int16_t cloudY = (cy - q / 4 < cloudYMax) ? (cy - q / 4) : cloudYMax;
      cloudPuff(gfx, x + q / 2, cloudY, cloudW, color);
      break;
    }
    case IconId::WxCloudy: {
      cloudPuff(gfx, x, y + q / 2, size, color);
      break;
    }
    case IconId::WxRain: {
      cloudPuff(gfx, x, y, size, color);
      // Three slanted drops under the cloud.
      for (int i = 0; i < 3; i++) {
        const int16_t dx = x + q - t + static_cast<int16_t>(i) * q;
        for (int16_t o = 0; o < t; o++) {
          gfx.drawLine(dx + o, y + size - q, dx - q / 3 + o, y + size - 1, color);
        }
      }
      break;
    }
    case IconId::WxSnow: {
      cloudPuff(gfx, x, y, size, color);
      for (int i = 0; i < 3; i++) {
        const int16_t dx = x + q - t + static_cast<int16_t>(i) * q;
        gfx.fillCircle(dx, y + size - q / 2, t, color);
      }
      break;
    }
    case IconId::WxThunder: {
      cloudPuff(gfx, x, y, size, color);
      // Bolt: two triangles sharing the waist, so it reads at small sizes.
      const int16_t bx = cx;
      const int16_t by = y + size - q * 2;
      gfx.fillTriangle(bx + q / 2, by, bx - q / 2, by + q, bx + t, by + q, color);
      gfx.fillTriangle(bx + q / 2, by + q, bx - q / 3, y + size - 1, bx - t, by + q, color);
      break;
    }
    case IconId::WxFog: {
      cloudPuff(gfx, x, y, size, color);
      // Three staggered bars: fog is the cloud, sitting on the ground.
      // Anchored from the bottom so the stagger can't run past the box —
      // anchoring from the top (y + size - q) did, growing with size.
      for (int i = 0; i < 3; i++) {
        const int16_t by = y + size - t - static_cast<int16_t>(2 - i) * (t * 2);
        const int16_t inset = static_cast<int16_t>(i) * q / 2;
        gfx.fillRect(x + inset, by, size - inset * 2, t, color);
      }
      break;
    }
```

- [ ] **Step 4: Add `forCondition()` at the bottom of `Icons.cpp`, inside `namespace icons`**

```cpp
IconId forCondition(const char* condition) {
  if (condition == nullptr) {
    return IconId::WxCloudy;
  }
  struct Entry {
    const char* name;
    IconId id;
  };
  // Exact matches against conditionFromWmo()'s closed output set. Matching is
  // exact, so "Snow" and "Snow showers" cannot shadow each other and the order
  // of this table carries no meaning.
  static const Entry kTable[] = {
      {"Clear", IconId::WxClear},
      {"Mostly clear", IconId::WxPartlyCloudy},
      {"Overcast", IconId::WxCloudy},
      {"Fog", IconId::WxFog},
      {"Drizzle", IconId::WxRain},
      {"Rain", IconId::WxRain},
      {"Showers", IconId::WxRain},
      {"Snow", IconId::WxSnow},
      {"Snow showers", IconId::WxSnow},
      {"Thunderstorm", IconId::WxThunder},
  };
  for (const Entry& e : kTable) {
    if (strcmp(condition, e.name) == 0) {
      return e.id;
    }
  }
  // The service's own "Weather" unknown, or a hand-edited cache file.
  return IconId::WxCloudy;
}
```

`Icons.cpp` needs `#include <string.h>` for `strcmp` if it is not already
pulled in by `Arduino_GFX_Library.h`; add it explicitly rather than relying on
a transitive include.

- [ ] **Step 5: Compile gate**

Run: `./scripts/build.sh`
Expected: clean. A `-Wswitch` warning here means an `IconId` case was missed —
fix it rather than adding a `default:`, which would hide the next one.

- [ ] **Step 6: Commit**

```bash
git add little-cube-os/src/ui/Icons.h little-cube-os/src/ui/Icons.cpp
git commit -m "UI: weather condition glyphs and an exact-match condition mapper"
```

**Device gate:** glyphs are verified in Task 7 (Weather) and Task 8 (Today).

---

## Task 3: QR rendering on the core's bundled encoder

**Files:**
- Create: `little-cube-os/src/ui/QrCode.h`
- Create: `little-cube-os/src/ui/QrCode.cpp`

⚠️ **This task originally called for adding `ricmoo/QRCode` and registering it
in `scripts/build.sh`, `scripts/install-libraries.sh` and
`little-cube-os/sketch.yaml`. Do none of that.** The attempt failed to compile:
the ESP32 Arduino core already ships `espressif__qrcode`, and its `qrcode.h`
sits ahead of any library on the include path
(`tools/esp32s3-libs/3.3.8/include/espressif__qrcode/include/qrcode.h`).

The collision was the platform pointing out the dependency was redundant.
`libespressif__qrcode.a` is already in the default `ld_libs`, so this links
with **no build-system changes at all**, and the bundled encoder supports QR
versions 2-40 against ricmoo's 8.

- [ ] **Step 1: Read the real header before writing anything**

```bash
cat ~/Library/Arduino15/packages/esp32/tools/esp32s3-libs/3.3.8/include/espressif__qrcode/include/qrcode.h
```

The API:

```c
esp_err_t esp_qrcode_generate(esp_qrcode_config_t *cfg, const char *text);
int  esp_qrcode_get_size(esp_qrcode_handle_t qrcode);                 // side in modules
bool esp_qrcode_get_module(esp_qrcode_handle_t qrcode, int x, int y); // true = black

typedef struct {
    void (*display_func)(esp_qrcode_handle_t qrcode);
    int max_qrcode_version;   // 2-40
    int qrcode_ecc_level;     // ESP_QRCODE_ECC_LOW / MED / QUART / HIGH
} esp_qrcode_config_t;
```

Returns `ESP_OK`, `ESP_FAIL`, or `ESP_ERR_NO_MEM`.

- [ ] **Step 2: Create `little-cube-os/src/ui/QrCode.h`**

```cpp
#pragma once

#include <Arduino.h>

class Arduino_GFX;

// QR rendering, wrapping the ESP-IDF encoder bundled with the Arduino core
// (espressif__qrcode, already in the default ld_libs). This header and its
// .cpp are the ONLY place that encoder is included. Using it rather than a
// third-party library means no added dependency and no three-site
// registration in build.sh / install-libraries.sh / sketch.yaml.
namespace qrcode {

// Draws a QR for `text` inside a maxSizePx square at (x, y), picking the
// largest module size that fits including the mandatory 4-module quiet zone.
//
// Returns false when the text does not encode, when maxSizePx cannot afford
// the minimum module size, or when text is empty. The caller must draw its own
// fallback — a truncated or undersized QR is not a degraded QR, it is an
// unscannable one.
//
// `bg` must be a LIGHT color. QR scanners require dark modules on a light
// field; inheriting theme::kBg on a dark palette produces a code no phone will
// read, so the caller passes the quiet-zone color explicitly.
//
// NOT REENTRANT — see the .cpp. Safe only from the loop task.
bool draw(Arduino_GFX& gfx, int16_t x, int16_t y, int16_t maxSizePx, const char* text,
          uint16_t fg, uint16_t bg);

}  // namespace qrcode
```

- [ ] **Step 3: Create `little-cube-os/src/ui/QrCode.cpp`**

Two constraints the API imposes, both of which must be documented in the source
rather than left for the next reader to discover:

1. **`esp_qrcode_generate()` is callback-based with no user-data parameter.**
   `display_func` receives only the handle, so the render target and geometry
   must reach it through file scope. That makes `draw()` non-reentrant. It is
   safe here only because all rendering runs on the single loop task — say so
   in the comment; it is a real constraint, not a shortcut.
2. **It allocates** (`ESP_ERR_NO_MEM` is a documented return). Cap
   `max_qrcode_version` at 10 to bound it — version 10 at ECC_LOW holds ~271
   alphanumeric characters, far more than any article URL. The allocation is
   per page-entry, not per-frame: `NewsApp::render()` returns early when the
   frame is not dirty, so a static QR page encodes exactly once.

Also required: `ESP_QRCODE_ECC_LOW` (maximises payload per module, correct for
a clean backlit surface rather than a printed label); the 4-module quiet zone
counted in the fit so the border is never sacrificed; `bg` filled behind the
whole code including the quiet zone; and a **2px minimum module size**, not
1px — a 1px-module QR will not scan off this panel, so returning true for one
would be the function lying about success.

See the committed `little-cube-os/src/ui/QrCode.cpp` for the full
implementation.

- [ ] **Step 4: Compile gate — BOTH paths**

```bash
./scripts/build.sh
```

```bash
arduino-cli compile --profile littlecube little-cube-os
```

Both must be clean.

⚠️ **If the link fails with `objs.a(...) in archive is not an object`, that is
a corrupted build cache, not your code** — it happens when two `arduino-cli`
processes write the same sketch cache concurrently. Clear it and rebuild:

```bash
rm -rf ~/Library/Caches/arduino/sketches/*
```

- [ ] **Step 5: Commit**

```bash
git add little-cube-os/src/ui/QrCode.h little-cube-os/src/ui/QrCode.cpp
git commit -m "UI: QR rendering on the core's bundled encoder, no new dependency"
```

**Device gate:** verified in Task 10 by scanning the News QR with a phone.

---


## Task 4: Persisted clock-face setting

> ⚠️ **The steps below are SUPERSEDED and kept only as a record.** They
> describe a duplicated `kClockFaceCount` in `SettingsService.cpp`'s anonymous
> namespace guarded by a `static_assert`. Review rejected both: the cloned
> `themeIndex` template does not duplicate anything (it includes `ui/Theme.h`
> and reads `theme::kThemeCount` directly), and the assert compared a literal
> against itself, catching one drift mode of three while missing the
> out-of-bounds one.
>
> **What actually shipped:** `FaceId` and `kFaceCount` live in
> `little-cube-os/src/ui/ClockFaces.h`; `SettingsService.cpp` includes it
> beside `ui/Theme.h`; there is no duplicated constant and no assert. Both
> setters also gained an unchanged-value early return (a per-frame caller
> would otherwise exhaust the NVS partition in ~15 days), and `clockface` is
> wired into the `settings` serial family so Task 5's persistence gate is
> assertable over serial rather than only visually.

**Files:**
- Modify: `little-cube-os/src/services/SettingsService.h`
- Modify: `little-cube-os/src/services/SettingsService.cpp`

This clones the existing `themeIndex()` pattern exactly — same shape, same
clamp-on-load, same NVS namespace.

- [ ] **Step 1: Add the accessor pair to `SettingsService.h`**

Beside `themeIndex()`:

```cpp
  // Selected clock face (0..clockfaces::kFaceCount-1); see ui/ClockFaces.h.
  // Stored as a plain uint8 rather than the enum so this header does not have
  // to depend on an app header.
  uint8_t clockFace() const { return clockFace_; }
  void setClockFace(uint8_t value);
```

And the member, beside `themeIndex_`:

```cpp
  uint8_t clockFace_ = 0;
```

- [ ] **Step 2: Add the NVS key in `SettingsService.cpp`**

Beside `kKeyTheme` (note the existing comment: NVS keys are capped at 15 chars —
`clockface` is 9):

```cpp
constexpr const char* kKeyClockFace = "clockface";
```

- [ ] **Step 3: Load it in `load()`, beside the `themeIndex_` load**

```cpp
  clockFace_ = prefs.getUChar(kKeyClockFace, 0);
  if (clockFace_ >= kClockFaceCount) {
    clockFace_ = 0;
  }
```

`kClockFaceCount` is declared in the anonymous namespace at the top of
`SettingsService.cpp`, so the service does not include an app header:

```cpp
// Mirrors clockfaces::kFaceCount (ui/ClockFaces.h). Duplicated rather than
// included so a service does not depend on an app; the static_assert in
// ClockFaces.h is what keeps the two honest.
constexpr uint8_t kClockFaceCount = 6;
```

- [ ] **Step 4: Add the setter, beside `setThemeIndex()`**

```cpp
void SettingsService::setClockFace(uint8_t value) {
  clockFace_ = value < kClockFaceCount ? value : 0;
  prefs.putUChar(kKeyClockFace, clockFace_);
}
```

Clamping to 0 rather than to the maximum matters: a firmware downgrade that
drops faces must land on the default face, not on whatever now sits at the top
of the enum.

- [ ] **Step 5: Compile gate**

Run: `./scripts/build.sh`
Expected: clean.

- [ ] **Step 6: Commit**

```bash
git add little-cube-os/src/services/SettingsService.h little-cube-os/src/services/SettingsService.cpp
git commit -m "Settings: persist the selected clock face"
```

**Device gate:** persistence is verified in Task 5 (switch face, reboot, face survives).

---

## Task 5: Clock faces — structure and the three static faces

**Files:**
- Modify: `little-cube-os/src/ui/ClockFaces.h` (created in Task 4 with the enum + count)
- Create: `little-cube-os/src/ui/ClockFaces.cpp`
- Modify: `little-cube-os/src/apps/ClockApp.h`
- Modify: `little-cube-os/src/apps/ClockApp.cpp`

⚠️ **`ClockFaces.h` lives in `ui/`, not `apps/`, and there is no duplicated
count.** An earlier draft of this plan put the header under `apps/` and had
`SettingsService.cpp` keep its own `kClockFaceCount = 6`, guarded by a
`static_assert`. Task 4's review killed both ideas:

- The template being cloned **does not duplicate anything**.
  `SettingsService.cpp` includes `../ui/Theme.h` and reads `theme::kThemeCount`
  directly. The layering problem is solved by putting the constant where both
  layers can see it, not by copying it.
- `static_assert(kFaceCount == 6, ...)` compares a literal against itself. It
  cannot see a constant in an anonymous namespace in another translation unit,
  so it catches one drift mode of three — and misses the dangerous one, where
  settings is bumped to 7 against six faces and dispatches out of bounds.

Task 4 therefore already created `little-cube-os/src/ui/ClockFaces.h` holding
`FaceId` and `kFaceCount`. **This task extends that file; it does not create
it, and nothing goes under `apps/ClockFaces.h` — that path does not exist and
must not be created.**

- [ ] **Step 1: Extend `little-cube-os/src/ui/ClockFaces.h`**

```cpp
#pragma once

#include <Arduino.h>
#include <time.h>

class Arduino_GFX;
struct SystemState;

// Clock faces: pure renderers with no app state. ClockApp owns selection,
// persistence and the animation clock; this file owns pixels only.
namespace clockfaces {

enum class FaceId : uint8_t {
  BigDigital,  // default
  Stacked,
  Word,
  Blinky,
  BigEyes,
  MoodCube,
};
constexpr uint8_t kFaceCount = 6;

// SettingsService reads kFaceCount directly from this header — the same way
// it reads theme::kThemeCount from ui/Theme.h — so there is no duplicated
// constant to keep in sync.

const char* name(FaceId id);

struct FaceContext {
  const struct tm* time = nullptr;   // nullptr when the clock is not set
  const SystemState* state = nullptr;  // battery/charging, for MoodCube
  uint32_t animMs = 0;               // monotonic ms since the app opened
  int16_t shiftX = 0;
  int16_t shiftY = 0;
};

// Draws the face. Returns true when an animation is in flight and the face
// wants another frame soon; false when it is static until the minute rolls.
//
// This return value is load-bearing: it is what keeps an animated face from
// pinning the frame loop and the panel at full tilt. ClockApp must honour it
// rather than redrawing unconditionally.
bool render(Arduino_GFX& gfx, FaceId id, const FaceContext& ctx);

}  // namespace clockfaces
```

- [ ] **Step 2: Create `little-cube-os/src/ui/ClockFaces.cpp` with the shared parts and the three static faces**

```cpp
#include "../ui/ClockFaces.h"

#include <Arduino_GFX_Library.h>
#include <string.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../ui/Theme.h"
#include "../ui/widgets/Widgets.h"

namespace clockfaces {

namespace {

const char* kWeekdays[7] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                            "Thursday", "Friday", "Saturday"};
const char* kMonths[12] = {"January", "February", "March",     "April",   "May",      "June",
                           "July",    "August",   "September", "October", "November", "December"};

// Formats HH:MM, or --:-- when the clock is not set.
void formatTime(const struct tm* t, char* out, size_t outLen) {
  if (t == nullptr) {
    snprintf(out, outLen, "--:--");
    return;
  }
  snprintf(out, outLen, "%02d:%02d", t->tm_hour, t->tm_min);
}

void formatDate(const struct tm* t, char* out, size_t outLen) {
  if (t == nullptr) {
    snprintf(out, outLen, "time not set");
    return;
  }
  snprintf(out, outLen, "%s, %s %d", kWeekdays[t->tm_wday > 6 ? 0 : t->tm_wday],
           kMonths[t->tm_mon > 11 ? 0 : t->tm_mon], t->tm_mday);
}

// ---------------------------------------------------------------------------
// Static faces
// ---------------------------------------------------------------------------

bool renderBigDigital(Arduino_GFX& gfx, const FaceContext& ctx) {
  char big[8];
  char date[48];
  formatTime(ctx.time, big, sizeof(big));
  formatDate(ctx.time, date, sizeof(date));

  // kTextDim, never pure white: spec §37 names a fixed white max-brightness
  // clock as exactly the thing that burns a panel in.
  widgets::textCentered(gfx, ctx.shiftX, DISPLAY_HEIGHT / 2 - 70 + ctx.shiftY, DISPLAY_WIDTH,
                        big, widgets::TextStyle::Display, theme::kTextDim);
  widgets::textCentered(gfx, ctx.shiftX, DISPLAY_HEIGHT / 2 + 30 + ctx.shiftY, DISPLAY_WIDTH,
                        date, widgets::TextStyle::Caption,
                        ctx.time != nullptr ? theme::kTextDim : theme::kWarn);
  return false;
}

bool renderStacked(Arduino_GFX& gfx, const FaceContext& ctx) {
  char hh[4];
  char mm[4];
  if (ctx.time != nullptr) {
    snprintf(hh, sizeof(hh), "%02d", ctx.time->tm_hour);
    snprintf(mm, sizeof(mm), "%02d", ctx.time->tm_min);
  } else {
    snprintf(hh, sizeof(hh), "--");
    snprintf(mm, sizeof(mm), "--");
  }
  char date[48];
  formatDate(ctx.time, date, sizeof(date));

  const int16_t x = theme::kSafeInset + ctx.shiftX;
  widgets::text(gfx, x, 110 + ctx.shiftY, hh, widgets::TextStyle::Display, theme::kText);
  widgets::text(gfx, x, 210 + ctx.shiftY, mm, widgets::TextStyle::Display, theme::kTextDim);
  widgets::text(gfx, x, 330 + ctx.shiftY, date, widgets::TextStyle::Caption,
                ctx.time != nullptr ? theme::kTextDim : theme::kWarn);
  return false;
}

// Minute buckets for the word clock: minute rounded to the nearest five.
const char* kMinutePhrase[13] = {
    "",         "five past",     "ten past",  "quarter past", "twenty past",
    "twenty-five past",          "half past", "twenty-five to", "twenty to",
    "quarter to", "ten to",      "five to",   ""};
const char* kHourWord[12] = {"twelve", "one", "two",  "three",  "four", "five",
                             "six",    "seven", "eight", "nine", "ten", "eleven"};

bool renderWord(Arduino_GFX& gfx, const FaceContext& ctx) {
  char phrase[64];
  if (ctx.time == nullptr) {
    snprintf(phrase, sizeof(phrase), "time not set");
  } else {
    const int bucket = ((ctx.time->tm_min + 2) / 5) % 13;
    // Buckets 7..12 read "<something> to the NEXT hour"; 12 is the next hour
    // exactly. Bucket 0 is this hour exactly.
    const int hourShift = bucket >= 7 ? 1 : 0;
    const int hour12 = (ctx.time->tm_hour + hourShift) % 12;
    if (bucket == 0 || bucket == 12) {
      snprintf(phrase, sizeof(phrase), "%s o'clock", kHourWord[hour12]);
    } else {
      snprintf(phrase, sizeof(phrase), "%s %s", kMinutePhrase[bucket], kHourWord[hour12]);
    }
  }
  const int16_t x = theme::kSafeInset + ctx.shiftX;
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kSafeInset;
  widgets::textBlock(gfx, x, 130 + ctx.shiftY, w, phrase, widgets::TextStyle::Title,
                     ctx.time != nullptr ? theme::kText : theme::kWarn, 4);
  return false;
}

}  // namespace

const char* name(FaceId id) {
  switch (id) {
    case FaceId::BigDigital: return "Digital";
    case FaceId::Stacked: return "Stacked";
    case FaceId::Word: return "Words";
    case FaceId::Blinky: return "Blinky";
    case FaceId::BigEyes: return "Big Eyes";
    case FaceId::MoodCube: return "Mood Cube";
  }
  return "Digital";
}

bool render(Arduino_GFX& gfx, FaceId id, const FaceContext& ctx) {
  switch (id) {
    case FaceId::BigDigital: return renderBigDigital(gfx, ctx);
    case FaceId::Stacked: return renderStacked(gfx, ctx);
    case FaceId::Word: return renderWord(gfx, ctx);
    // Animated faces land in Task 6; until then they fall back to the default
    // face so the enum is never able to render nothing.
    case FaceId::Blinky:
    case FaceId::BigEyes:
    case FaceId::MoodCube: return renderBigDigital(gfx, ctx);
  }
  return renderBigDigital(gfx, ctx);
}

}  // namespace clockfaces
```

- [ ] **Step 3: Rewrite `little-cube-os/src/apps/ClockApp.h`**

```cpp
#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../ui/StatusBar.h"
#include "../ui/ClockFaces.h"

// Clock (spec §12): a big readable time in one of several selectable faces.
// Tap cycles faces; the choice persists in NVS. Alarms, timers and stopwatch
// are deliberately a separate spec.
//
// This app owns SELECTION, PERSISTENCE and the ANIMATION CLOCK. Pixels live in
// ClockFaces.cpp.
class ClockApp : public App {
 public:
  explicit ClockApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override {}
  void onPause() override {}
  void onResume() override { dirty_ = true; }

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  clockfaces::FaceId face() const;

  Services& services_;
  StatusBar statusBar_;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  // Monotonic ms since the app opened; drives every face animation.
  uint32_t animMs_ = 0;
  // Whether the last render() reported an animation in flight. When false the
  // app redraws only on a minute change or a state bump, exactly as before.
  bool animating_ = false;
  // Minute last drawn, so a static face still updates when the clock rolls.
  int lastMinute_ = -1;
};
```

- [ ] **Step 4: Rewrite `little-cube-os/src/apps/ClockApp.cpp`**

```cpp
#include "ClockApp.h"

#include <Arduino_GFX_Library.h>

#include "../board_config.h"
#include "../core/SystemState.h"
#include "../hardware/DisplayAdapter.h"
#include "../services/SettingsService.h"
#include "../services/TimeService.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"
#include "../ui/widgets/Widgets.h"

void ClockApp::onOpen() {
  animMs_ = 0;
  animating_ = false;
  lastMinute_ = -1;
  dirty_ = true;
}

clockfaces::FaceId ClockApp::face() const {
  const uint8_t stored =
      services_.settings != nullptr ? services_.settings->clockFace() : 0;
  return static_cast<clockfaces::FaceId>(stored < clockfaces::kFaceCount ? stored : 0);
}

void ClockApp::update(uint32_t deltaMs) {
  animMs_ += deltaMs;

  // An animated face wants a frame every tick; the ~30 fps present() cap is
  // what actually bounds the cost. A static face redraws only when the minute
  // rolls over, which is the pre-existing behaviour.
  if (animating_) {
    dirty_ = true;
    return;
  }
  struct tm t;
  if (services_.time != nullptr && services_.time->now(t) && t.tm_min != lastMinute_) {
    dirty_ = true;
  }
}

void ClockApp::render() {
  const SystemState& state = *services_.state;
  if (!dirty_ && state.version == lastStateVersion_) {
    return;
  }
  lastStateVersion_ = state.version;
  dirty_ = false;

  DisplayAdapter* display = services_.display;
  if (display == nullptr || !display->ready()) {
    return;
  }
  Arduino_GFX& gfx = *display->canvas();
  gfx.fillScreen(theme::kBg);

  const int16_t shiftX = services_.amoled->shiftX();
  const int16_t shiftY = services_.amoled->shiftY();
  statusBar_.render(gfx, state, shiftX, shiftY);

  struct tm t;
  const bool haveTime = services_.time != nullptr && services_.time->valid() &&
                        services_.time->now(t);
  lastMinute_ = haveTime ? t.tm_min : -1;

  clockfaces::FaceContext ctx;
  ctx.time = haveTime ? &t : nullptr;
  ctx.state = &state;
  ctx.animMs = animMs_;
  ctx.shiftX = shiftX;
  ctx.shiftY = shiftY;

  const clockfaces::FaceId id = face();
  animating_ = clockfaces::render(gfx, id, ctx);

  widgets::footer(gfx, "tap: next face", clockfaces::name(id), shiftX, shiftY);

  display->markDirty();
}

bool ClockApp::handleInput(const InputEvent& event) {
  if (event.action == InputAction::Tap && services_.settings != nullptr) {
    const uint8_t next =
        static_cast<uint8_t>((services_.settings->clockFace() + 1) % clockfaces::kFaceCount);
    services_.settings->setClockFace(next);
    animMs_ = 0;  // start the new face's animation from a known point
    dirty_ = true;
    return true;
  }
  return false;  // Back/Home fall through to the router
}
```

- [ ] **Step 5: Compile gate**

Run: `./scripts/build.sh`
Expected: clean.

- [ ] **Step 6: Commit**

```bash
git add little-cube-os/src/ui/ClockFaces.h little-cube-os/src/ui/ClockFaces.cpp \
        little-cube-os/src/apps/ClockApp.h little-cube-os/src/apps/ClockApp.cpp
git commit -m "Clock: face abstraction plus digital, stacked and word faces"
```

**Device gate (record, do not tick from a compile):** open Clock, tap three
times and confirm the face changes and its name appears in the footer; reboot
and confirm the face survives; confirm the footer is fully visible and not
clipped at the bottom-left.

---

## Task 6: The three animated faces

**Files:**
- Modify: `little-cube-os/src/ui/ClockFaces.cpp` (helpers, three renderers, dispatch)

- [ ] **Step 1: Add the animation helpers to the anonymous namespace in `ClockFaces.cpp`**

Place after `formatDate()`:

```cpp
// ---------------------------------------------------------------------------
// Animation timing
// ---------------------------------------------------------------------------
// Blinks are scheduled off animMs alone — no stored state, so a face is a pure
// function of (time, state, animMs) and ClockApp never has to reset anything
// but the clock. A fixed cadence reads as mechanical, so the gap is jittered
// by hashing the blink's own index.
constexpr uint32_t kBlinkCloseMs = 140;
constexpr uint32_t kBlinkGapMinMs = 3000;
constexpr uint32_t kBlinkGapSpanMs = 3500;

// Cheap integer hash; deterministic, so the same animMs always draws the same
// frame and there is no RNG dependency.
uint32_t hash32(uint32_t v) {
  v ^= v >> 16;
  v *= 0x7feb352dU;
  v ^= v >> 15;
  v *= 0x846ca68bU;
  v ^= v >> 16;
  return v;
}

// True while the eyes should be shut. Walks forward through blink slots rather
// than dividing, because the gap varies per blink.
bool eyesClosed(uint32_t animMs) {
  uint32_t cursor = 0;
  for (uint32_t i = 0; i < 4096; i++) {
    const uint32_t gap = kBlinkGapMinMs + (hash32(i) % kBlinkGapSpanMs);
    if (animMs < cursor + gap) {
      return false;
    }
    cursor += gap;
    if (animMs < cursor + kBlinkCloseMs) {
      return true;
    }
    cursor += kBlinkCloseMs;
  }
  return false;
}

// Pupil drift: a slow triangle wave in [-1, 1] scaled to `range` pixels.
int16_t glanceOffset(uint32_t animMs, uint32_t periodMs, int16_t range) {
  const uint32_t phase = animMs % periodMs;
  const int32_t half = static_cast<int32_t>(periodMs / 2);
  const int32_t tri = phase < periodMs / 2
                          ? static_cast<int32_t>(phase)
                          : half * 2 - static_cast<int32_t>(phase);
  return static_cast<int16_t>((tri * 2 * range) / half - range);
}

// One eye: white, pupil, catchlight. `openH` is the vertical radius, which the
// blink squeezes to a slit.
void drawEye(Arduino_GFX& gfx, int16_t cx, int16_t cy, int16_t rx, int16_t ry,
             int16_t pupilR, int16_t pupilDx) {
  if (ry <= 2) {
    // Shut: a lid line reads better than a squashed ellipse.
    gfx.fillRect(cx - rx, cy - 2, rx * 2, 4, theme::kText);
    return;
  }
  gfx.fillEllipse(cx, cy, rx, ry, theme::kText);
  gfx.fillCircle(cx + pupilDx, cy + ry / 6, pupilR, theme::kBg);
  gfx.fillCircle(cx + pupilDx + pupilR / 2, cy + ry / 6 - pupilR / 2, pupilR / 3,
                 theme::kText);
}
```

- [ ] **Step 2: Add the three renderers, after `renderWord()`**

```cpp
bool renderBlinky(Arduino_GFX& gfx, const FaceContext& ctx) {
  const bool shut = eyesClosed(ctx.animMs);
  const int16_t cy = 150 + ctx.shiftY;
  const int16_t ry = shut ? 2 : 34;
  const int16_t dx = glanceOffset(ctx.animMs, 7000, 8);

  drawEye(gfx, DISPLAY_WIDTH / 2 - 60 + ctx.shiftX, cy, 30, ry, 12, dx);
  drawEye(gfx, DISPLAY_WIDTH / 2 + 60 + ctx.shiftX, cy, 30, ry, 12, dx);

  // Smile: an arc approximated by three chords, which is cheaper and crisper
  // at this size than fillArc.
  const int16_t mx = DISPLAY_WIDTH / 2 + ctx.shiftX;
  const int16_t my = cy + 70;
  for (int16_t o = 0; o < 4; o++) {
    gfx.drawLine(mx - 30, my + o, mx - 10, my + 14 + o, theme::kText);
    gfx.drawLine(mx - 10, my + 14 + o, mx + 10, my + 14 + o, theme::kText);
    gfx.drawLine(mx + 10, my + 14 + o, mx + 30, my + o, theme::kText);
  }

  char big[8];
  formatTime(ctx.time, big, sizeof(big));
  widgets::textCentered(gfx, ctx.shiftX, my + 60, DISPLAY_WIDTH, big,
                        widgets::TextStyle::Display, theme::kTextDim);
  return true;
}

bool renderBigEyes(Arduino_GFX& gfx, const FaceContext& ctx) {
  const bool shut = eyesClosed(ctx.animMs);
  const int16_t cy = DISPLAY_HEIGHT / 2 + ctx.shiftY;
  const int16_t ry = shut ? 2 : 62;
  const int16_t dx = glanceOffset(ctx.animMs, 8000, 14);

  drawEye(gfx, DISPLAY_WIDTH / 2 - 82 + ctx.shiftX, cy, 52, ry, 22, dx);
  drawEye(gfx, DISPLAY_WIDTH / 2 + 82 + ctx.shiftX, cy, 52, ry, 22, dx);

  // The time is deliberately small and out of the way: this face carries the
  // least information of the six, by design.
  char big[8];
  formatTime(ctx.time, big, sizeof(big));
  widgets::text(gfx, theme::kSafeInset + ctx.shiftX, theme::kStatusBarHeight + 16 + ctx.shiftY,
                big, widgets::TextStyle::Caption, theme::kTextDim);
  return true;
}

bool renderMoodCube(Arduino_GFX& gfx, const FaceContext& ctx) {
  enum class Mood : uint8_t { Charging, Low, Sleepy, Happy };
  Mood mood = Mood::Happy;
  const SystemState* s = ctx.state;
  // First match wins. batteryPercent < 0 means UNKNOWN and must never trigger
  // the low-battery face; and a low battery that is charging is good news, so
  // Charging is checked first deliberately.
  if (s != nullptr && s->charging) {
    mood = Mood::Charging;
  } else if (s != nullptr && s->batteryPercent >= 0 && s->batteryPercent < 15) {
    mood = Mood::Low;
  } else if (ctx.time != nullptr && (ctx.time->tm_hour >= 22 || ctx.time->tm_hour < 7)) {
    mood = Mood::Sleepy;
  }

  const int16_t side = 260;
  const int16_t fx = (DISPLAY_WIDTH - side) / 2 + ctx.shiftX;
  const int16_t fy = (DISPLAY_HEIGHT - side) / 2 - 10 + ctx.shiftY;
  gfx.fillRoundRect(fx, fy, side, side, 60, theme::kPanel);
  gfx.drawRoundRect(fx, fy, side, side, 60, theme::kAccent);

  const int16_t eyeY = fy + 100;
  const int16_t eyeL = fx + 78;
  const int16_t eyeR = fx + side - 78;
  const bool shut = eyesClosed(ctx.animMs);

  switch (mood) {
    case Mood::Charging: {
      // Star eyes: a plus and a cross overlaid reads as a sparkle at this size.
      for (int16_t i = 0; i < 2; i++) {
        const int16_t ex = i == 0 ? eyeL : eyeR;
        gfx.fillRect(ex - 3, eyeY - 20, 6, 40, theme::kText);
        gfx.fillRect(ex - 20, eyeY - 3, 40, 6, theme::kText);
        gfx.drawLine(ex - 14, eyeY - 14, ex + 14, eyeY + 14, theme::kText);
        gfx.drawLine(ex - 14, eyeY + 14, ex + 14, eyeY - 14, theme::kText);
      }
      break;
    }
    case Mood::Low: {
      // Droopy: lids drawn over the top third of each eye.
      drawEye(gfx, eyeL, eyeY, 24, shut ? 2 : 22, 10, 0);
      drawEye(gfx, eyeR, eyeY, 24, shut ? 2 : 22, 10, 0);
      gfx.fillRect(eyeL - 26, eyeY - 24, 52, 16, theme::kPanel);
      gfx.fillRect(eyeR - 26, eyeY - 24, 52, 16, theme::kPanel);
      break;
    }
    case Mood::Sleepy: {
      // Half-closed: two lid lines, no whites at all.
      for (int16_t o = 0; o < 5; o++) {
        gfx.drawLine(eyeL - 24, eyeY + o, eyeL + 24, eyeY + o, theme::kText);
        gfx.drawLine(eyeR - 24, eyeY + o, eyeR + 24, eyeY + o, theme::kText);
      }
      break;
    }
    case Mood::Happy: {
      drawEye(gfx, eyeL, eyeY, 24, shut ? 2 : 26, 11, glanceOffset(ctx.animMs, 6500, 5));
      drawEye(gfx, eyeR, eyeY, 24, shut ? 2 : 26, 11, glanceOffset(ctx.animMs, 6500, 5));
      gfx.fillCircle(fx + 40, eyeY + 44, 14, theme::kBad);
      gfx.fillCircle(fx + side - 40, eyeY + 44, 14, theme::kBad);
      break;
    }
  }

  // Mouth: flat when the battery is low, a smile otherwise.
  const int16_t mx = fx + side / 2;
  const int16_t my = eyeY + 62;
  if (mood == Mood::Low) {
    gfx.fillRect(mx - 26, my, 52, 5, theme::kText);
  } else {
    for (int16_t o = 0; o < 5; o++) {
      gfx.drawLine(mx - 28, my + o, mx - 8, my + 16 + o, theme::kText);
      gfx.drawLine(mx - 8, my + 16 + o, mx + 8, my + 16 + o, theme::kText);
      gfx.drawLine(mx + 8, my + 16 + o, mx + 28, my + o, theme::kText);
    }
  }

  char big[8];
  formatTime(ctx.time, big, sizeof(big));
  widgets::textCentered(gfx, fx, fy + side - 62, side, big, widgets::TextStyle::Title,
                        theme::kTextDim);
  return true;
}
```

- [ ] **Step 3: Replace the fallback dispatch cases in `render()`**

```cpp
    case FaceId::Blinky: return renderBlinky(gfx, ctx);
    case FaceId::BigEyes: return renderBigEyes(gfx, ctx);
    case FaceId::MoodCube: return renderMoodCube(gfx, ctx);
```

- [ ] **Step 4: Compile gate**

Run: `./scripts/build.sh`
Expected: clean. If `fillEllipse` is unavailable in this Arduino_GFX version,
substitute `fillCircle` scaled on the minor axis rather than adding a library.

- [ ] **Step 5: Commit**

```bash
git add little-cube-os/src/ui/ClockFaces.cpp
git commit -m "Clock: Blinky, Big Eyes and Mood Cube animated faces"
```

**Device gate:** eyes blink at an irregular cadence, not a metronome; pupils
drift; MoodCube shows star eyes on USB power and the sleepy face after 22:00;
**and the screen still dims and blanks on the normal timeout** — that last one
is the regression that matters, because nothing here may call `keepAwake()`.

---

## Task 7: Weather — three views

**Files:**
- Modify: `little-cube-os/src/apps/WeatherApp.h`
- Modify: `little-cube-os/src/apps/WeatherApp.cpp`

- [ ] **Step 1: Add view state to `WeatherApp.h`**

Replace the class body's private section and add the enum:

```cpp
 private:
  enum class View : uint8_t { Now, Forecast, Details };

  void renderNow(Arduino_GFX& gfx, const WeatherSnapshot& wx, int16_t top);
  void renderForecast(Arduino_GFX& gfx, const WeatherSnapshot& wx, int16_t top);
  void renderDetails(Arduino_GFX& gfx, const WeatherSnapshot& wx, int16_t top);
  // Shared freshness string, so the three views can never disagree about age.
  void formatFreshness(const WeatherSnapshot& wx, char* out, size_t outLen) const;

  Services& services_;
  StatusBar statusBar_;
  View view_ = View::Now;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;
  widgets::Rect refreshRect_;
```

The header needs `#include "../services/WeatherService.h"` and
`#include "../ui/widgets/Widgets.h"`, and a forward declaration
`class Arduino_GFX;`. Also update the class comment — tap no longer refreshes:

```cpp
// Weather (spec §13): current conditions + 3-day outlook, always labelled with
// freshness. TAP CYCLES VIEWS (Now -> Forecast -> Details); refresh moved to a
// long-press plus a visible button on the Details view, so it stays
// discoverable instead of becoming a hidden gesture. Offline shows the cached
// snapshot with its age, never pretending it is current.
```

- [ ] **Step 2: Replace `render()` in `WeatherApp.cpp`**

```cpp
void WeatherApp::formatFreshness(const WeatherSnapshot& wx, char* out, size_t outLen) const {
  if (wx.fetchedAtUptimeMs == 0) {
    snprintf(out, outLen, "cached (before restart)");
    return;
  }
  const uint32_t ageMin = (millis() - wx.fetchedAtUptimeMs) / 60000UL;
  if (ageMin < 1) {
    snprintf(out, outLen, "updated just now");
  } else if (ageMin < 60) {
    snprintf(out, outLen, "updated %lum ago", (unsigned long)ageMin);
  } else {
    snprintf(out, outLen, "updated %luh ago", (unsigned long)(ageMin / 60));
  }
}

void WeatherApp::render() {
  const SystemState& state = *services_.state;
  if (!dirty_ && state.version == lastStateVersion_) {
    return;
  }
  lastStateVersion_ = state.version;
  dirty_ = false;

  DisplayAdapter* display = services_.display;
  if (display == nullptr || !display->ready()) {
    return;
  }
  Arduino_GFX& gfx = *display->canvas();
  gfx.fillScreen(theme::kBg);

  const int16_t shiftX = services_.amoled->shiftX();
  const int16_t shiftY = services_.amoled->shiftY();
  statusBar_.render(gfx, state, shiftX, shiftY);

  const WeatherSnapshot* wx =
      services_.weather != nullptr ? &services_.weather->snapshot() : nullptr;

  if (wx == nullptr || !wx->valid) {
    const int16_t top = widgets::header(gfx, "Weather", shiftX, shiftY);
    // These two states are deliberately distinct: one is fixable by walking to
    // Wi-Fi range, the other needs phone setup.
    widgets::text(gfx, theme::kPadding, top + 30,
                  state.internet ? "no location set - use phone setup"
                                 : "offline - no cached weather yet",
                  widgets::TextStyle::Body, theme::kTextDim);
    refreshRect_ = widgets::button(gfx, theme::kPadding, top + 90,
                                   DISPLAY_WIDTH - 2 * theme::kPadding, 56, "Retry", true);
    widgets::footer(gfx, "tap the button to retry", nullptr, shiftX, shiftY);
    display->markDirty();
    return;
  }

  const char* title = "Weather";
  const char* tapHint = "tap: forecast";
  switch (view_) {
    case View::Now: title = "Now"; tapHint = "tap: forecast"; break;
    case View::Forecast: title = "Forecast"; tapHint = "tap: details"; break;
    case View::Details: title = "Details"; tapHint = "tap: now"; break;
  }
  const int16_t top = widgets::header(gfx, title, shiftX, shiftY);

  refreshRect_ = widgets::Rect{};  // only the Details view offers a button
  switch (view_) {
    case View::Now: renderNow(gfx, *wx, top); break;
    case View::Forecast: renderForecast(gfx, *wx, top); break;
    case View::Details: renderDetails(gfx, *wx, top); break;
  }

  widgets::footer(gfx, tapHint, "hold: refresh", shiftX, shiftY);
  display->markDirty();
}

void WeatherApp::renderNow(Arduino_GFX& gfx, const WeatherSnapshot& wx, int16_t top) {
  widgets::text(gfx, theme::kPadding, top, wx.location, widgets::TextStyle::Caption,
                theme::kAccent);

  char freshness[40];
  formatFreshness(wx, freshness, sizeof(freshness));
  widgets::text(gfx, theme::kPadding, top + 22, freshness, widgets::TextStyle::Caption,
                wx.fetchedAtUptimeMs == 0 ? theme::kWarn : theme::kTextDim);

  icons::draw(gfx, icons::forCondition(wx.condition), theme::kPadding, top + 56, 96,
              theme::kAccent, theme::kBg);

  char big[12];
  snprintf(big, sizeof(big), "%d\xF8", (int)(wx.temperatureC + 0.5f));
  widgets::text(gfx, theme::kPadding + 120, top + 60, big, widgets::TextStyle::Display,
                theme::kText);

  widgets::text(gfx, theme::kPadding, top + 176, wx.condition, widgets::TextStyle::Body,
                theme::kText);

  char range[48];
  snprintf(range, sizeof(range), "H %d\xF8   L %d\xF8   rain %d%%", (int)(wx.highC + 0.5f),
           (int)(wx.lowC + 0.5f), wx.precipitationChancePct);
  widgets::text(gfx, theme::kPadding, top + 212, range, widgets::TextStyle::Body,
                theme::kTextDim);
}

void WeatherApp::renderForecast(Arduino_GFX& gfx, const WeatherSnapshot& wx, int16_t top) {
  static const char* kWeekdaysShort[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  struct tm t;
  const bool haveDate =
      services_.time != nullptr && services_.time->valid() && services_.time->now(t);

  int16_t y = top + 10;
  for (int i = 0; i < 3; i++) {
    char label[8];
    if (i == 0) {
      snprintf(label, sizeof(label), "Today");
    } else if (i == 1) {
      snprintf(label, sizeof(label), "Tmrw");
    } else if (haveDate) {
      snprintf(label, sizeof(label), "%s", kWeekdaysShort[(t.tm_wday + i) % 7]);
    } else {
      snprintf(label, sizeof(label), "+%dd", i);
    }

    widgets::card(gfx, theme::kPadding, y, DISPLAY_WIDTH - 2 * theme::kPadding, 96);
    widgets::text(gfx, theme::kPadding + 16, y + 34, label, widgets::TextStyle::Body,
                  theme::kText);
    // The glyph replaces the trailing condition text, which truncated on the
    // device ("Overcas").
    icons::draw(gfx, icons::forCondition(wx.days[i].condition), theme::kPadding + 110, y + 20,
                56, theme::kAccent, theme::kPanel);

    char temps[24];
    snprintf(temps, sizeof(temps), "%d\xF8 / %d\xF8", (int)(wx.days[i].highC + 0.5f),
             (int)(wx.days[i].lowC + 0.5f));
    widgets::text(gfx, theme::kPadding + 186, y + 22, temps, widgets::TextStyle::Body,
                  theme::kText);

    char rain[16];
    snprintf(rain, sizeof(rain), "rain %d%%", wx.days[i].precipitationChancePct);
    widgets::text(gfx, theme::kPadding + 186, y + 56, rain, widgets::TextStyle::Caption,
                  theme::kTextDim);
    y += 104;
  }
}

void WeatherApp::renderDetails(Arduino_GFX& gfx, const WeatherSnapshot& wx, int16_t top) {
  int16_t y = top + 6;
  const int16_t labelX = theme::kPadding;
  const int16_t valueX = theme::kPadding + 130;

  struct Row {
    const char* label;
    char value[40];
  };
  Row rows[5];
  snprintf(rows[0].value, sizeof(rows[0].value), "%s", wx.location);
  rows[0].label = "Location";
  snprintf(rows[1].value, sizeof(rows[1].value), "%s", wx.condition);
  rows[1].label = "Condition";
  snprintf(rows[2].value, sizeof(rows[2].value), "%d%%", wx.precipitationChancePct);
  rows[2].label = "Rain";
  snprintf(rows[3].value, sizeof(rows[3].value), "%d\xF8 / %d\xF8", (int)(wx.highC + 0.5f),
           (int)(wx.lowC + 0.5f));
  rows[3].label = "High / low";
  rows[4].label = "Connection";
  snprintf(rows[4].value, sizeof(rows[4].value), "%s",
           services_.state->internet ? "online" : "offline");

  for (const Row& r : rows) {
    widgets::text(gfx, labelX, y, r.label, widgets::TextStyle::Caption, theme::kTextDim);
    widgets::text(gfx, valueX, y, r.value, widgets::TextStyle::Body, theme::kText);
    y += 40;
  }

  char freshness[40];
  formatFreshness(wx, freshness, sizeof(freshness));
  widgets::text(gfx, labelX, y, "Updated", widgets::TextStyle::Caption, theme::kTextDim);
  widgets::text(gfx, valueX, y, freshness, widgets::TextStyle::Body,
                wx.fetchedAtUptimeMs == 0 ? theme::kWarn : theme::kText);
  y += 56;

  // A visible control, so refresh is never gesture-only.
  refreshRect_ = widgets::button(gfx, theme::kPadding, y,
                                 DISPLAY_WIDTH - 2 * theme::kPadding, 56, "Refresh", true);
}
```

Add these includes at the top of `WeatherApp.cpp`:

```cpp
#include "../ui/Icons.h"
#include "../ui/widgets/Widgets.h"
```

- [ ] **Step 3: Replace `handleInput()`**

```cpp
bool WeatherApp::handleInput(const InputEvent& event) {
  if (event.action == InputAction::Tap) {
    // The Details view's button wins over the view cycle, so the visible
    // control is never shadowed by the background gesture.
    if (refreshRect_.w > 0 && refreshRect_.contains(event.x, event.y)) {
      if (services_.weather != nullptr && services_.weather->refresh()) {
        Serial.println("[weather] manual refresh requested");
      }
      dirty_ = true;
      return true;
    }
    switch (view_) {
      case View::Now: view_ = View::Forecast; break;
      case View::Forecast: view_ = View::Details; break;
      case View::Details: view_ = View::Now; break;
    }
    dirty_ = true;
    return true;
  }
  if (event.action == InputAction::LongPress) {
    if (services_.weather != nullptr && services_.weather->refresh()) {
      Serial.println("[weather] manual refresh requested");
    }
    dirty_ = true;
    return true;
  }
  return false;
}
```

- [ ] **Step 4: Reset the view on open**

In `WeatherApp.h`, change `onOpen()`:

```cpp
  void onOpen() override {
    view_ = View::Now;  // reopening always lands on the hero, not a stale view
    dirty_ = true;
  }
```

- [ ] **Step 5: Compile gate**

Run: `./scripts/build.sh`
Expected: clean.

- [ ] **Step 6: Commit**

```bash
git add little-cube-os/src/apps/WeatherApp.h little-cube-os/src/apps/WeatherApp.cpp
git commit -m "Weather: Now/Forecast/Details views with condition glyphs"
```

**Device gate:** tap cycles all three views and wraps; long-press refreshes from
any view; the Details button refreshes; per-day glyphs match the conditions; the
offline and no-location states still read differently; a snapshot restored from
cache after a reboot still says `cached (before restart)` in the warning color.

---

## Task 8: Today — cards

**Files:**
- Modify: `little-cube-os/src/apps/TodayApp.h` (comment only)
- Modify: `little-cube-os/src/apps/TodayApp.cpp` (render)

- [ ] **Step 1: Correct the class comment in `TodayApp.h`**

The current comment promises rows that this task deletes:

```cpp
// Today (spec §11): the default practical dashboard — time, date, weather and
// device status at a glance, as cards.
//
// There is deliberately NO "Next event" or "Alarm" row. Nothing in the codebase
// writes SystemState::alarmArmed, and CalendarService is never consulted here,
// so those rows could only ever render a placeholder dash. A screen that always
// says "- nothing scheduled" is stating something it cannot know. Wiring
// CalendarService in is a small, separate change — and it needs an onOpen()
// load, because calendar reads touch SD and must never happen in render().
```

- [ ] **Step 2: Replace `render()` in `TodayApp.cpp`**

```cpp
void TodayApp::render() {
  const SystemState& state = *services_.state;
  if (!dirty_ && state.version == lastStateVersion_) {
    return;
  }
  lastStateVersion_ = state.version;
  dirty_ = false;

  DisplayAdapter* display = services_.display;
  if (display == nullptr || !display->ready()) {
    return;
  }
  Arduino_GFX& gfx = *display->canvas();
  gfx.fillScreen(theme::kBg);

  const int16_t shiftX = services_.amoled->shiftX();
  const int16_t shiftY = services_.amoled->shiftY();
  statusBar_.render(gfx, state, shiftX, shiftY);

  struct tm t;
  const bool valid =
      services_.time != nullptr && services_.time->valid() && services_.time->now(t);

  // --- Hero: time + date -----------------------------------------------
  char big[8];
  if (valid) {
    snprintf(big, sizeof(big), "%02d:%02d", t.tm_hour, t.tm_min);
  } else {
    snprintf(big, sizeof(big), "--:--");
  }
  const int16_t x = theme::kPadding;
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding;
  int16_t y = theme::kStatusBarHeight + 18 + shiftY;

  widgets::text(gfx, x + shiftX, y, big, widgets::TextStyle::Display,
                valid ? theme::kText : theme::kTextDim);
  y += 74;

  char date[32];
  if (valid) {
    snprintf(date, sizeof(date), "%s, %s %d", kWeekdaysShort[t.tm_wday > 6 ? 0 : t.tm_wday],
             kMonthsShort[t.tm_mon > 11 ? 0 : t.tm_mon], t.tm_mday);
  } else {
    snprintf(date, sizeof(date), "date pending time sync");
  }
  widgets::text(gfx, x + shiftX, y, date, widgets::TextStyle::Caption, theme::kTextDim);
  y += 40;

  // --- Weather card ----------------------------------------------------
  const WeatherSnapshot* wx =
      services_.weather != nullptr ? &services_.weather->snapshot() : nullptr;
  widgets::card(gfx, x, y, w, 108);
  if (wx != nullptr && wx->valid) {
    icons::draw(gfx, icons::forCondition(wx->condition), x + 16, y + 22, 64, theme::kAccent,
                theme::kPanel);

    char temp[12];
    snprintf(temp, sizeof(temp), "%d\xF8", (int)(wx->temperatureC + 0.5f));
    widgets::text(gfx, x + 96, y + 18, temp, widgets::TextStyle::Title, theme::kText);
    widgets::text(gfx, x + 96, y + 62, wx->condition, widgets::TextStyle::Caption,
                  theme::kTextDim);

    char hl[24];
    snprintf(hl, sizeof(hl), "H %d\xF8  L %d\xF8", (int)(wx->highC + 0.5f),
             (int)(wx->lowC + 0.5f));
    widgets::textRight(gfx, x + w - 16, y + 18, hl, widgets::TextStyle::Caption,
                       theme::kTextDim);

    // Never present stale data as current (spec §11).
    if (wx->fetchedAtUptimeMs == 0) {
      widgets::textRight(gfx, x + w - 16, y + 62, "cached", widgets::TextStyle::Caption,
                         theme::kWarn);
    } else {
      const uint32_t ageMin = (millis() - wx->fetchedAtUptimeMs) / 60000UL;
      if (ageMin > 60) {
        char age[24];
        snprintf(age, sizeof(age), "%luh old", (unsigned long)(ageMin / 60));
        widgets::textRight(gfx, x + w - 16, y + 62, age, widgets::TextStyle::Caption,
                           theme::kWarn);
      }
    }
  } else {
    widgets::text(gfx, x + 16, y + 40, "weather: not fetched yet", widgets::TextStyle::Body,
                  theme::kTextDim);
  }
  y += 120;

  // --- Status card -----------------------------------------------------
  widgets::card(gfx, x, y, w, 140);
  int16_t ry = y + 16;

  char battery[32];
  if (!state.batteryPresent || state.batteryPercent < 0) {
    snprintf(battery, sizeof(battery), "unknown");
  } else if (state.charging) {
    snprintf(battery, sizeof(battery), "%d%% - charging", state.batteryPercent);
  } else {
    snprintf(battery, sizeof(battery), "%d%%", state.batteryPercent);
  }
  widgets::text(gfx, x + 16, ry, "Battery", widgets::TextStyle::Caption, theme::kTextDim);
  widgets::text(gfx, x + 130, ry, battery, widgets::TextStyle::Body,
                (state.batteryPercent >= 0 && state.batteryPercent < 15 && !state.charging)
                    ? theme::kWarn
                    : theme::kText);
  ry += 40;

  widgets::text(gfx, x + 16, ry, "Storage", widgets::TextStyle::Caption, theme::kTextDim);
  widgets::text(gfx, x + 130, ry, sdCardStateName(state.sd), widgets::TextStyle::Body,
                state.sd == SdCardState::Mounted ? theme::kText : theme::kWarn);
  ry += 40;

  widgets::text(gfx, x + 16, ry, "Network", widgets::TextStyle::Caption, theme::kTextDim);
  widgets::text(gfx, x + 130, ry, state.internet ? "online" : "offline",
                widgets::TextStyle::Body, state.internet ? theme::kText : theme::kTextDim);

  // Recording appears only while it is true — an absent row beats a dash.
  if (state.recording) {
    widgets::footer(gfx, "recording now", nullptr, shiftX, shiftY);
  }

  display->markDirty();
}
```

Add these includes to `TodayApp.cpp`:

```cpp
#include "../hardware/SdCardState.h"
#include "../ui/Icons.h"
#include "../ui/widgets/Widgets.h"
```

`sdCardStateName()` is already declared in `SdCardState.h` and is used the same
way by `RecorderApp`.

- [ ] **Step 3: Compile gate**

Run: `./scripts/build.sh`
Expected: clean. An `unused variable` warning for the removed rows' helpers
means dead code was left behind — delete it rather than silencing it.

- [ ] **Step 4: Commit**

```bash
git add little-cube-os/src/apps/TodayApp.h little-cube-os/src/apps/TodayApp.cpp
git commit -m "Today: card layout with weather glyph and a real battery row"
```

**Device gate:** cards render without overlapping; the weather glyph matches the
condition; the battery row tracks a real charge/discharge; pulling the SD card
changes the Storage row to a specific state, not a generic error; the recording
footer appears only while recording.

---

## Task 9: Assistant — bubbles, meter, paging

**Files:**
- Modify: `little-cube-os/src/apps/AssistantApp.h`
- Modify: `little-cube-os/src/apps/AssistantApp.cpp`

- [ ] **Step 1: Replace the private section of `AssistantApp.h`**

```cpp
 private:
  // Answer pagination: long answers used to truncate silently at 11 lines.
  void layoutAnswerPages();
  int16_t answerAreaTop() const;
  int16_t answerAreaBottom() const;

  Services& services_;
  StatusBar statusBar_;
  widgets::Rect talkRect_;
  uint8_t lastState_ = 255;
  uint32_t lastStateVersion_ = 0;
  uint32_t tickMs_ = 0;
  bool dirty_ = true;

  // Smoothed microphone level, 0..255, decayed in update() so the meter falls
  // rather than flickering between frames.
  uint8_t micLevel_ = 0;

  static constexpr uint8_t kMaxAnswerPages = 12;
  uint8_t answerPage_ = 0;
  uint8_t answerPages_ = 1;
  uint16_t answerPageOffset_[kMaxAnswerPages] = {};
```

Replace the local `struct Rect` with the shared one — add
`#include "../ui/widgets/Widgets.h"` and delete the nested definition. The
duplicate type is exactly the sort of thing that drifts.

- [ ] **Step 2: Re-paginate on state transitions, and add the level decay**

⚠️ **Read this before writing the code.** The obvious way to decide when to
re-paginate — caching `lastAnswer()`'s pointer and comparing it — **does not
work**. `lastAnswer()` returns a pointer into a fixed buffer the service owns,
so the pointer is byte-identical for every answer it ever produces. A pointer
comparison would detect a change exactly never, and the screen would show the
first answer's pagination forever. Trigger off the state transition instead: a
new answer always arrives with one.

In `update()`, extend the existing state-change branch:

```cpp
  const uint8_t s = static_cast<uint8_t>(ai->state());
  if (s != lastState_) {
    lastState_ = s;
    layoutAnswerPages();  // see the warning above: never compare lastAnswer()
    dirty_ = true;
  }
```

Then, inside the existing `Listening` branch:

```cpp
  if (ai->state() == AssistantService::State::Listening) {
    // recordedPeak() is a volatile uint16 the capture task already maintains;
    // reading it costs nothing and needs no change to the audio path.
    if (services_.audio != nullptr) {
      const uint16_t peak = services_.audio->recordedPeak();
      const uint8_t scaled = static_cast<uint8_t>(peak >> 8);
      // Attack instantly, decay slowly: a meter that falls as fast as it rises
      // reads as noise.
      micLevel_ = scaled > micLevel_ ? scaled
                                     : static_cast<uint8_t>(micLevel_ - (micLevel_ / 8));
    }
    tickMs_ += deltaMs;
    if (tickMs_ >= 100) {  // 10 Hz: fast enough to look live, cheap enough
      tickMs_ = 0;
      dirty_ = true;
    }
  } else {
    micLevel_ = 0;
  }
```

Add `#include "../hardware/audio/AudioAdapter.h"`.

- [ ] **Step 3: Add pagination helpers**

```cpp
int16_t AssistantApp::answerAreaTop() const {
  return talkRect_.y + talkRect_.h + 16;
}

int16_t AssistantApp::answerAreaBottom() const {
  return DISPLAY_HEIGHT - 56;  // clears the footer band
}

void AssistantApp::layoutAnswerPages() {
  AssistantService* ai = services_.assistant;
  answerPages_ = 1;
  answerPage_ = 0;
  answerPageOffset_[0] = 0;
  if (ai == nullptr) {
    return;
  }
  const char* answer = ai->lastAnswer();
  if (answer == nullptr || answer[0] == '\0') {
    return;
  }
  // widgets::textBlock returns the y below the last line drawn, so paginating
  // means walking it with a line cap and recording where each page stopped.
  // The canvas is needed to measure real glyph widths.
  DisplayAdapter* display = services_.display;
  if (display == nullptr || !display->ready()) {
    return;
  }
  Arduino_GFX& gfx = *display->canvas();
  const int16_t w = DISPLAY_WIDTH - 2 * theme::kPadding - 24;
  const int16_t lineH = widgets::lineHeight(widgets::TextStyle::Caption);
  const int16_t budget = answerAreaBottom() - answerAreaTop() - 24;
  const uint8_t linesPerPage = budget > 0 ? static_cast<uint8_t>(budget / lineH) : 1;

  size_t offset = 0;
  const size_t len = strlen(answer);
  while (offset < len && answerPages_ < kMaxAnswerPages) {
    const size_t consumed =
        widgets::measureBlock(gfx, answer + offset, w, widgets::TextStyle::Caption,
                              linesPerPage);
    if (consumed == 0) {
      break;  // nothing fits; stop rather than loop forever
    }
    offset += consumed;
    if (offset < len) {
      answerPageOffset_[answerPages_] = static_cast<uint16_t>(offset);
      answerPages_++;
    }
  }
}
```

⚠️ This needs one new primitive: `widgets::measureBlock()`, which reports how
many characters a wrapped block consumes for a given line budget. Without it,
pagination and rendering would each wrap independently and could disagree —
the same trap `NewsApp` avoids with its single shared wrap walker. Add to
`Widgets.h` beside `textBlock`:

```cpp
// How many characters of `text` a textBlock() of the same width, style and
// line cap would consume. Pagination and rendering MUST share one wrap walker;
// two independent ones drift and lose lines at page boundaries.
size_t measureBlock(Arduino_GFX& gfx, const char* text, int16_t w, TextStyle style,
                    uint8_t maxLines);
```

And to `Widgets.cpp`, factored out of the existing `textBlock(TextStyle)` so
both call the same loop:

```cpp
namespace {
// Shared wrap walker. Draws when `gfxOut` is non-null; always returns the
// number of characters consumed.
size_t wrapWalk(Arduino_GFX& gfx, const char* text, int16_t x, int16_t y, int16_t w,
                TextStyle style, uint16_t color, uint8_t maxLines, bool draw,
                int16_t* yOut) {
  if (text == nullptr || text[0] == '\0') {
    if (yOut != nullptr) {
      *yOut = y;
    }
    return 0;
  }
  const int16_t lineH = lineHeight(style);
  const size_t len = strlen(text);
  size_t pos = 0;
  uint8_t lines = 0;
  while (pos < len && lines < maxLines) {
    // Grow the candidate until it no longer fits, then back off to the last
    // space inside the window.
    size_t take = 0;
    size_t lastSpace = 0;
    char probe[96];
    while (pos + take < len && take < sizeof(probe) - 1) {
      probe[take] = text[pos + take];
      probe[take + 1] = '\0';
      if (textWidth(gfx, probe, style) > w) {
        break;
      }
      if (text[pos + take] == ' ') {
        lastSpace = take;
      }
      take++;
    }
    if (pos + take < len && lastSpace > 0) {
      take = lastSpace;
    }
    if (take == 0) {
      break;  // a single glyph wider than the box; refuse to spin
    }
    if (draw) {
      char line[96];
      const size_t n = take < sizeof(line) - 1 ? take : sizeof(line) - 1;
      memcpy(line, text + pos, n);
      line[n] = '\0';
      text(gfx, x, y, line, style, color);
    }
    pos += take;
    while (pos < len && text[pos] == ' ') {
      pos++;
    }
    y += lineH;
    lines++;
  }
  if (yOut != nullptr) {
    *yOut = y;
  }
  return pos;
}
}  // namespace
```

`text()` is declared in `Widgets.h`, which `Widgets.cpp` includes first, so the
anonymous-namespace helper can call it even though its definition appears later
in the file. Then `textBlock(TextStyle)` becomes a call to
`wrapWalk(..., /*draw=*/true, &yOut)` and `measureBlock` a call with
`draw=false`, so the two can never disagree.

- [ ] **Step 4: Replace `render()`'s body below the talk button**

```cpp
  // Live level meter while listening: the screen must show the mic is open.
  if (listening) {
    const int16_t mx = talkRect_.x;
    const int16_t my = talkRect_.y + talkRect_.h + 10;
    const int16_t mw = talkRect_.w;
    gfx.fillRoundRect(mx, my, mw, 10, 5, theme::kPanel);
    const int16_t lit = static_cast<int16_t>((static_cast<int32_t>(mw) * micLevel_) / 255);
    if (lit > 0) {
      gfx.fillRoundRect(mx, my, lit, 10, 5, theme::kBad);
    }
  }

  int16_t y = answerAreaTop() + (listening ? 20 : 0);

  if (s == AssistantService::State::Error && ai->lastError()[0] != '\0') {
    // Error stacks ABOVE the exchange so the failed question stays visible.
    y = widgets::textBlock(gfx, theme::kPadding, y, DISPLAY_WIDTH - 2 * theme::kPadding,
                           ai->lastError(), widgets::TextStyle::Caption, theme::kBad, 3);
    y += 10;
  }

  // Question bubble: right-aligned on kPanelAlt.
  if (ai->lastTranscript()[0] != '\0') {
    const int16_t bw = DISPLAY_WIDTH - 2 * theme::kPadding - 40;
    const int16_t bx = DISPLAY_WIDTH - theme::kPadding - bw;
    int16_t inner = y + 12;
    const int16_t after =
        widgets::textBlock(gfx, bx + 14, inner, bw - 28, ai->lastTranscript(),
                           widgets::TextStyle::Caption, theme::kText, 3);
    gfx.fillRoundRect(bx, y, bw, after - y + 12, theme::kCardRadius, theme::kPanelAlt);
    // Re-draw the text over the fill: the bubble height is only known after
    // the wrap, and the canvas has no transparency to draw behind.
    widgets::textBlock(gfx, bx + 14, inner, bw - 28, ai->lastTranscript(),
                       widgets::TextStyle::Caption, theme::kText, 3);
    y = after + 22;
  }

  // Answer bubble: left-aligned on kPanel, paginated.
  if (ai->lastAnswer()[0] != '\0') {
    // Pagination is computed in update() on the state transition, never here:
    // render() must stay free of layout work that can change what it draws
    // mid-frame.
    const int16_t bw = DISPLAY_WIDTH - 2 * theme::kPadding - 40;
    const int16_t bx = theme::kPadding;
    const uint16_t offset = answerPageOffset_[answerPage_ < answerPages_ ? answerPage_ : 0];
    const int16_t budget = answerAreaBottom() - y - 24;
    const uint8_t lineCap =
        budget > 0 ? static_cast<uint8_t>(budget / widgets::lineHeight(
                                                       widgets::TextStyle::Caption))
                   : 1;
    const int16_t after =
        widgets::textBlock(gfx, bx + 14, y + 12, bw - 28, ai->lastAnswer() + offset,
                           widgets::TextStyle::Caption, theme::kText, lineCap);
    gfx.fillRoundRect(bx, y, bw, after - y + 12, theme::kCardRadius, theme::kPanel);
    widgets::textBlock(gfx, bx + 14, y + 12, bw - 28, ai->lastAnswer() + offset,
                       widgets::TextStyle::Caption, theme::kText, lineCap);
  }

  char left[40];
  if (answerPages_ > 1) {
    snprintf(left, sizeof(left), "pg %u/%u", (unsigned)(answerPage_ + 1),
             (unsigned)answerPages_);
  } else if (ai->historyDepth() > 0) {
    snprintf(left, sizeof(left), "%u exchange%s in memory", ai->historyDepth(),
             ai->historyDepth() == 1 ? "" : "s");
  } else {
    left[0] = '\0';
  }
  widgets::footer(gfx, left, answerPages_ > 1 ? "swipe up/down" : nullptr,
                  services_.amoled->shiftX(), services_.amoled->shiftY());
  display->markDirty();
```

Replace the hand-placed title with
`widgets::header(gfx, "Assistant", shiftX, shiftY)` and delete the file-local
`printWrapped()` helper entirely — it assumes a fixed character cell and will
wrap wrongly against a proportional face.

- [ ] **Step 5: Add answer paging to `handleInput()`**

Before the existing `Tap` handling:

```cpp
  if (event.action == InputAction::SwipeUp && answerPage_ + 1 < answerPages_) {
    answerPage_++;
    dirty_ = true;
    return true;
  }
  if (event.action == InputAction::SwipeDown && answerPage_ > 0) {
    answerPage_--;
    dirty_ = true;
    return true;
  }
```

- [ ] **Step 6: Compile gate**

Run: `./scripts/build.sh`
Expected: clean.

- [ ] **Step 7: Commit**

```bash
git add little-cube-os/src/apps/AssistantApp.h little-cube-os/src/apps/AssistantApp.cpp \
        little-cube-os/src/ui/widgets/Widgets.h little-cube-os/src/ui/widgets/Widgets.cpp
git commit -m "Assistant: bubbles, live mic meter and paged answers"
```

**Device gate:** the meter moves with your voice and falls smoothly; a long
answer pages with swipe up/down and the last page is not clipped; `Back` during
listening still cancels without sending; an error still shows above the
question.

---

## Task 10: News — footers, QR page, tracking strip

**Files:**
- Modify: `little-cube-os/src/apps/NewsApp.h`
- Modify: `little-cube-os/src/apps/NewsApp.cpp`

- [ ] **Step 1: Add the tracking-strip helper to the anonymous namespace in `NewsApp.cpp`**

```cpp
// Known tracking keys, plus the `at_*` family the BBC feed uses.
bool isTrackingKey(const char* key, size_t len) {
  static const char* kExact[] = {"utm_source", "utm_medium", "utm_campaign",
                                 "utm_term",   "utm_content", "ref", "fbclid"};
  if (len > 3 && strncmp(key, "at_", 3) == 0) {
    return true;
  }
  for (const char* k : kExact) {
    if (strlen(k) == len && strncmp(key, k, len) == 0) {
      return true;
    }
  }
  return false;
}

// Drops the query string ONLY when every key in it is a known tracker. If any
// other key is present the query survives whole — a denser QR that resolves
// beats a clean one that 404s.
void stripTrackingParams(const char* url, char* out, size_t outLen) {
  const char* q = strchr(url, '?');
  if (q == nullptr) {
    snprintf(out, outLen, "%s", url);
    return;
  }
  const char* p = q + 1;
  while (*p != '\0') {
    const char* eq = strchr(p, '=');
    const char* amp = strchr(p, '&');
    const char* keyEnd = (eq != nullptr && (amp == nullptr || eq < amp)) ? eq : amp;
    const size_t keyLen = keyEnd != nullptr ? static_cast<size_t>(keyEnd - p) : strlen(p);
    if (!isTrackingKey(p, keyLen)) {
      snprintf(out, outLen, "%s", url);  // a real parameter: keep everything
      return;
    }
    if (amp == nullptr) {
      break;
    }
    p = amp + 1;
  }
  const size_t baseLen = static_cast<size_t>(q - url);
  const size_t n = baseLen < outLen - 1 ? baseLen : outLen - 1;
  memcpy(out, url, n);
  out[n] = '\0';
}
```

- [ ] **Step 2: Add QR page state to `NewsApp.h`**

```cpp
  // The QR is one extra page appended to the detail pager, so reaching it is
  // the paging gesture the user already knows: pg N/N simply IS the QR page.
  bool qrPageActive() const { return detailPage_ == detailPages_; }
  char qrUrl_[200] = "";
```

`detailPages_` counts the text pages; the pager's upper bound becomes
`detailPages_ + 1`. Every existing comparison of the form
`detailPage_ + 1 < detailPages_` becomes `detailPage_ < detailPages_`.

- [ ] **Step 3: Populate `qrUrl_` in `composeDetail()`**

At the end of the existing `composeDetail()`:

```cpp
  qrUrl_[0] = '\0';
  if (services_.news != nullptr && openIndex_ < count()) {
    const NewsHeadline& h = services_.news->headline(openIndex_);
    if (h.link[0] != '\0') {
      stripTrackingParams(h.link, qrUrl_, sizeof(qrUrl_));
    }
  }
```

`composeDetail()` already resolves the current headline to build `detail_`;
reuse that local instead of calling `headline()` a second time if the accessor
name differs — check `NewsService.h` for the exact spelling before writing this
line. The bounds guard (`openIndex_ < count()`) must stay either way: a
background refresh can shrink the list between the tap and this call.

- [ ] **Step 4: Render the QR page in `renderDetail()`**

Wrap the existing text rendering in a branch:

```cpp
  if (qrPageActive()) {
    const int16_t side = 260;
    const int16_t qx = (DISPLAY_WIDTH - side) / 2;
    const int16_t qy = kDetailTop + 20;
    // Explicitly light quiet zone: a QR on theme::kBg is unscannable on every
    // dark palette, and half the built-in themes are dark.
    const bool ok = qrcode::draw(gfx, qx, qy, side, qrUrl_, 0x0000, 0xFFFF);
    if (ok) {
      widgets::textCentered(gfx, 0, qy + side + 16, DISPLAY_WIDTH, "scan to open",
                            widgets::TextStyle::Body, theme::kText);
    } else {
      // A missing QR is a degraded page, never a blank one.
      widgets::textCentered(gfx, 0, kDetailTop + 40, DISPLAY_WIDTH,
                            "link too long to encode", widgets::TextStyle::Body,
                            theme::kWarn);
      widgets::textBlock(gfx, theme::kPadding, kDetailTop + 90,
                         DISPLAY_WIDTH - 2 * theme::kPadding, qrUrl_,
                         widgets::TextStyle::Caption, theme::kTextDim, 6);
    }
  } else {
    // ... the existing wrapWalk() call, unchanged ...
  }
```

Add `#include "../ui/QrCode.h"`.

- [ ] **Step 5: Lower `kDetailBottom` to clear the new footer rule**

⚠️ Found during Task 1's review. `NewsApp.cpp:24` defines
`kDetailBottom = DISPLAY_HEIGHT - 36` (= 412). The shared footer's rule now
sits at 404, so the detail text area currently runs **8 px past it** and body
text would draw on top of the hairline. Change it to sit clear of the band:

```cpp
// Footer band starts here. widgets::footer() puts its rule at
// DISPLAY_HEIGHT - 44 and returns a content budget 12 px above that, so this
// must not exceed 392.
constexpr int16_t kDetailBottom = DISPLAY_HEIGHT - 56;
```

Re-check `detailMaxLines()` after this change — one fewer line may now fit per
page, and the pagination must be recomputed against the value actually used or
the last line of each page is silently lost.

- [ ] **Step 6: Convert both footers**

Replace the detail footer (`NewsApp.cpp:392-399`):

```cpp
  char foot[24];
  snprintf(foot, sizeof(foot), "pg %u/%u   A%u", (unsigned)(detailPage_ + 1),
           (unsigned)(detailPages_ + 1), (unsigned)fs);
  widgets::footer(gfx, foot, "swipe: prev/next", services_.amoled->shiftX(),
                  services_.amoled->shiftY());
```

And the list footer (`NewsApp.cpp:300`) — the one that was never photographed
but clips identically — with the equivalent `widgets::footer()` call, keeping
whatever string it currently builds.

- [ ] **Step 6: Compile gate**

Run: `./scripts/build.sh`
Expected: clean.

- [ ] **Step 7: Commit**

```bash
git add little-cube-os/src/apps/NewsApp.h little-cube-os/src/apps/NewsApp.cpp
git commit -m "News: safe-area footers and a scannable QR page"
```

**Device gate:** the bottom-left footer is fully visible on both the list and the
detail; paging to the last page shows the QR; **a phone actually scans it and
opens the right article**; a hand-made very long link shows the refusal text
instead of a broken code.

---

## Task 11: Recorder

**Files:**
- Modify: `little-cube-os/src/apps/RecorderApp.cpp`

- [ ] **Step 1: Add the duration formatter to the anonymous namespace**

```cpp
// "~16274 min left on card" both overran the line and was unreadable as a
// quantity. Render the largest sensible unit instead.
void formatRemaining(uint32_t seconds, char* out, size_t outLen) {
  const uint32_t minutes = seconds / 60;
  if (minutes >= 48 * 60) {
    snprintf(out, outLen, "~%lu days left", (unsigned long)(minutes / (24 * 60)));
  } else if (minutes >= 90) {
    snprintf(out, outLen, "~%lu h left", (unsigned long)(minutes / 60));
  } else {
    snprintf(out, outLen, "~%lu min left", (unsigned long)minutes);
  }
}
```

- [ ] **Step 2: Use it in `render()`**

Replace the `ready · ~%lu min left on card` branch:

```cpp
  } else if (services_.sdCard != nullptr && services_.sdCard->writable()) {
    char remain[32];
    formatRemaining(rec != nullptr ? rec->estimatedRemainingSec() : 0, remain,
                    sizeof(remain));
    snprintf(line, sizeof(line), "ready · %s", remain);
    gfx.setTextColor(theme::kTextDim);
  } else {
```

- [ ] **Step 3: Migrate the header and text to the type system**

Replace the hand-placed title:

```cpp
  const int16_t shiftX = services_.amoled->shiftX();
  const int16_t shiftY = services_.amoled->shiftY();
  const int16_t top = widgets::header(gfx, "Recorder", shiftX, shiftY);
```

Then replace the status line's four-call sequence:

```cpp
  // was: setTextSize / setTextColor / setCursor / print
  uint16_t statusColor = theme::kTextDim;
  if (recording) {
    statusColor = rec->paused() ? theme::kWarn : theme::kBad;
  } else if (services_.sdCard == nullptr || !services_.sdCard->writable()) {
    statusColor = theme::kWarn;
  }
  widgets::text(gfx, theme::kPadding, top + 4, line, widgets::TextStyle::Body, statusColor);
```

and the list caption:

```cpp
  widgets::text(gfx, theme::kPadding, y,
                totalRecordings_ > 0 ? "newest first (tap = play)" : "no recordings yet",
                widgets::TextStyle::Caption, theme::kTextDim);
```

and the STOP / REC button labels, which currently centre by a hardcoded pixel
offset (`recordRect_.x + w / 2 - 36`) that is wrong for a proportional face:

```cpp
  widgets::textCentered(gfx, recordRect_.x, recordRect_.y + 24, recordRect_.w,
                        recording ? "STOP" : "REC", widgets::TextStyle::Body,
                        recording ? theme::kBg : theme::kText);
```

Lay the transport controls out from `top` rather than the old `kTop` constant,
and keep the red dot before `REC` — it is the only color cue that the button
starts a recording rather than stopping one.

- [ ] **Step 4: Fix the delete-button placement**

Replace the hardcoded offset:

```cpp
    // Against the safe inset, not a magic 44 from the raw panel edge — the
    // corner radius eats the difference on the bottom rows.
    const int16_t delW = 44;
    const int16_t delX = DISPLAY_WIDTH - theme::kSafeInset - delW;
    rowRects_[i] = widgets::listItem(gfx, theme::kPadding, y,
                                     delX - theme::kPadding - 12, recordings_[i].name,
                                     secondary, false);
    rowDeleteRects_[i] = widgets::Rect{delX, static_cast<int16_t>(y + 6), delW, 40};
```

- [ ] **Step 5: Convert the footer**

```cpp
  if (totalRecordings_ > 0) {
    const size_t first = pageFirst_[page_] + 1;
    char footText[48];
    if (recordingCount_ > 0 && totalRecordings_ > recordingCount_) {
      snprintf(footText, sizeof(footText), "%u-%u of %u", (unsigned)first,
               (unsigned)(first + recordingCount_ - 1), (unsigned)totalRecordings_);
      widgets::footer(gfx, footText, "swipe up/down", shiftX, shiftY);
    } else {
      snprintf(footText, sizeof(footText), "%u recording%s", (unsigned)totalRecordings_,
               totalRecordings_ == 1 ? "" : "s");
      widgets::footer(gfx, footText, nullptr, shiftX, shiftY);
    }
  }
```

- [ ] **Step 6: Adjust `listTop()` and `rowsThatFit()` for the taller footer band**

`kFooterHeight` becomes `48` (the band moved from -28 to -44 plus caption
height). `rowsThatFit()` already subtracts it, so the row count self-corrects —
but verify on device that the last row is not clipped.

- [ ] **Step 7: Compile gate**

Run: `./scripts/build.sh`
Expected: clean.

- [ ] **Step 8: Commit**

```bash
git add little-cube-os/src/apps/RecorderApp.cpp
git commit -m "Recorder: type system, readable remaining time, safe-area rows"
```

**Device gate:** the `ready · ~N days left` line fits on one line; the delete
button does not crowd the name; the last list row is not clipped by the footer;
paging still works past the first screenful; **stop-recording still works and
Back during a take is still refused** — those behaviours must not regress.

---

## Task 12: Documentation and validation checklist

**Files:**
- Modify: `docs/hardware-validation.md`
- Modify: `technical.md`

- [ ] **Step 1: Add the checklist section to `docs/hardware-validation.md`**

Append, leaving **every box unchecked** — the file's rule is absolute and a
compile ticks nothing:

```markdown
## Screen polish (Clock faces, Today, Weather, News, Recorder, Assistant)

- [ ] All six clock faces render; tap cycles them; the face name shows in the footer
- [ ] Selected face survives a reboot (NVS persistence)
- [ ] Blinky / Big Eyes blink at an irregular cadence, not a metronome
- [ ] Mood Cube: star eyes on USB power, sleepy face after 22:00, droopy under 15%
- [ ] Clock still dims AND blanks on the normal screen timeout with a face open
- [ ] Weather tap cycles Now -> Forecast -> Details and wraps back
- [ ] Weather long-press refreshes from any view; the Details button also refreshes
- [ ] Per-day forecast glyphs match the conditions; no truncated condition text
- [ ] Weather offline vs no-location states still read differently
- [ ] Cached-before-restart weather still says so, in the warning color
- [ ] Today: cards do not overlap; weather glyph matches; battery row tracks a real charge
- [ ] Today: pulling the SD card shows a specific state, never a generic error
- [ ] Assistant: level meter moves with speech and falls smoothly
- [ ] Assistant: a long answer pages with swipe up/down; the last page is not clipped
- [ ] Assistant: Back during listening still cancels without sending
- [ ] News: the bottom-left footer is fully visible on BOTH the list and the detail
- [ ] News: the last detail page shows a QR, and a phone scans it to the right article
- [ ] News: an over-long link shows "link too long to encode", not a broken code
- [ ] Recorder: the remaining-time line fits on one line and reads sensibly
- [ ] Recorder: last list row is not clipped; delete button does not crowd the name
- [ ] Recorder: stop still works; Back during a take is still refused
- [ ] **Burn-in soak:** footers visibly drift over ~4 minutes on ALL six screens.
      This is the regression check for the shiftX/shiftY fix and cannot be
      verified any other way.
- [ ] Footer corner clearance: photograph a footer carrying BOTH strings and
      confirm neither is clipped. The ~19px caption clearance is exact, but
      "kSafeInset clears the bezel radius" is a working assumption — there is
      no measured corner-radius constant in the codebase. If the radius turns
      out smaller than assumed, the text inset can drop toward kPadding and
      merge with the rule's.
- [ ] Footer hint legibility on the LOW-CONTRAST palettes specifically: Matrix
      (kTextDim measures 3.50:1 against kBg) and Mint (3.97:1). The rest of the
      ten sit at 4.2:1 or better, so these two are where a quiet caption fails
      first.
- [ ] A footer whose left string is too long to fit ellipsizes rather than
      running into the corner, and the right-hand hint still renders
- [ ] Boots and renders all six screens with no SD card
- [ ] Boots and renders all six screens with no Wi-Fi
- [ ] Boots and renders all six screens with the clock unset
```

- [ ] **Step 2: Update `technical.md`**

The repository-layout section mentions `scripts/` and `docs/`; add the QR
library to the dependency story so a clean-machine setup is not surprised:

```markdown
Libraries are installed by `scripts/install-libraries.sh` and passed explicitly
to `arduino-cli` by `scripts/build.sh`. Because passing any `--library` disables
automatic discovery, a new dependency must be added in three places: that
script, the install script, and the `littlecube` profile in
`little-cube-os/sketch.yaml`. Current additions beyond the board stack: JPEGDEC
(video), QRCode (news article links).
```

- [ ] **Step 3: Full clean build, both paths**

```bash
./scripts/build.sh
```

```bash
arduino-cli compile --profile littlecube little-cube-os
```

Expected: both clean. The second is the only check that catches a `sketch.yaml`
omission.

- [ ] **Step 4: Commit**

```bash
git add docs/hardware-validation.md technical.md
git commit -m "Docs: screen polish validation checklist and QR dependency note"
```

- [ ] **Step 5: Flash and hand over**

```bash
./scripts/upload.sh
```

Then walk the checklist above **on the device**. If the serial console is silent
after flashing, unplug and replug — that is the documented `hwcdc` /
`CDCOnBoot=cdc` re-enumeration quirk, not a bad build. Report which items pass
and which do not; tick nothing that was not observed.

---

## Risks

**`widgets::measureBlock()` is new surface (Task 9).** It refactors the existing
`textBlock(TextStyle)` to share one wrap walker. Every screen already using
`textBlock` — Files, Contacts, Notes, Settings, Calendar, Reader, Video — is
affected. If wrapping changes anywhere, this is the cause. Check a Notes body
and a News detail after Task 9, not just the Assistant.

**Flash growth.** JPEGDEC already made this build large and the faces add
drawing code. The QR encoder costs nothing new — it ships with the core and is
already in the default `ld_libs`. Measured so far: 1,925,375 bytes at Task 1,
1,926,775 at Task 3, i.e. **11% of a 16 MB flash**. There is no realistic
pressure here; the earlier concern about the partition overflowing was
unfounded.

**`fillEllipse` availability (Task 6).** If this Arduino_GFX version lacks it,
substitute scaled `fillCircle` calls rather than adding a library.

**Nothing in this plan may call `AmoledProtection::keepAwake()`.** That path
exists for video playback. An animated clock face that holds the panel awake is
precisely the spec §37 burn-in case, and the user explicitly chose to respect
the timeout.
