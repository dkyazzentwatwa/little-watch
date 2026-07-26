# USB Serial Interface

The cube is the display; USB serial is the keyboard (spec §16–§18). Every
command below is implemented in `little-cube-os/src/serial/` and dispatches
through a service — no command touches hardware or the filesystem directly.

> **Verification status:** the command surface compiles and is wired, but the
> serial families have not been re-run end-to-end on hardware since the
> reliability remediation (async eject, audio gate/reaper, LittleFS mount fix).
> Treat behaviour described here as *implemented*, not *validated* —
> `docs/hardware-validation.md` is the record of what has actually been tested.

## Connecting

```bash
./scripts/monitor.sh          # 115200 baud, needs LITTLECUBE_PORT
```

Any terminal works (`screen /dev/cu.usbmodem101 115200`, `picocom`, the
Arduino IDE monitor). The console is available from boot (`CDCOnBoot=cdc`);
the boot banner ends with `boot: ready`.

The console is also where the system prints unsolicited status lines —
`[sd] ...`, `[setup] ...`, `[wifi] ...`. These interleave with command
output; they are not responses to what you typed.

## Grammar

```
family verb arg arg ...
```

- Tokens split on spaces. A token may be double-quoted to contain spaces:
  `wifi connect "Home Network"`.
- Some verbs take the **whole remainder** of the line as one argument
  (`notes rename 3 My title`, `recordings rename 1 "Standup notes"`,
  `open <app>`); surrounding quotes are stripped, trailing spaces trimmed.
- A family with no verb prints that family's help.
- Unknown family or verb prints a specific error naming what was unknown and
  pointing at `help` / `help <family>`.
- **Lines are capped at 255 characters.** A longer line is discarded whole
  with `error: line too long (max 255 chars); ignored` — it is never executed
  as a truncated command.
- Parsing is non-blocking: `update()` drains whatever bytes are available each
  frame and only acts on a completed line. `\r` is ignored, so CRLF terminals
  are fine.
- Every accepted line publishes `SerialCommandReceived` on the event bus.

## Families

`help` with no argument prints the index. `help <topic>` prints the full
listing for `notes`, `files`, `storage`, `wifi`, `recordings`, `audio`,
`calendar`, `contacts` and `settings`. Other topics report that no detailed
help exists yet.

### general

| Command | Effect |
|---|---|
| `help [topic]` | command index, or a family's detailed help |
| `status` | firmware/version, uptime, free heap + PSRAM, Wi-Fi state, SD state (+ free MB when mounted), battery, clock |
| `version` | `Little Cube OS <version>` |
| `uptime` | seconds since boot |
| `reboot` | flushes serial, then `ESP.restart()` |
| `open <app>` | open an app by name, case-insensitive |
| `back` | router back step; prints the app it landed on |
| `input debug <on\|off>` | log raw touch/gesture decisions from `InputAdapter` |

`open` accepts: Home, Today, Clock, Weather, News, Calendar, Notes, Reader,
Recorder, Audio, Files, Contacts, Calculator, Settings. An unknown name prints the
full list rather than a generic error.

`open` and `back` drive the same router the touchscreen does, so
`onOpen`/`onClose` and `onPause`/`onResume` fire exactly as they would from a
gesture — this is the intended way to exercise the navigation lifecycle.

### time

| Command | Effect |
|---|---|
| `time` | current date/time, suffixed `(not set)` when the clock is unvalidated |
| `time set YYYY-MM-DD HH:MM[:SS]` | manual RTC set; seconds optional |

`time` reports `RTC unavailable` if the PCF85063 does not answer.

### notes

Ids are 1-based positions in the **last `notes list` output**. Any command
taking `<id|path>` also accepts a full path.

| Command | Effect |
|---|---|
| `notes list` | ids, `[pin]`/`[*]` markers, title, size, path |
| `notes show <id\|path>` | print a note (capped at 16 KB, truncation stated) |
| `notes new` | create `note-YYYYMMDD-HHMMSS.md` and open multiline capture |
| `notes write <filename>` | overwrite/create a note (multiline); `.md` appended if no `.md`/`.txt` extension |
| `notes append <id\|path>` | append to an existing note (multiline) |
| `notes rename <id> "<title>"` | rename via slugified title |
| `notes favorite <id> on\|off` | star / unstar |
| `notes pin <id> on\|off` | pin to the top of lists |
| `notes delete <id> confirm` | delete; without `confirm` it only prints what would go |
| `notes export <id>` | copy into `/littlecube/exports` |
| `notes import <path>` | copy a file into `/littlecube/notes/text` |

