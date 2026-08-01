# Hardware Validation Checklist

**Rule: no item may be checked without physical testing on the actual
cube.** Compile success is not hardware validation. Check items only after
performing the step on the device and observing the described result.

## Toolchain

- [ ] Arduino CLI core installed (`esp32:esp32@3.3.8`)
- [ ] Correct FQBN confirmed (`esp32s3` + 16M/opi/hwcdc/cdc/custom)
- [ ] Sketch compiles (`./scripts/build.sh`)
- [ ] Firmware uploads (`./scripts/upload.sh`, incl. 1200-baud re-enumeration)
- [ ] Serial monitor connects (`./scripts/monitor.sh`, boot banner visible)

## Display

- [ ] Display initializes (boot splash renders)
- [ ] Display dimensions confirmed (368x448, content reaches all edges)
- [ ] Frame canvas allocated in PSRAM (`getFreePsram()` drops ~322 KB — logged at boot)
- [ ] Brightness control tested (sweep visibly dims; 0 turns panel off)
- [ ] Screen timeout tested (dims, then off; touch/BOOT wakes)
- [ ] Pixel shift tested (status bar/clock drift a few px over minutes)

## Touch & input

- [ ] Touch coordinates confirmed (tap targets match finger position; identity mapping)
- [ ] Swipe detection tested (all four directions)
- [ ] Long press tested (~650 ms)
- [ ] Double tap tested
- [ ] BOOT button short press = Back, long press = Home

## Wi-Fi

- [ ] Wi-Fi scan tested (`wifi scan` lists nearby networks)
- [ ] Wi-Fi connection tested (correct password connects; state = connected)
- [ ] Wrong password reports AuthenticationFailed (not a generic error)
- [ ] Unknown SSID reports NetworkNotFound
- [ ] Connected-without-internet state distinct (probe fails, state says so)
- [ ] Setup access point tested (LittleCube-XXXX appears on a phone)
- [ ] Setup portal tested (192.168.4.1 loads; scan/select/password/save works)
- [ ] Serial Wi-Fi provisioning tested (`wifi connect "<ssid>"` + password prompt)
- [ ] Saved-network reconnection tested (reboot -> auto-reconnect)
- [ ] Offline mode disables the radio

## SD card

- [ ] SD insertion detected
- [ ] SD mount tested (state = mounted; `/littlecube/` tree created)
- [ ] SD read tested (note listing + open)
- [ ] SD write tested (serial note -> file on card)
- [ ] Safe eject tested (`storage eject`; card removable without corruption)
- [ ] Removal while idle tested (state = removed unexpectedly; no crash)
- [ ] Removal during write/recording tested (clean stop; recovery metadata; no crash)
- [ ] Full-card behavior tested (recording stops cleanly with reason)
- [ ] Read-only card detected

## Audio

- [x] Microphone input tested (recording captures actual sound — voice takes verified on-device and on a computer, 2026-07-26)
- [ ] Speaker output tested (tone/beep audible)
- [x] Recording tested (10 s -> valid .wav on SD, plays on a computer; 12 s takes pulled via `files dump`, CRC-verified, clean spectrum — 2026-07-26)
- [x] Audio playback tested (recorded WAV plays through the speaker — confirmed by ear, 2026-07-26)
- [ ] Volume control tested
- [ ] Radio station presets load from `/littlecube/radio/stations.txt`
- [ ] Radio France test streams connect and play audible MP3 audio
- [ ] Radio pause, resume, stop, and station switching tested
- [ ] Radio ICY metadata/status appears when provided by the station
- [ ] Radio reconnect tested after a temporary network drop
- [ ] Radio stops when Wi-Fi enters offline/no-internet mode
- [ ] Starting a recording while radio is active is refused cleanly

## Assistant (voice AI)

- [x] `assistant ask` end-to-end: chat + TTS + speaker playback, 4 consecutive
      exchanges, internal heap stable at 55 KB free (2026-07-26)
