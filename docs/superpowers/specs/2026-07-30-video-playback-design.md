# Video Playback (MJPEG) — Design

Date: 2026-07-30 · Status: approved (chat) · Owner: cypher

## Goal

Watch pre-transcoded TV shows and anime on the cube, off the SD card: a Video
app with a browsable episode library, resume positions, and folder-based
auto-advance. Video is Motion JPEG decoded frame-by-frame (the ESP32-S3 has no
hardware video decoder, so H.264/HEVC is impossible at any resolution); audio
is raw PCM through the existing ES8311 path. Episodes are produced on the
user's Mac by an ffmpeg-based packing script.

## Decisions (user-confirmed)

- **Orientation**: rotated landscape. Frames are pre-rotated by ffmpeg to
  252×448 and drawn filling the panel's full 448 px axis; the user turns the
  device sideways like a phone. Picture is ~35×20 mm vs ~29×16 mm letterboxed.
- **Source**: SD card only. No streaming, no Wi-Fi dependency, no Mac helper.
  Matches the "everything degrades" rule — video works with Wi-Fi off.
- **Subtitles**: none in v1. A real scope cut (subbed anime is unusable
  without them), accepted to ship sooner. The player loop is structured so a
  sidecar `.srt` renderer can be added later without touching the decoder.
- **Audio**: 22.05 kHz mono s16 PCM, no compression. The speaker is ~1 cm with
  no headphone jack; PCM costs zero decode CPU and feeds I2S directly.
- **App scope**: browse + resume + queue. Library list of `/littlecube/video/`,
  positions saved to NVS with "Resume at mm:ss" on reopen, auto-advance to the
  next episode in the folder on natural completion.
- **Container**: custom `.lcv` (option A over standard AVI or sibling files):
  purpose-built header + frame index gives clean seek/resume/duration; ffmpeg
  still does all real work (scale, rotate, JPEG encode, PCM extract) and a
  small muxer script staples the output together.
- **Placement**: primary carousel, after Audio.
- **Feature flag**: `FEATURE_VIDEO`, default on, compiles the whole subsystem
  out when 0 (new library dependency + largest single addition to the tree).

## Expectations (stated up front, not failures)

- ~15 fps at 252×448; frames drop under load, audio never stutters.
- ~300 MB per 22-minute episode at these settings.
- Battery: playback draws roughly 250–400 mA (AMOLED bright + both cores +
  SD); expect 30–45 min per charge. One episode ≈ one charge — this is why
  resume exists. Below 10 % battery the app warns before starting playback.
- Picture is postage-stamp sized (~35×20 mm). Watchable, not an iPad.

## Architecture

Four new units, one job each:

| Unit | Purpose | Depends on |
|---|---|---|
| `video/LcvReader.{h,cpp}` | Container parsing: header validation, chunk iteration, index lookups. No decoding, no display, no tasks. | SdStorage (open via sanitized path) |
| `video/VideoPlayer.{h,cpp}` | Playback engine: reader task, PSRAM frame ring, JPEG decode, A/V clock, play/pause/seek/stop. No UI. | LcvReader, AudioAdapter, DisplayAdapter |
| `services/VideoService.{h,cpp}` | Library scan (pull-only, like MusicService — no task, no update slot), resume-position store, queue ordering. | SdStorage, Preferences |
| `apps/VideoApp.{h,cpp}` | Library list + player chrome; input remapping for sideways use. | VideoPlayer, VideoService, Theme/StatusBar/AmoledProtection |

Plus:

- `serial/commands/VideoCommands.{h,cpp}` — `video list/play/pause/seek/stop/
  status/queue` family, wired into `SerialCommandService::handleLine()` + help.
- `scripts/pack-video.sh` + `scripts/lcv_mux.py` — Mac-side: ffmpeg transcode
  (scale, transpose, mjpeg q~7, 22.05 kHz mono s16le) piped into the Python
  muxer that writes the `.lcv`.
- `docs/lcv-format.md` — the container spec below, normative.
- `AppId::Video` + `kAppCount` bump + `appName()` + `AppRegistry` entry.
- New library **JPEGDEC** (ESP32-S3 SIMD paths), added in all three places per
  CLAUDE.md: `scripts/build.sh`, `scripts/install-libraries.sh`,
  `little-cube-os/sketch.yaml`.