Bare names go under `/littlecube/notes/text/`; a name containing `/` is
sanitized as a full path first. Commands needing the card report the specific
`SdCardState` (`error: SD card read-only`, `... not present`) rather than a
generic failure.

### files

Every path is run through `SdStorage::sanitizePath()`. `..`, backslashes,
control characters and anything outside `/littlecube` or the
`/cypher-puter/desk` deck-interop root are refused with
`error: invalid path (must stay inside /littlecube or /cypher-puter/desk)`.

| Command | Effect |
|---|---|
| `files list [path]` | directory listing, default `/littlecube` |
| `files tree [path]` | tree, 3 levels deep |
| `files cat <path>` | print a text file, capped at 8 KB |
| `files mkdir <path>` | create a directory |
| `files copy <src> <dst>` | copy a file |
| `files move <src> <dst>` | move / rename |
| `files rename <src> <newname>` | rename in place; `newname` may not contain `/` |
| `files delete <path> confirm` | delete a file or empty directory |

### storage

| Command | Effect |
|---|---|
| `storage status` / `storage usage` | card state + total/used/free MB (identical output) |
| `storage mount` | retry the mount, walking the full 25/20/10/4 MHz speed ladder |
| `storage eject` | **asynchronous** safe eject — see below |
| `storage index` | (re)create the `/littlecube` tree |
| `storage backup` | prints that it is not implemented in v1 |

**`storage eject` is asynchronous.** It no longer refuses while something is
writing and it does not unmount inside the command. It returns immediately
with:

```
stopping writers and unmounting — wait for 'safe to remove the card'
```

The adapter then latches the state, publishes `SdRemoved` so recorder and
playback stop and close their files, and unmounts only once every writer has
gone quiet (or a 3 s deadline expires). Confirmation arrives later, unprompted,
on the same console:

```
[sd] safe to remove the card
```

Do not pull the card before that line. After a user eject the adapter stops
polling for insertion entirely — it will not silently remount the card five
seconds later. `storage mount` is the way back.

### wifi

| Command | Effect |
|---|---|
| `wifi scan` | async scan; results print when complete |
| `wifi connect "<ssid>"` | connect; password prompted on the next line |
| `wifi connect "<ssid>" open` | connect to an open network, no prompt |
| `wifi connect "<ssid>" hidden` | hidden SSID, password prompted |
| `wifi status` | state, current SSID + RSSI + IP, saved-network count, offline mode |
| `wifi disconnect` | drop the connection |
| `wifi forget "<ssid>"` | remove a saved network |
| `wifi offline on\|off` | radio kill-switch |

SSIDs longer than 32 characters are rejected as usage errors.

#### The password prompt

`wifi connect "<ssid>"` arms a one-shot prompt: the **next line you send is
treated as a password**, not as a command. The cube prints an honest warning
first, then `Password:`.

What the firmware guarantees, and what it does not:

- The password is never echoed by the cube, never printed, never logged, and
  never written to the SD card. It goes straight to `WifiService::connectTo()`.
- Both the line buffer and the stored SSID are `memset` to zero immediately
  after use — on the success path *and* on the overflow path.
- **Your terminal may still show what you type.** The cube cannot control
  local echo on the host side. This is stated in the prompt rather than
  papered over.
- Credentials are saved in **NVS, which is not encrypted on this board**.
  Anyone with physical access and `esptool read_flash` can read them back.
  Also stated in the prompt.
- A password line over 255 characters is not truncated and used: the prompt is
  cancelled, buffers are wiped, and the console prints
  `password entry cancelled`.

## Multiline capture

`notes new`, `notes write <file>` and `notes append <id|path>` switch the
console into capture mode. While capture is active **every line goes into the
buffer** — the command dispatcher is not consulted, so you cannot run a
command until you end the capture.

```
Enter text.
Type .END on a line by itself to save.
Type .CANCEL to discard. (.PREVIEW / .CLEAR also work)
```

Control lines are case-sensitive, must be alone on their line, and are never
stored:

