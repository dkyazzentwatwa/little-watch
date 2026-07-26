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

## Assistant (voice AI)

- [x] `assistant ask` end-to-end: chat + TTS + speaker playback, 4 consecutive
      exchanges, internal heap stable at 55 KB free (2026-07-26)
- [x] Multi-turn memory: follow-up questions answered with history 1→4 (2026-07-26)
- [x] STT on live speech (user-verified voice exchange, 2026-07-26)
- [x] Empty/unintelligible audio reports "didn't catch that", not a crash (2026-07-26)
- [ ] Full tap-talk-listen loop on the fixed firmware (talk button, Back-cancel)
- [ ] Error surfaces on-screen in the Assistant app (not only over serial)
- [ ] Offline / missing-key / no-SD refusals show their specific messages

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