### AudioAdapter change: externally-fed PCM mode

Today AudioAdapter owns file reading for every playback path. For video the
demuxer owns the file, so AudioAdapter gains a push-mode stream that owns only
codec + I2S + amp:

```cpp
bool     beginPcmStream(uint32_t sampleRate, uint8_t channels);
size_t   writePcm(const int16_t* samples, size_t count, uint32_t timeoutMs);
uint32_t pcmSamplesPlayed() const;   // 32-bit: atomic cross-task read
void     endPcmStream();
```

Same `PlayState` gate as every other path: video audio is playback, so it
cannot start while recording (half-duplex preserved), and `isPlaying()` holds
while a stream is open. `writePcm` blocks at most `timeoutMs` (bounded, called
from the reader task — never the loop). `pcmSamplesPlayed` counts samples
handed to I2S minus nothing — DMA depth is corrected on the consumer side.

## Threading & A/V sync

**One new FreeRTOS task** (reader, core 0), following the AudioAdapter worker
contract exactly: states owned by the loop task only; the task's last two
statements are `xSemaphoreGive(done)` then `vTaskDelete(nullptr)`; stops are
requests polled by the task; the task is never killed from outside.

Reader task loop: read next frame group from SD → push compressed JPEG into a
PSRAM ring (slot size = header `maxFrameBytes`, ~4–6 slots) → `writePcm()` the
group's audio with a bounded timeout. The I2S DMA buffers (~370 ms deep) *are*
the audio buffer — no second ring, no second task. SD reads therefore stay
~370 ms ahead of the speaker, which absorbs FATFS latency spikes.

**Audio is the master clock; video chases it.** On the loop task, per frame:

```
targetFrame = (pcmSamplesPlayed() - dmaDepthSamples) * fps / audioRateHz
```

Pop ring frames older than `targetFrame` (dropped, counted), decode the
current one, hold the last decoded frame if the ring is empty. Decode runs on
the loop task: ~10 ms JPEGDEC decode + ~5 ms PSRAM blit + ~16 ms QSPI flush ≈
31 ms against a 66 ms frame budget at 15 fps. Pause = stop consuming ring +
`pausePlayback`-style codec mute; the reader blocks naturally on the full ring
and full DMA.

## The `.lcv` container

64-byte little-endian header, then chunks, then a frame index at EOF:

```
off size field                off size field
 0   4   magic "LCV1"         20   4   frameCount
 4   2   version (1)          24   4   durationMs
 6   2   headerBytes (64)     28   4   indexOffset
 8   2   width  (252)         32   4   dataOffset
10   2   height (448)         36   4   maxFrameBytes
12   2   fps    (15)          40  24   reserved (zero)
14   2   audioChannels (1)
16   4   audioRateHz (22050)
```

- Chunks: 4-byte header (1-byte type: video/audio; 24-bit payload size), then
  payload, 4-byte aligned. A **frame group** = one video chunk (baseline JPEG,
  pre-rotated) + one audio chunk covering the frame's duration. At 15 fps,
  22050/15 = 1470 samples/frame exactly — integral, no drift accumulation.
- Index: `frameCount × uint32` file offsets to each frame group, at
  `indexOffset`. **Never loaded into RAM**: a seek reads 4 bytes at
  `indexOffset + N*4`. Sequential playback never touches it.
- `maxFrameBytes` sizes the ring slots at open; a frame chunk claiming more
  is a corruption signal.
- Open-time validation: magic, version, headerBytes, width/height within
  panel bounds, fps 1–30, audioRateHz sane, `dataOffset < indexOffset ≤
  fileSize`, `maxFrameBytes` ≤ 96 KB. Any failure → specific refusal reason
  surfaced in UI and `video status`.

## Rendering & input (sideways)

- Frame blit at `x=58, y=0` — 252 px wide, full 448 px tall; two 58 px strips
  read as above/below the picture once the device is turned.
- Chrome (title, scrub bar, time, battery) is rendered into a **448×58
  off-screen canvas** in normal landscape text orientation with existing Theme
  helpers, then transposed onto the main canvas (~26k px, ~1 ms) — only when
  chrome content changes. No rotated-text renderer.