- [x] Multi-turn memory: follow-up questions answered with history 1→4 (2026-07-26)
- [x] STT on live speech (user-verified voice exchange, 2026-07-26)
- [x] Empty/unintelligible audio reports "didn't catch that", not a crash (2026-07-26)
- [ ] Full tap-talk-listen loop on the fixed firmware (talk button, Back-cancel)
- [ ] Error surfaces on-screen in the Assistant app (not only over serial)
- [ ] Offline / missing-key / no-SD refusals show their specific messages
- [ ] Notes voice flow: record -> transcribe -> review -> save creates a readable `.md` note on SD
- [ ] Notes voice flow: discard leaves no text note and keeps the WAV available in Recorder

## Card safety (async eject + mid-write removal)

- [ ] Record, then pull the card mid-write: no freeze, no crash, no reboot
- [ ] Same test: recovery metadata written to LittleFS (`/recovery_recording.json`)
- [ ] Same test: state reported specifically (`removed unexpectedly`), never generic
- [ ] `storage eject` while a recording is running (recording stops, then "safe to remove")
- [ ] `storage eject` while a WAV is playing (playback stops, then "safe to remove")
- [ ] `storage eject` returns immediately; `[sd] safe to remove the card` arrives after
- [ ] After eject the card is NOT silently remounted 5 s later (insert poll stays off)
- [ ] `storage mount` after an eject remounts with the card still in the slot
- [ ] Full card still lists (`files list`, `notes list`, `recordings list`)
- [ ] Full card still allows deletion (`files delete <path> confirm` frees space)

## Power loss

- [ ] Yank power mid-note-write; the note survives as itself, not as a `.bak`
- [ ] Yank power mid-recording; boot recovers, recording marked incomplete, no crash
- [ ] Yank power mid-settings-write; settings load with sane values (no boot loop)

## AMOLED protection (spec §37)

- [ ] Screen dims at `screenTimeoutSec` (not before, not at boot)
- [ ] Screen goes dark ~10 s after the dim step
- [ ] Tap wakes the screen WITHOUT actuating the control under the finger
- [ ] Second tap after a wake DOES actuate normally (swallow does not latch)
- [ ] A finger resting on the screen does not blank it (idle clock held)
- [ ] Always-on prevents blanking but still dims
- [ ] Screen timeout "never" neither dims nor blanks
- [ ] Bedtime caps brightness inside the window and releases outside it
- [ ] Bedtime window works across midnight (22:00 -> 07:00 default)
- [ ] Bedtime never raises brightness above the stored setting
- [ ] Chrome (status bar, clock digits) visibly drifts over ~4 minutes
- [ ] Brightness +/- in Settings takes effect within a frame (no fight with dimming)

## Navigation lifecycle

- [ ] Settings -> Setup Mode -> open Clock -> back shows Wi-Fi, not stale AP instructions
- [ ] Same sequence: the setup AP is confirmed GONE from a phone's network list
- [ ] Same sequence ending in Home (long press) also kills the AP
- [ ] Recorder list refreshes on back (new recording appears without reopening)
- [ ] Nine serial `open` commands, then `back` repeatedly: coherent chain down to Home
- [ ] `back` at Home is harmless (stays at Home, no crash, no empty screen)

## Provisioning security

- [ ] The setup network stays up (does not vanish seconds after appearing)
- [ ] On-screen AP password differs between two consecutive setup sessions
- [ ] AP password never appears in the serial log
- [ ] With an AP named `"><script>alert(1)</script>` in range, the portal renders
      it as literal text (no script runs, form not rewritten)
- [ ] Portal shuts down after a successful save
- [ ] Portal times out on inactivity

## Persistence & reliability

- [ ] Boots without Wi-Fi
- [ ] Boots without SD card
- [ ] Notes persist across reboot
- [ ] Settings persist across reboot
- [ ] Alarm persistence tested (once alarms land)
- [ ] Reboot recovery tested (`reboot` over serial; device returns to Home)
- [ ] Unexpected power loss tested (yank power mid-write; device recovers, file marked incomplete)

## Milestone demo (spec §48, run end-to-end)

- [ ] Cold boot -> Home carousel -> insert SD -> read note -> serial
      multiline note -> reboot -> note persists -> phone Wi-Fi setup ->
      weather updates -> record voice note -> play it back

## Screen polish — foundation (Tasks 1-4)