| Line | Effect |
|---|---|
| `.END` | save and leave capture |
| `.CANCEL` | discard everything and leave capture |
| `.PREVIEW` | echo what has been captured so far (first 4 KB) |
| `.CLEAR` | throw away the buffer and keep capturing from empty |

Text streams to `/littlecube/cache/.serial-input.tmp` as it arrives — RAM
holds one line at a time — so a long paste cannot exhaust memory. Total input
is capped at 64 KB; exceeding it ends the capture with an error.

On `.END`:

- **Overwrite mode** — any existing target is renamed to `<target>.bak`, the
  temp file is renamed into place, then the `.bak` is dropped. A failure at
  the rename step restores the `.bak`.
- **Append mode** — the temp file is streamed onto the end of the target, then
  removed.

Success prints `Saved: <path> (<n> B)`; `.CANCEL` prints `Discarded.`

### When a save fails

**Only `.CANCEL` deletes your text.** Every failure path — missing target
directory, card pulled mid-write, full card, a write error during capture —
deliberately leaves the temp file on disk and tells you where it is:

```
Not saved. Your text is still in /littlecube/cache/.serial-input.tmp — copy it
out before the next write.
```

Recover it with `files cat /littlecube/cache/.serial-input.tmp`. The next
multiline capture reuses and clears that path, so copy it out first.

## audio / recordings / volume

Ids are 1-based positions in the most recent `list` output of that family.

### recordings

| Command | Effect |
|---|---|
| `recordings list` | id, name, size in KB, estimated duration |
| `recordings info <id>` | path + format (`WAV mono 16-bit @ 16000 Hz`) |
| `recordings rename <id> "<name>"` | rename inside `/littlecube/recordings`; `.wav` appended if missing; no `/` allowed |
| `recordings delete <id> confirm` | delete; `confirm` required |

### audio

| Command | Effect |
|---|---|
| `audio list` | playable files (WAV recordings in v1), marks the current track |
| `audio play <id\|path>` | play a WAV; a token containing `/` is treated as a path and sanitized |
| `audio pause` / `audio resume` | pause / resume playback |
| `audio stop` | stop playback |
| `audio next` / `audio previous` | step through the last listing; past either end prints `end of list` |
| `volume` | print the current volume |
| `volume <0-100>` | set volume (persists to NVS and applies to the codec) |

`volume` is its own family, not `audio volume` — the token after `volume` is
the value.

Playback start is a **request**: `requestPlayWavFile()` asks any running
player to stop and starts the new file once the old task has been reaped on a
later frame. Nothing blocks the loop waiting for a task to die, and no task is
ever force-deleted (that would strand the FATFS volume mutex). Consequently
`audio play` can fail with `error: playback failed (recording active? file
gone?)` while a recording is in progress.

**Scope of the serial `audio` family: WAV recordings only.** `audio list` and
`audio play` operate on the recordings under `/littlecube/recordings` via
`RecorderService::list()`. Music and podcast episodes (MP3/AAC/FLAC/WAV) play
through the **Audio app on the device** (its Music and Podcasts screens,
decoded by ESP8266Audio) — there is no serial verb to start them. Fetch podcast
episodes onto the card with the `podcast` serial family (`help podcast`); copy
music into `/littlecube/music` yourself. Radio (internet streaming) is not
implemented.

### calendar