- Chrome auto-hides after ~4 s of no input; while visible it offsets by
  `AmoledProtection::shiftX()/shiftY()` (spec §37 — it is the only static
  content on screen). Full-motion video itself is inherently burn-in-safe.
- `VideoApp` remaps input for the rotated frame: swipe axes rotate 90°; tap
  toggles chrome; visible controls (never gesture-only) for play/pause, stop,
  and back per spec §9. The BOOT button keeps its system-wide meaning
  (Short → Back, Long → Home — the only button the firmware receives; the
  second physical button is the AXP2101 power button, handled by the PMU).
  Corner-radius: chrome insets from panel corners (device corners are visibly
  rounded).

## Library, resume, queue (VideoService)

- **Library**: `list()` over `/littlecube/video/` (new `paths::kVideo`),
  `.lcv` only, same case-insensitive stable sort + keyset paging as
  MusicService. Duration read from each header (64-byte read per row, cached
  per listing pass). Subdirectories one level deep = "seasons"; the list shows
  folders first, then files.
- **Resume**: one NVS blob (`Preferences`, existing `littlecube` namespace),
  ≤ 32 records of `{crc32(path), positionMs, lastPlayedAt}` ≈ 384 bytes,
  LRU-evicted. Saved every ~5 s during playback and on stop/close. Positions
  < 30 s are not saved; ≥ 95 % marks the episode finished and clears the
  record. Reopening a file with a record offers "Resume at mm:ss / Start
  over".
- **Queue**: the folder is the playlist. On natural completion only (an
  end-of-file edge mirroring `lastPlayCompleted()` — a user stop never
  advances), play the next `.lcv` in the same directory in sort order. Player
  chrome gets next/previous-episode controls.

## Failure behaviour (everything degrades)

- No card → specific `SdCardState` message in the app, never generic.
- Card yanked mid-play → reader task flags the failed read and exits via its
  semaphore; loop task stops playback, calls `endPcmStream()`, shows
  `RemovedUnexpectedly`. Mirrors `abandonRecording()`.
- Bad/oversized frame chunk → skip, hold previous frame, count it; abort with
  a message after N (≈15) consecutive failures.
- Recording active → refuse to start ("recording in progress").
- Battery < 10 % → warning screen before starting ("~½ episode per charge").
- Resume record for a deleted file → dropped silently on next library scan.

## Transcode pipeline (Mac side)

`./scripts/pack-video.sh input.mkv [output.lcv]`:

1. ffmpeg pass: `-vf "scale=-2:252:force_original_aspect_ratio=decrease,
   pad=448:252:(ow-iw)/2:0,transpose=1,fps=15" -c:v mjpeg -q:v 7` → frames;
   `-ac 1 -ar 22050 -f s16le` → audio. (Exact filter chain finalized during
   implementation; requirement: 252×448 pre-rotated baseline JPEGs, 15 fps,
   22.05 kHz mono s16le.)
2. `lcv_mux.py` interleaves frame groups, writes header + index, computes
   `maxFrameBytes` and `durationMs`.
3. Output copied to the card under `/littlecube/video/<Show>/`.

## Testing

No host harness exists; per repo rules a clean compile proves nothing works.
Hardware validation additions (`docs/hardware-validation.md`):

- Pack a real episode; verify header fields with `lcv_mux.py --inspect`.
- On device: library lists it with correct duration; plays with lip-sync
  (audio-master check: cover the SD reader's throughput by playing a
  high-motion scene and confirm frames drop while audio stays clean).
- Pause/resume, seek via scrub bar, stop, reopen → resume offer at the right
  position; finish an episode → auto-advance to the next file.
- Card-yank during playback → clean stop + specific state, no hang, recorder
  still works afterwards.
- `video status` over serial during playback reports fps achieved, frames
  dropped, ring depth.
- Recording refusal both directions (video during record, record during
  video).

Serial verification commands: `video list`, `video play <path>`, `video
pause`, `video seek <mm:ss>`, `video stop`, `video status`, `video queue`.

## Out of scope (v1)

Subtitles (structured for later via sidecar `.srt`), Wi-Fi streaming, on-device
transcoding of foreign formats, playback speeds, thumbnails in the library,
brightness-per-app, landscape support in any other app.