**2026-07-31 session.** Flashed the foundation build (`widgets::footer()`,
weather glyphs, QR wrapper, persisted clock face) and exercised it over serial.

⚠️ **Nothing visual is verified by this session, because nothing visual
changed.** All four foundation pieces are still uncalled — the linker
garbage-collects `footer()`, `qrcode::draw()` and the glyph mapper while no
screen references them. The device renders exactly as it did before. The
footer geometry, the `kSafeInset` bezel assumption, and the glyph shapes all
remain unverified until Task 5 puts a face on screen.

- [x] Firmware boots after the foundation work; `version` answers
      `Little Cube OS 0.1.0`, `uptime` counts from reset (2026-07-31)
- [x] `settings list` includes `clockface`, picked up from `kKeys[]` rather
      than hand-printed (2026-07-31)
- [x] `settings set clockface 3` → `reboot` → `settings get clockface` returns
      `3`. NVS persistence confirmed on device, not inferred (2026-07-31)
- [x] `settings set clockface 9` is refused with
      `usage: settings set clockface <0-5>` and leaves the stored value
      untouched — enumerated keys reject rather than clamp (2026-07-31)
- [ ] The unchanged-value early return actually suppresses an NVS write
      (needs an erase-count probe or a long soak; the serial round-trip above
      cannot distinguish a skipped write from a performed one)

## Screen polish — clock faces (Task 5)

**2026-07-31, photographed on device.**

- [x] **`kSafeInset = 20` clears the bezel corner radius.** The footer's left
      caption and right hint are both fully legible at the bottom of the
      panel, unclipped. This was the load-bearing prose assumption behind
      every footer on all six screens, and it had never been tested — there is
      still no measured corner-radius constant in the codebase, but the
      geometry is now confirmed adequate at this inset.
- [x] Footer renders with a hairline rule, left status and right hint, and
      neither string truncates at the widths in use
- [x] `kTextDim` footer text is legible on the **Matrix** palette — the worst
      of the ten at 3.50:1 against `kBg`. The earlier `kPanelAlt` text on the
      same screen was invisible; this is the fix confirmed.
- [x] Tap cycles faces; the name updates in the footer each tap
- [x] Digital, Stacked and Words all render; Words wraps rather than clipping
      (`"quarter past twelve"` fits one line in `Title`)
- [x] Blinky / Big Eyes / Mood Cube fall back to Digital while showing their
      own name, as designed pending Task 6
- [ ] Face selection survives a reboot **through the UI** (tap to cycle, then
      reboot) — the serial round-trip is verified above, the tap path is not
- [ ] Screen still dims and blanks on the normal timeout with a face open

## Screen polish — ASCII faces, Weather, Today, News (Tasks 6-8, 10)

**2026-07-31, all confirmed on the device.**

- [x] Four ASCII clock faces render: Block, Prompt, Segment, Binary. The
      Segment face is the notable one — the built-in font's `|` (0x7C) is a
      **broken bar** (rows 3 and 7 blank), so a seven-segment vertical spanning
      two stacked cells would have rendered as four dashes. Substituting
      `0xB3`, the only full-height solid column in the table, reads correctly
      on glass.
- [x] Weather renders as `wttr.in`-style ASCII art with tap-cycled
      Now/Forecast/Details views
- [x] Today renders as a `neofetch`-style system readout
- [x] **News QR scans with a phone and opens the correct article.** This is
      the first on-device execution of `qrcode::draw()` and everything behind
      it: the ESP-IDF encoder bundled with the Arduino core (no third-party
      library), the encoded-code cache, the `bg` luminance guard, and the 2 px
      minimum module size. A stripped BBC URL lands on QR version 3 at 7 px per
      module in a 260 px box.
- [x] News footers clear the bezel on **both** the list and the detail views —
      this was the user's originally reported "cut off on bottom left"
- [ ] The `link too long to encode` fallback — not reachable in practice
      (`Headline::url` is 160 bytes, well under the v17 cap of 644), so it is
      contract-only and untested
- [ ] Sub-zero temperatures agree across Weather, Today and the Home carousel
      (the `roundC` consolidation) — needs a freezing day or a forced snapshot

## Video (FEATURE_VIDEO)