Reads and writes the per-day JSON files under `/littlecube/calendar/`. Ids are
1-based positions in the **last `calendar list` output** (which is a single
day's agenda). A `<day>` argument is `today` (default), `tomorrow`,
`yesterday`, a relative `+N`/`-N`, or an absolute `YYYY-MM-DD`.

| Command | Effect |
|---|---|
| `calendar list [<day>]` | that day's agenda, with ids for the verbs below |
| `calendar show <id>` | full details of one event |
| `calendar next` | next upcoming event, searching the next 7 days |
| `calendar add <day> <HH:MM\|-> "<title>" ["<note>"]` | create an event; `-` for an untimed / all-day entry |
| `calendar done <id> [on\|off]` | mark complete / dismissed (default `on`) |
| `calendar delete <id> confirm` | delete an event; `confirm` required |
| `calendar export [<day>]` | copy the day file into `/littlecube/exports` |
| `calendar import <path> [<day>] [confirm]` | copy a JSON day file into the calendar |

When the clock is unset, "today" is unknown; give an explicit `YYYY-MM-DD` day
(the on-device app shows the same no-clock state). `calendar import` overwrites
an existing day file, so it takes `confirm`.

### contacts

Reads and writes `/littlecube/contacts/contacts.json`. Favourites live in
LittleFS (`/contacts_flags.json`), never on the SD card, so starring a contact
does not rewrite the address book. Ids are 1-based positions in the **last
`contacts list` / `contacts search` output**.

| Command | Effect |
|---|---|
| `contacts list` | all contacts, favourites first |
| `contacts search <text>` | case-insensitive match on name (scans the whole file, not just the cached window) |
| `contacts show <id>` | full record (name, phone, email, note, voice) |
| `contacts add "<name>" ["<phone>"] ["<email>"] ["<note>"]` | create a contact; only the name is required |
| `contacts set <id> <field> <value>` | edit one field: `name`, `phone`, `email`, `note`, or `voice` |
| `contacts favorite <id> on\|off` | star / unstar |
| `contacts delete <id> confirm` | delete a contact; `confirm` required |
| `contacts export` | copy `contacts.json` into `/littlecube/exports` |
| `contacts import <path> confirm` | replace the whole address book; `confirm` required |

`contacts set <id> voice <path>` runs the path through `sanitizePath()` before
storing it, like any other user-supplied path.

### settings

Reads and writes the typed settings in NVS (the same values the Settings app
edits). Values out of range are clamped and the **stored** value is printed
back. Wi-Fi credentials are not here — see `wifi`.

| Command | Effect |
|---|---|
| `settings list` | every setting and its current value, plus `weather.city` and the active theme |
| `settings get <key>` | one value |
| `settings set <key> <value>` | change one value |
| `settings themes` | list the 10 palettes (five dark, five light) with the active one marked |
| `settings bedtime <on\|off> [HH:MM HH:MM] [brightness]` | the nightly dim window |

Keys for `get` / `set`:

| Key | Value |
|---|---|
| `brightness` | 16–255 (0 would blank the panel, so it is floored) |
| `timeout` | screen-off seconds (5–3600), or `0` / `never` |
| `alwayson` | `on\|off` — keep the screen lit while powered |
| `devicename` | up to 32 characters |
| `timezone` | POSIX TZ, e.g. `GMT0BST,M3.5.0/1,M10.5.0` or `UTC0` |
| `volume` | 0–100 (also pushed to the codec, like the `volume` family) |
| `theme` | `0`–`9` or a name (e.g. `Paper`) — `settings themes` lists them |
| `weather.city` | a town name, e.g. `London` — geocoded to coordinates over Wi-Fi |
| `bedtime` / `bedtimestart` / `bedtimeend` / `bedtimebrightness` | the bedtime window's toggle, ends and brightness ceiling |

Two behaviours worth calling out:

- **`settings set timezone <tz>` takes effect without a reboot.** It publishes
  `SystemEvent::SettingsChanged`, which `TimeService` handles by re-applying the
  zone (`setenv`/`tzset`) and re-deriving the RTC — the on-screen clock shifts
  within the frame instead of at the next power cycle.
- **`settings bedtime` is the only editor anywhere for the bedtime window's
  start, end and brightness.** The Settings app exposes only the on/off toggle;
  the times and the nightly brightness ceiling can be changed *only* over
  serial (or `settings set bedtimestart` / `bedtimeend` / `bedtimebrightness`).
  With no arguments, `settings bedtime` prints the current window.

## Not implemented yet

`storage backup` is recognised but explicitly unimplemented in v1 — it prints
that it is not implemented and points at the product-spec follow-ups. Every
other family listed above is implemented and dispatches through a service.

## Safety properties

- Command length capped at 255 chars/line; multiline capped at 64 KB — malformed
  input cannot exhaust memory (spec §43).
- Every user-supplied path is sanitized; traversal outside the allowed roots is
  refused (spec §44).
- Destructive verbs (`notes delete`, `files delete`, `recordings delete`) require
  a literal `confirm` token and echo the resolved path first.
- Nothing in the serial path blocks the frame: scans, connects, mounts, playback
  and eject are all requests completed by later frames.
- Serial commands are never exposed over the network.
