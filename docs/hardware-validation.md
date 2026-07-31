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