Prereq: at least one real `.lcv` on the card under `/littlecube/video/`
(`./scripts/pack-video.sh episode.mkv`), verified `OK` by
`scripts/lcv_mux.py inspect` on the computer first.

**2026-07-31 session** — packed a real 23-minute 4:3 anime episode from `.mp4`
and played it on the cube. Observed over serial during playback:
`252x448@15fps shown=57 dropped=0 ring=4` at 0:03, `shown=111 dropped=2` at
0:07 — ~14 fps sustained against the 15 fps target, ring staying full (SD
read-ahead has headroom). Codec came up at 22050 Hz. Items below checked from
that session; unchecked items were not exercised.

- [x] `video list` over serial shows the file with the correct duration
      (`23:10  HXH 1999 Dub EP. 1.lcv`)
- [x] Library UI lists it; tap starts playback; device turned sideways shows
      the picture upright and filling the panel's long axis (4:3 source is
      letterboxed inside the frame by ffmpeg's pad — expected, not a fault)
- [x] Colors correct (if red/blue swap: flip the `setPixelType` endianness
      in `VideoPlayer.cpp::decodeFrame`) — no swap; RGB565_LITTLE_ENDIAN is
      correct for this panel
- [x] Chrome reads the same way up as the picture (if upside-down: apply the
      transpose flip noted in `VideoApp.cpp::renderChrome`) — transpose
      handedness correct as written
- [ ] Lip-sync: dialogue matches mouths after 5+ minutes of playback
      (audio-master check; tune `kDmaDepthSamples` in `VideoPlayer.h` if
      video leads/trails)
- [ ] High-motion scene: frames drop (`video status` dropped counter rises)
      while audio stays clean and unbroken. If video-side stalls appear,
      `kRingSlots` may be raised to 8 (must divide 256; ~2x PSRAM) — raise it
      BEFORE raising `kDmaDepthSamples` past 5880 (see the invariant in
      `VideoPlayer.h`)
- [ ] Natural end: expect the last ~3 frames to snap (dropped by design as
      the clock runs out) and ~370 ms of audio tail to drain after the
      screen returns; an auto-advance splices the next episode behind that
      tail (documented v1 limitation)
- [ ] Pause: instant silence + frozen frame; resume continues in sync
- [ ] Seek via scrub bar and `video seek +60`; picture + audio land together
      (~370 ms of pre-seek audio drains across the splice — documented)
- [ ] Stop, reopen the episode: resume offer at the right position
- [ ] Watch an episode to the end: position record cleared, next `.lcv` in
      the folder auto-plays; the LAST file in a folder returns to the library
- [ ] Serial `video play` while the app is open on the library: adopts to
      the player screen within a tick
- [x] `video status` reports sane fps/shown/dropped/ring during playback
- [x] Volume: the chrome's `-`/`+` buttons and left/right swipes both change
      level, and `vol NN%` appears in the readout for ~1.5 s
- [ ] Recording refusal both directions (`video play` during a recording;
      recording start during playback)
- [ ] Card yank mid-play: specific error state, no hang; recorder still
      works afterwards; reinserted card lists again
- [ ] Battery < 10 %: warning screen appears before playback starts
- [ ] Back (BOOT short) in the player exits to the library; long-press Home
      leaves the app, playback stops, position saved
- [ ] Chrome auto-hides within ~4 s in all states (playing and paused); leave
      the cube on Home 10+ min after watching: no chrome burn-in artifacts
- [ ] Screen stays lit for a full episode with no touches (keepAwake path);
      pausing lets it dim/blank normally
- [ ] Tapping a corrupt/truncated .lcv shows its specific refusal reason as a
      toast
- [x] 4:3 source packs to 310x414 and fills noticeably more of the panel
      than a 16:9 episode; picture never runs under the chrome strip
      (2026-07-31: a 480x360 season repacked to 310x414 — user-confirmed
      "screen full size now", ~1.5x the visible area of the padded 252x448)

## Screen polish — Recorder and Assistant (Tasks 9, 11)

**2026-07-31.**

- [x] Recorder status line fits on one line and reads sensibly — was
      `ready · ~16274 min left on ca` running 372 px on a 368 px panel, now
      `ready - ~11 days left` at 246 px against 344 px available
