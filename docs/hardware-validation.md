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

- [ ] Microphone input tested (recording captures actual sound)
- [ ] Speaker output tested (tone/beep audible)
- [ ] Recording tested (10 s -> valid .wav on SD, plays on a computer)
- [ ] Audio playback tested (recorded WAV plays through the speaker)
- [ ] Volume control tested

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