- [x] **Non-ASCII glyphs purged from on-screen strings.** The `FreeSans*`
      faces contain only 0x20-0x7E, so the `·` in the Recorder's status line
      rendered as garbage — the stray character visible in the user's first
      photograph. The same middot was in on-screen strings in the Home
      carousel, Audio, Video, Settings and Notes, and `Carousel.cpp` also
      carried a `°`, so `26°C · Clear` was drawing two garbage glyphs on the
      most-seen screen. All replaced with ASCII.
- [x] Assistant renders and operates correctly on device (2026-07-31)
- [x] **A long assistant answer pages with swipe up/down without truncating.**
      This also validates the shared wrap-walker refactor behind it: pagination
      and drawing now go through one implementation, verified beforehand
      against the pre-refactor code over 40,320 cases (40 strings x 4 styles x
      28 widths x 9 line caps) with zero mismatches. That refactor is used by
      FilesApp, ContactsApp, NotesApp, SettingsApp, CalendarApp, ReaderApp,
      VideoApp and NewsApp, so a regression would have been device-wide.
- [ ] Assistant idle card shows the correct specific blocker (`no API key` /
      `offline` / `no SD card` / `ready`) for each of those states
- [ ] Assistant level meter tracks a real voice and decays (~2.5 s from full)
- [ ] `Back` during listening still cancels the take without sending
- [ ] Recorder: last list row is not clipped by the taller footer band
- [ ] Recorder: a long filename ellipsizes rather than running under the
      delete button (`REC_20260731_235959.wav` measures 303 px against a
      264 px row)

### Not verifiable without specific conditions

- [ ] Sub-zero temperatures agree across Weather, Today and the Home carousel
      (the `roundC` consolidation) — needs a freezing day or a forced snapshot
- [ ] `takeLivePeak()` does not disturb `recordPeak_`: record a quiet note and
      confirm normalization still boosts it, and that the serial
      `mic peak N/32767` report is unchanged. Verified by inspection
      (`git diff` on `AudioAdapter.cpp` is three pure additions and
      `RecorderService.cpp` is not in the changeset), not on hardware.
- [ ] Burn-in soak: footers visibly drift over ~4 minutes on all six screens

## Assistant — Responses API migration (2026-08-01)

Verified on device over serial, with a key configured and Wi-Fi up.

- [x] `POST /v1/responses` with `gpt-5.6-luna` returns 200 and parses. This
      validates the three request fields that could not be confirmed against
      any reachable spec beforehand: `max_output_tokens`, `reasoning:
      {"effort": "none"}`, and `tool_choice: "auto"` alongside a built-in
      `web_search` tool. None was rejected.
- [x] The `output` array walk finds the answer text. The migration doc's own
      example shows a `{"type":"reasoning"}` item FIRST, before the message,
      so naive `output[0]` parsing would have returned empty — the
      skip-non-message walk handles it.
- [x] Search-triggering question produces a larger body than a conversational
      one (4183 vs 3362 bytes), consistent with the tool engaging.
- [x] **Internal heap holds steady across repeated exchanges.** Largest free
      block settles after the first exchange and then holds:
      43 KB -> 24 -> 24 -> 24, free 67 -> 65 -> 64 -> 64 KB. This is the check
      that matters: the documented prior failure was a CONTINUOUS decline
      (69 -> 52 -> 49, then esp-aes allocation failures). Sinking the response
      body into PSRAM rather than an internal-RAM String is what this
      confirms.
- [x] A request while a reply is still playing is refused specifically
      (`assistant busy or no key`), not generically
- [ ] `response too large` path — not reachable with real answers (~3-4 KB
      against a 64 KB buffer); contract-only
- [x] Spoken (microphone) path works on the new endpoint — tap-talk-send
      through STT, Responses and TTS (user-confirmed, 2026-08-01)
- [ ] `store: false` actually suppresses server-side retention — the request
      field is sent and the exchange still succeeds, but retention is
      OpenAI-side and cannot be observed from the device. Confirm in the
      platform dashboard if it matters.
- [ ] Whether web search materially improves answer accuracy for current-events
      questions, vs the model answering from parametric knowledge
