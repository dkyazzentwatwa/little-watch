# Video Playback (MJPEG) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Play pre-transcoded TV/anime episodes (`.lcv` files: MJPEG video + raw PCM audio) off the SD card, with a library browser, resume positions, and folder auto-advance, per `docs/superpowers/specs/2026-07-30-video-playback-design.md`.

**Architecture:** A custom `.lcv` container is produced on the Mac by ffmpeg + a Python muxer. On device: `LcvReader` parses the container; `VideoPlayer` runs one reader task (SD → PSRAM frame ring → I2S audio) and decodes JPEG frames on the loop task, chasing an audio-master clock; `VideoService` handles library/resume/queue; `VideoApp` is the UI (portrait library, rotated-landscape player). Everything is behind `FEATURE_VIDEO`.

**Tech Stack:** Arduino CLI, esp32 core 3.3.8, JPEGDEC (new dependency), ESP_I2S (existing), GFX Library for Arduino 1.6.6 (existing), ffmpeg + Python 3 (Mac side only).

**Read first:** `docs/superpowers/specs/2026-07-30-video-playback-design.md` (the contract for this plan), `CLAUDE.md` (build rules — Arduino CLI only, every library passed explicitly), `little-cube-os/src/hardware/audio/AudioAdapter.h` (the worker-task contract this plan mirrors).

**Verification reality:** there is no unit-test harness — this is on-device firmware. Per repo rules, each task's check is a clean `./scripts/build.sh` compile, and compiling proves only that it builds. Task 2 (host-side tooling) has real runnable tests. Hardware validation happens at the end via `docs/hardware-validation.md` and is NEVER checked off without physical testing on the cube.

**Environment for every compile step:**

```bash
export LITTLECUBE_FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=custom"
```

Expected output of a good build ends with `[build] output: <repo>/dist`.

---

### Task 1: Foundation — flag, path, dependency, format doc

**Files:**
- Modify: `little-cube-os/src/feature_flags.h`
- Modify: `little-cube-os/src/storage/StoragePaths.h`
- Modify: `little-cube-os/src/storage/SdStorage.cpp` (the `kTree[]` array, ~line 11)
- Modify: `scripts/build.sh`, `scripts/install-libraries.sh`, `little-cube-os/sketch.yaml`
- Create: `docs/lcv-format.md`

- [ ] **Step 1: Add the feature flag** in `feature_flags.h`, after `#define FEATURE_CAMERA 0`:

```cpp
// Video playback (docs/superpowers/specs/2026-07-30-video-playback-design.md).
// Compiles out the whole subsystem: LcvReader, VideoPlayer, VideoService,
// VideoApp, the `video` serial family, and every JPEGDEC use.
#define FEATURE_VIDEO 1
```

- [ ] **Step 2: Add the storage root.** In `StoragePaths.h` after `kRadioStations`:

```cpp
constexpr const char* kVideo = "/littlecube/video";
```

In `SdStorage.cpp`, add `paths::kVideo,` to the `kTree[]` array (after the radio entry, matching the existing order-by-area style).

- [ ] **Step 3: Install JPEGDEC and record its version:**

```bash
arduino-cli lib install "JPEGDEC"
arduino-cli lib list JPEGDEC
```

Note the installed version from the second command's output (e.g. `1.8.2`).

- [ ] **Step 4: Register the dependency in all three places** (CLAUDE.md rule):
  - `scripts/build.sh`: add `  --library "${ARDUINO_LIB_ROOT}/JPEGDEC" \` after the ESP8266Audio line.
  - `scripts/install-libraries.sh`: add `"${ARDUINO_CLI}" lib install "JPEGDEC@<version>"` after the ESP8266Audio line, with the version from Step 3.
  - `little-cube-os/sketch.yaml`: add `      - JPEGDEC (<version>)` to the libraries list.

- [ ] **Step 5: Write `docs/lcv-format.md`** — normative container spec:

````markdown
# .lcv — Little Cube Video container (v1)

Motion-JPEG video + raw PCM audio for on-device playback (design:
docs/superpowers/specs/2026-07-30-video-playback-design.md). Produced by
scripts/pack-video.sh; consumed by little-cube-os/src/video/LcvReader.
All integers little-endian.

## Layout

64-byte header, then frame-group chunks, then a frame index at EOF.

## Header (64 bytes)

| off | size | field | v1 value / rule |
|-----|------|-------|-----------------|
| 0   | 4    | magic | `LCV1` |
| 4   | 2    | version | 1 |
| 6   | 2    | headerBytes | 64 |
| 8   | 2    | width | pre-rotated frame width (252) |
| 10  | 2    | height | pre-rotated frame height (448) |
| 12  | 2    | fps | 1–30 (15) |
| 14  | 2    | audioChannels | 1 |
| 16  | 4    | audioRateHz | 22050; must be divisible by fps |
| 20  | 4    | frameCount | > 0 |
| 24  | 4    | durationMs | frameCount * 1000 / fps |
| 28  | 4    | indexOffset | > dataOffset, + 4*frameCount <= file size |
| 32  | 4    | dataOffset | >= 64 |
| 36  | 4    | maxFrameBytes | largest video payload; <= 98304 (96 KB) |
| 40  | 24   | reserved | zero |

## Chunks

4-byte chunk header: byte 0 = type (1 = video, 2 = audio), bytes 1–3 =
payload size, LE24. Payload follows, padded to a 4-byte boundary (padding
bytes are not counted in the size).

A **frame group** = one video chunk (one complete baseline JPEG, already
rotated 90° CW so it lands on the portrait panel with no runtime rotation)
followed by one audio chunk holding exactly audioRateHz/fps mono s16 samples
(the final group may be shorter; the muxer zero-pads it).

## Index

frameCount × uint32 LE at indexOffset: the file offset of each frame
group's video chunk header. Never loaded into RAM on device — a seek reads
4 bytes at indexOffset + 4*N.
````

- [ ] **Step 6: Compile** — `./scripts/build.sh`. Expected: clean build (nothing uses the flag yet), ends `[build] output: .../dist`.

- [ ] **Step 7: Commit**

```bash
git add little-cube-os/src/feature_flags.h little-cube-os/src/storage/StoragePaths.h little-cube-os/src/storage/SdStorage.cpp scripts/build.sh scripts/install-libraries.sh little-cube-os/sketch.yaml docs/lcv-format.md
git commit -m "Video: FEATURE_VIDEO flag, /littlecube/video root, JPEGDEC dependency, .lcv format doc"
```

---

### Task 2: Mac-side tooling — muxer + pack script (host-testable)

**Files:**
- Create: `scripts/lcv_mux.py`
- Create: `scripts/pack-video.sh`

- [ ] **Step 1: Write `scripts/lcv_mux.py`:**

```python
#!/usr/bin/env python3
"""Mux ffmpeg output into a .lcv container (docs/lcv-format.md).

mux:     concatenated-MJPEG stream + raw s16le mono PCM  ->  .lcv
inspect: print and sanity-check a .lcv header and index
"""
import argparse
import struct
import sys

MAGIC = b"LCV1"
VERSION = 1
HEADER_BYTES = 64
CHUNK_VIDEO = 1
CHUNK_AUDIO = 2
MAX_FRAME_BYTES = 96 * 1024


def split_jpegs(data: bytes):
    """Split a concatenated MJPEG stream on SOI/EOI markers.

    Inside JPEG entropy data every 0xFF is stuffed with 0x00, so a literal
    FFD9 is always a real end-of-image — splitting on it is safe.
    """
    frames = []
    i = 0
    while True:
        soi = data.find(b"\xff\xd8", i)
        if soi < 0:
            break
        eoi = data.find(b"\xff\xd9", soi + 2)
        if eoi < 0:
            sys.exit(f"error: frame {len(frames)}: unterminated JPEG at offset {soi}")
        frames.append(data[soi:eoi + 2])
        i = eoi + 2
    return frames


def chunk(ctype: int, payload: bytes) -> bytes:
    if len(payload) >= 1 << 24:
        sys.exit(f"error: chunk of {len(payload)} bytes exceeds LE24 size field")
    header = bytes([ctype]) + len(payload).to_bytes(3, "little")
    pad = (-len(payload)) % 4
    return header + payload + b"\x00" * pad


def mux(args):
    with open(args.video, "rb") as f:
        frames = split_jpegs(f.read())
    with open(args.audio, "rb") as f:
        pcm = f.read()
    if not frames:
        sys.exit("error: no JPEG frames found in video stream")
    if args.rate % args.fps != 0:
        sys.exit(f"error: rate {args.rate} not divisible by fps {args.fps}")
    max_frame = max(len(f) for f in frames)
    if max_frame > MAX_FRAME_BYTES:
        sys.exit(f"error: largest frame is {max_frame} B (> {MAX_FRAME_BYTES}); "
                 "raise -q:v (lower quality) and re-run ffmpeg")

    spf = args.rate // args.fps          # samples per frame group
    bpf = spf * 2                        # bytes per frame group (mono s16)
    out = bytearray(b"\x00" * HEADER_BYTES)
    index = []
    for n, jpg in enumerate(frames):
        index.append(len(out))
        out += chunk(CHUNK_VIDEO, jpg)
        a = pcm[n * bpf:(n + 1) * bpf]
        a += b"\x00" * (bpf - len(a))    # zero-pad the tail group
        out += chunk(CHUNK_AUDIO, a)
    index_offset = len(out)
    for off in index:
        out += struct.pack("<I", off)

    header = struct.pack(
        "<4sHHHHHHIIIIII24x",
        MAGIC, VERSION, HEADER_BYTES,
        args.width, args.height, args.fps, 1,
        args.rate, len(frames), len(frames) * 1000 // args.fps,
        index_offset, HEADER_BYTES, max_frame)
    assert len(header) == HEADER_BYTES
    out[:HEADER_BYTES] = header
    with open(args.output, "wb") as f:
        f.write(out)
    print(f"wrote {args.output}: {len(frames)} frames, "
          f"{len(frames) * 1000 // args.fps} ms, maxFrame {max_frame} B, "
          f"{len(out)} B total")


def inspect(args):
    with open(args.file, "rb") as f:
        data = f.read()
    if len(data) < HEADER_BYTES:
        sys.exit("error: file shorter than header")
    (magic, version, header_bytes, width, height, fps, channels, rate,
     frame_count, duration_ms, index_offset, data_offset,
     max_frame) = struct.unpack("<4sHHHHHHIIIIII24x", data[:HEADER_BYTES])
    print(f"magic={magic} version={version} {width}x{height} @{fps}fps "
          f"audio={rate}Hz ch={channels}")
    print(f"frames={frame_count} duration={duration_ms}ms "
          f"maxFrameBytes={max_frame} dataOffset={data_offset} "
          f"indexOffset={index_offset}")
    ok = (magic == MAGIC and version == VERSION and header_bytes == HEADER_BYTES
          and 0 < fps <= 30 and channels == 1 and rate % fps == 0
          and frame_count > 0 and data_offset >= HEADER_BYTES
          and index_offset + 4 * frame_count <= len(data)
          and 0 < max_frame <= MAX_FRAME_BYTES)
    # Every index entry must point at a video chunk header.
    for n in range(frame_count):
        off = struct.unpack_from("<I", data, index_offset + 4 * n)[0]
        if off + 4 > index_offset or data[off] != CHUNK_VIDEO:
            print(f"BAD index entry {n}: offset {off}")
            ok = False
            break
    print("OK" if ok else "INVALID")
    sys.exit(0 if ok else 1)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("mux")
    m.add_argument("--video", required=True)
    m.add_argument("--audio", required=True)
    m.add_argument("--fps", type=int, default=15)
    m.add_argument("--rate", type=int, default=22050)
    m.add_argument("--width", type=int, default=252)
    m.add_argument("--height", type=int, default=448)
    m.add_argument("-o", "--output", required=True)
    m.set_defaults(func=mux)
    i = sub.add_parser("inspect")
    i.add_argument("file")
    i.set_defaults(func=inspect)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Write `scripts/pack-video.sh`:**

```bash
#!/usr/bin/env bash
set -euo pipefail

# Transcode any video ffmpeg can read into a .lcv for the cube.
# Usage: ./scripts/pack-video.sh input.mkv [output.lcv]
# 448x252 letterboxed, rotated 90 CW (transpose=1) to 252x448 so frames land
# on the portrait panel with no runtime rotation; 15 fps; 22.05 kHz mono PCM.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IN="${1:?usage: pack-video.sh input.mkv [output.lcv]}"
OUT="${2:-${IN%.*}.lcv}"
FPS=15
RATE=22050
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

echo "[pack] video pass (mjpeg ${FPS} fps, rotated)"
ffmpeg -hide_banner -loglevel error -y -i "${IN}" \
  -vf "scale=w=448:h=252:force_original_aspect_ratio=decrease,pad=448:252:(ow-iw)/2:(oh-ih)/2,transpose=1,fps=${FPS}" \
  -c:v mjpeg -q:v 7 -pix_fmt yuvj420p -an -f image2pipe "${TMP}/video.mjpeg"

echo "[pack] audio pass (${RATE} Hz mono s16le)"
ffmpeg -hide_banner -loglevel error -y -i "${IN}" \
  -vn -ac 1 -ar "${RATE}" -c:a pcm_s16le -f s16le "${TMP}/audio.pcm"

python3 "${ROOT}/scripts/lcv_mux.py" mux \
  --video "${TMP}/video.mjpeg" --audio "${TMP}/audio.pcm" \
  --fps "${FPS}" --rate "${RATE}" --width 252 --height 448 -o "${OUT}"
python3 "${ROOT}/scripts/lcv_mux.py" inspect "${OUT}"
echo "[pack] done: ${OUT} — copy to the card under /littlecube/video/"
```

Then: `chmod +x scripts/pack-video.sh scripts/lcv_mux.py`

- [ ] **Step 3: Test with a synthetic clip** (this is the real, runnable test for this task):

```bash
cd "$(mktemp -d)"
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc=duration=3:size=640x360:rate=30" \
  -f lavfi -i "sine=frequency=440:duration=3" \
  -shortest -c:v libx264 -c:a aac test-input.mp4
<repo>/scripts/pack-video.sh test-input.mp4 test.lcv
```

Expected `inspect` output: `magic=b'LCV1' version=1 252x448 @15fps audio=22050Hz ch=1`, `frames=45 duration=3000ms`, ending `OK` (exit 0). If `frames` is 44–46 due to ffmpeg rounding, that is acceptable; `INVALID` or a traceback is a failure.

- [ ] **Step 4: Keep a real test file.** Run the same pack on an actual episode if one is handy (any `.mkv`/`.mp4`). Not required to proceed, but Task 12's hardware validation needs at least one `.lcv` on the SD card.

- [ ] **Step 5: Commit**

```bash
git add scripts/lcv_mux.py scripts/pack-video.sh
git commit -m "Video: pack-video.sh + lcv_mux.py transcode pipeline (Mac side)"
```

---

### Task 3: LcvReader — container parsing

**Files:**
- Create: `little-cube-os/src/video/LcvReader.h`
- Create: `little-cube-os/src/video/LcvReader.cpp`

- [ ] **Step 1: Write `LcvReader.h`:**

```cpp
#pragma once

#include <Arduino.h>

#include "../feature_flags.h"

#if FEATURE_VIDEO

#include <FS.h>

// .lcv container reader (docs/lcv-format.md). Pure parsing: header
// validation, sequential chunk iteration, index lookups. No decoding, no
// display, no tasks — VideoPlayer owns all of that. Instance methods are
// called from the reader task; the static readHeader() is safe anywhere.
struct LcvHeader {
  uint16_t width = 0;   // pre-rotated: 252
  uint16_t height = 0;  // pre-rotated: 448
  uint16_t fps = 0;
  uint16_t audioChannels = 0;
  uint32_t audioRateHz = 0;
  uint32_t frameCount = 0;
  uint32_t durationMs = 0;
  uint32_t indexOffset = 0;
  uint32_t dataOffset = 0;
  uint32_t maxFrameBytes = 0;
};

class LcvReader {
 public:
  enum class ChunkType : uint8_t { Video = 1, Audio = 2 };

  static constexpr uint32_t kMaxFrameBytes = 96 * 1024;

  // Reads and validates the 64-byte header of `path` (absolute, already
  // sanitized). On failure writes a short human-readable reason.
  static bool readHeader(const char* path, LcvHeader& out, char* reasonOut, size_t reasonLen);

  bool open(const char* path, char* reasonOut, size_t reasonLen);
  void close();
  bool isOpen() { return static_cast<bool>(file_); }
  const LcvHeader& header() const { return header_; }

  // Position the cursor at frame group N via the on-disk index: a single
  // 4-byte read at indexOffset + 4*N. The index is never loaded into RAM.
  bool seekToFrame(uint32_t frame);

  // Read the next chunk header. False at end of data (endOfData()) or on a
  // short/failed read.
  bool nextChunk(ChunkType& typeOut, uint32_t& sizeOut);
  // Read the current chunk payload (sizeBytes from nextChunk) into buf and
  // skip the alignment padding. buf must hold sizeBytes.
  bool readChunk(uint8_t* buf, uint32_t sizeBytes);
  bool skipChunk(uint32_t sizeBytes);
  // True when the cursor reached indexOffset cleanly — the natural end.
  bool endOfData() const { return eof_; }

 private:
  static bool parse(const uint8_t* raw, LcvHeader& out, uint32_t fileSize, char* reasonOut,
                    size_t reasonLen);

  fs::File file_;
  LcvHeader header_;
  bool eof_ = false;
};

#endif  // FEATURE_VIDEO
```

- [ ] **Step 2: Write `LcvReader.cpp`:**

```cpp
#include "LcvReader.h"

#if FEATURE_VIDEO

#include <SD_MMC.h>
#include <string.h>

#include "../board_config.h"

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void reason(char* out, size_t len, const char* msg) {
  if (out != nullptr && len > 0) {
    strncpy(out, msg, len - 1);
    out[len - 1] = '\0';
  }
}

}  // namespace

bool LcvReader::parse(const uint8_t* raw, LcvHeader& out, uint32_t fileSize, char* reasonOut,
                      size_t reasonLen) {
  if (memcmp(raw, "LCV1", 4) != 0) {
    reason(reasonOut, reasonLen, "not an .lcv file (bad magic)");
    return false;
  }
  if (rd16(raw + 4) != 1 || rd16(raw + 6) != 64) {
    reason(reasonOut, reasonLen, "unsupported .lcv version");
    return false;
  }
  out.width = rd16(raw + 8);
  out.height = rd16(raw + 10);
  out.fps = rd16(raw + 12);
  out.audioChannels = rd16(raw + 14);
  out.audioRateHz = rd32(raw + 16);
  out.frameCount = rd32(raw + 20);
  out.durationMs = rd32(raw + 24);
  out.indexOffset = rd32(raw + 28);
  out.dataOffset = rd32(raw + 32);
  out.maxFrameBytes = rd32(raw + 36);
  if (out.width == 0 || out.width > DISPLAY_WIDTH || out.height == 0 ||
      out.height > DISPLAY_HEIGHT) {
    reason(reasonOut, reasonLen, "frame size exceeds panel");
    return false;
  }
  if (out.fps == 0 || out.fps > 30 || out.audioChannels != 1 || out.audioRateHz < 8000 ||
      out.audioRateHz > 48000 || out.audioRateHz % out.fps != 0) {
    reason(reasonOut, reasonLen, "bad fps/audio parameters");
    return false;
  }
  if (out.frameCount == 0 || out.durationMs == 0 || out.dataOffset < 64 ||
      out.indexOffset <= out.dataOffset ||
      static_cast<uint64_t>(out.indexOffset) + 4ull * out.frameCount > fileSize) {
    reason(reasonOut, reasonLen, "corrupt .lcv layout");
    return false;
  }
  if (out.maxFrameBytes == 0 || out.maxFrameBytes > kMaxFrameBytes) {
    reason(reasonOut, reasonLen, "frames too large for playback");
    return false;
  }
  return true;
}

bool LcvReader::readHeader(const char* path, LcvHeader& out, char* reasonOut, size_t reasonLen) {
  fs::File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    reason(reasonOut, reasonLen, "cannot open file");
    return false;
  }
  uint8_t raw[64];
  const bool ok = f.read(raw, sizeof(raw)) == sizeof(raw);
  const uint32_t size = f.size();
  f.close();
  if (!ok) {
    reason(reasonOut, reasonLen, "file shorter than header");
    return false;
  }
  return parse(raw, out, size, reasonOut, reasonLen);
}

bool LcvReader::open(const char* path, char* reasonOut, size_t reasonLen) {
  close();
  file_ = SD_MMC.open(path, FILE_READ);
  if (!file_) {
    reason(reasonOut, reasonLen, "cannot open file");
    return false;
  }
  uint8_t raw[64];
  if (file_.read(raw, sizeof(raw)) != sizeof(raw) ||
      !parse(raw, header_, file_.size(), reasonOut, reasonLen)) {
    close();
    return false;
  }
  eof_ = false;
  return file_.seek(header_.dataOffset);
}

void LcvReader::close() {
  if (file_) {
    file_.close();
  }
  eof_ = false;
}

bool LcvReader::seekToFrame(uint32_t frame) {
  if (!file_ || frame >= header_.frameCount) {
    return false;
  }
  uint8_t raw[4];
  if (!file_.seek(header_.indexOffset + 4 * frame) || file_.read(raw, 4) != 4) {
    return false;
  }
  eof_ = false;
  return file_.seek(rd32(raw));
}

bool LcvReader::nextChunk(ChunkType& typeOut, uint32_t& sizeOut) {
  if (!file_ || eof_) {
    return false;
  }
  if (file_.position() >= header_.indexOffset) {
    eof_ = true;  // ran cleanly into the index: natural end of data
    return false;
  }
  uint8_t raw[4];
  if (file_.read(raw, 4) != 4) {
    return false;
  }
  const uint8_t type = raw[0];
  if (type != static_cast<uint8_t>(ChunkType::Video) &&
      type != static_cast<uint8_t>(ChunkType::Audio)) {
    return false;  // desynced — treated as a read failure by the caller
  }
  typeOut = static_cast<ChunkType>(type);
  sizeOut = static_cast<uint32_t>(raw[1]) | (static_cast<uint32_t>(raw[2]) << 8) |
            (static_cast<uint32_t>(raw[3]) << 16);
  return true;
}

bool LcvReader::readChunk(uint8_t* buf, uint32_t sizeBytes) {
  if (!file_ || file_.read(buf, sizeBytes) != sizeBytes) {
    return false;
  }
  const uint32_t pad = (4 - (sizeBytes % 4)) % 4;
  return pad == 0 || file_.seek(file_.position() + pad);
}

bool LcvReader::skipChunk(uint32_t sizeBytes) {
  if (!file_) {
    return false;
  }
  const uint32_t pad = (4 - (sizeBytes % 4)) % 4;
  return file_.seek(file_.position() + sizeBytes + pad);
}

#endif  // FEATURE_VIDEO
```

- [ ] **Step 3: Compile** — `./scripts/build.sh`. Expected: clean.

- [ ] **Step 4: Commit**

```bash
git add little-cube-os/src/video/
git commit -m "Video: LcvReader .lcv container parsing"
```

---

### Task 4: AudioAdapter — externally-fed PCM stream mode

**Files:**
- Modify: `little-cube-os/src/hardware/audio/AudioAdapter.h`
- Modify: `little-cube-os/src/hardware/audio/AudioAdapter.cpp`

- [ ] **Step 1: Declare the API.** In `AudioAdapter.h`, after the sleep-timer block (`sleepRemainingSec()`), add:

```cpp
  // ---- Externally-fed PCM stream (video audio) ----------------------------
  // The caller (VideoPlayer's reader task) owns the source and pushes
  // samples; this adapter owns only codec + I2S + amp. begin/end/pcmPause
  // run on the LOOP TASK; writePcm runs on the caller's worker task.
  // Gated like every playback path: begin refuses unless fully idle, and
  // every other play/record start refuses while a stream is open
  // (half-duplex preserved).
  bool beginPcmStream(uint32_t sampleRate, uint8_t channels);
  // Push mono s16 samples; expanded to the stereo slots I2S runs in. Blocks
  // on DMA backpressure (that backpressure IS the ~370 ms audio buffer);
  // returns early only when the stream is ended. NEVER call from the loop.
  size_t writePcm(const int16_t* samples, size_t count);
  // Total samples accepted so far — the A/V master clock. Reset by begin.
  uint32_t pcmSamplesPlayed() const { return pcmSamples_; }
  // Mutes the codec (instant silence regardless of DMA contents) and parks
  // writePcm; the ~370 ms already in DMA drains muted and is not replayed.
  void pcmPause(bool paused);
  bool pcmPaused() const { return pcmPaused_; }
  void endPcmStream();  // idempotent
  bool pcmStreamActive() const { return pcmActive_; }
```

And in the private members, next to the other volatiles:

```cpp
  volatile bool pcmActive_ = false;
  volatile bool pcmPaused_ = false;
  volatile uint32_t pcmSamples_ = 0;
```

Change the two inline state accessors in the header so the stream counts as playback:

```cpp
  bool isPlaying() const { return playState_ != PlayState::Idle || pcmActive_; }
  bool playbackIdle() const { return playState_ == PlayState::Idle && !pcmActive_; }
```

- [ ] **Step 2: Implement.** In `AudioAdapter.cpp`, add (near the other playback methods):

```cpp
// ---- Externally-fed PCM stream (video audio) --------------------------------

bool AudioAdapter::beginPcmStream(uint32_t sampleRate, uint8_t channels) {
  if (!ready_ || channels != 1) {
    return false;
  }
  if (playState_ != PlayState::Idle || recState_ != RecState::Idle || pcmActive_) {
    return false;
  }
  if (!ensureStarted(sampleRate)) {
    return false;
  }
  setPa(true);
  pcmSamples_ = 0;
  pcmPaused_ = false;
  pcmActive_ = true;
  return true;
}

size_t AudioAdapter::writePcm(const int16_t* samples, size_t count) {
  static constexpr size_t kChunkFrames = 256;
  int16_t stereo[kChunkFrames * 2];
  size_t done = 0;
  while (done < count && pcmActive_) {
    if (pcmPaused_) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    size_t n = count - done;
    if (n > kChunkFrames) {
      n = kChunkFrames;
    }
    for (size_t i = 0; i < n; i++) {
      stereo[2 * i] = samples[done + i];
      stereo[2 * i + 1] = samples[done + i];
    }
    i2s.write(reinterpret_cast<uint8_t*>(stereo), n * 2 * sizeof(int16_t));
    pcmSamples_ += n;
    done += n;
  }
  return done;
}

void AudioAdapter::pcmPause(bool paused) {
  pcmPaused_ = paused;
  if (started_) {
    Es8311::mute(paused);  // loop task — same cross-thread I2C rule as pausePlayback()
  }
}

void AudioAdapter::endPcmStream() {
  // Flags only: writePcm unblocks on !pcmActive_, and the normal idle path
  // in update() handles PA hold and I2S teardown. Leaving the codec muted
  // after a paused end is harmless — setPa(true) in the next start unmutes.
  pcmActive_ = false;
  pcmPaused_ = false;
}
```

- [ ] **Step 3: Gate every other start path.** Still in `AudioAdapter.cpp`:
  - In `playWavFile`, `playMusicFile`, `playRadio`, and `startRecordWav`, add immediately after their existing idle-state checks: `if (pcmActive_) { return false; }`
  - In `update()`, change the idle gate to include the stream:

```cpp
  if (playState_ != PlayState::Idle || recState_ != RecState::Idle || pcmActive_) {
    idleMs_ = 0;
    return;
  }
```

  - In `update()`, the `pendingPlay_` dispatch condition gains `&& !pcmActive_`.

- [ ] **Step 4: Compile** — `./scripts/build.sh`. Expected: clean.

- [ ] **Step 5: Commit**

```bash
git add little-cube-os/src/hardware/audio/
git commit -m "Audio: externally-fed PCM stream mode for video playback"
```

---

### Task 5: VideoPlayer — engine (ring, reader task, clock; no decode yet)

**Files:**
- Create: `little-cube-os/src/video/VideoPlayer.h`
- Create: `little-cube-os/src/video/VideoPlayer.cpp`

This task lands a fully working audio-plus-frame-transport engine: audio plays, frames flow through the ring and are popped/dropped by the A/V clock, but `decodeFrame()` only counts them (Task 6 adds JPEGDEC). That is a real, serial-verifiable intermediate — not a stub of the state machine.

- [ ] **Step 1: Write `VideoPlayer.h`:**

```cpp
#pragma once

#include <Arduino.h>

#include "../feature_flags.h"

#if FEATURE_VIDEO

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "LcvReader.h"

class AudioAdapter;
class DisplayAdapter;
class SdStorage;

// Playback engine. One reader task streams SD -> PSRAM frame ring + I2S
// audio; the loop task decodes JPEG frames chasing the audio clock.
//
// Task lifecycle contract — identical to AudioAdapter's workers:
//   * state_ is written by the LOOP TASK ONLY. It leaves a non-Idle state
//     only in update(), and only by taking done_.
//   * The reader task's last two statements are xSemaphoreGive(done_) then
//     vTaskDelete(nullptr). Nothing touches `self` after the give.
//   * Stops and seeks are REQUESTS; the task exits itself and is never
//     killed from outside.
class VideoPlayer {
 public:
  void begin(AudioAdapter* audio, DisplayAdapter* display, SdStorage* storage);

  // Starts playback (loop task only). Refuses — writing a short reason —
  // when already playing, recording is active, the path fails sanitizing,
  // or the header does not validate. startMs is rounded down to a frame.
  bool play(const char* path, uint32_t startMs, char* reasonOut, size_t reasonLen);
  void requestStop();                 // user stop; never counts as completed
  void requestSeek(int32_t deltaMs);  // relative, clamped to [0, duration)
  void setPaused(bool paused);
  bool paused() const { return paused_; }
  bool playing() const { return state_ != State::Idle; }
  bool idle() const { return state_ == State::Idle; }
  // True when the last playback reached the end of the file on its own.
  // Valid on the playing->idle edge; a requested stop clears it.
  bool completed() const { return completed_; }

  uint32_t positionMs() const;
  uint32_t durationMs() const { return header_.durationMs; }
  const char* path() const { return path_; }
  const LcvHeader& header() const { return header_; }

  // The app owns the screen; decode only happens while it says so. When
  // false, frames are still popped and dropped on the clock so audio and
  // position stay correct.
  void setUiActive(bool active) { uiActive_ = active; }

  // Diagnostics for `video status` and the chrome.
  uint32_t framesShown() const { return framesShown_; }
  uint32_t framesDropped() const { return framesDropped_; }
  uint8_t ringDepth() const { return static_cast<uint8_t>(head_ - tail_); }

  void update(uint32_t deltaMs);  // kernel slot: state machine + decode

 private:
  enum class State : uint8_t { Idle, Playing, Stopping };

  friend void videoReaderTask(void* arg);

  bool startTask(uint32_t startFrame);
  void finishPlayback();
  void consumeFrames();
  bool decodeFrame(uint8_t slot);  // Task 6 fills this in with JPEGDEC
  bool allocBuffers();
  void freeBuffers();

  static constexpr uint8_t kRingSlots = 5;
  static constexpr uint32_t kDmaDepthSamples = 4096;  // I2S depth estimate; tune on device
  static constexpr uint8_t kMaxConsecutiveBad = 15;

  AudioAdapter* audio_ = nullptr;
  DisplayAdapter* display_ = nullptr;
  SdStorage* storage_ = nullptr;

  State state_ = State::Idle;  // loop task only
  SemaphoreHandle_t done_ = nullptr;
  LcvReader reader_;           // instance methods used by the reader task only
  LcvHeader header_;
  char path_[160] = "";

  // SPSC ring: the reader task pushes (head_), the loop task pops (tail_).
  // Free-running uint8 indices; slot = index % kRingSlots.
  uint8_t* slots_[kRingSlots] = {nullptr};
  volatile uint32_t slotBytes_[kRingSlots] = {0};
  volatile uint32_t slotFrame_[kRingSlots] = {0};
  volatile uint8_t head_ = 0;
  volatile uint8_t tail_ = 0;
  int16_t* audioBuf_ = nullptr;  // one frame group of samples, PSRAM
  uint32_t audioBufBytes_ = 0;

  volatile bool stopReq_ = false;
  volatile bool taskEof_ = false;
  volatile bool taskFailed_ = false;
  bool taskDone_ = false;  // loop task: reader exited, ring may still drain
  bool taskRunning_ = false;

  bool paused_ = false;
  bool completed_ = false;
  volatile bool uiActive_ = false;
  bool seekPending_ = false;
  uint32_t seekTargetMs_ = 0;
  uint32_t taskStartFrame_ = 0;
  uint32_t baseFrame_ = 0;  // frame the current audio clock epoch started at

  uint32_t framesShown_ = 0;
  uint32_t framesDropped_ = 0;
  uint8_t consecutiveBad_ = 0;
};

#endif  // FEATURE_VIDEO
```

- [ ] **Step 2: Write `VideoPlayer.cpp`:**

```cpp
#include "VideoPlayer.h"

#if FEATURE_VIDEO

#include <esp_heap_caps.h>
#include <string.h>

#include "../board_config.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../storage/SdStorage.h"

namespace {

void reason(char* out, size_t len, const char* msg) {
  if (out != nullptr && len > 0) {
    strncpy(out, msg, len - 1);
    out[len - 1] = '\0';
  }
}

}  // namespace

// Reader task: stream frame groups from SD into the ring + I2S. Runs on
// core 0 so decode/render on the loop task never waits behind SD I/O.
void videoReaderTask(void* arg) {
  auto* self = static_cast<VideoPlayer*>(arg);
  LcvReader& r = self->reader_;
  uint32_t frame = self->taskStartFrame_;
  bool failed = false;

  while (!self->stopReq_) {
    LcvReader::ChunkType type;
    uint32_t size = 0;
    if (!r.nextChunk(type, size)) {
      failed = !r.endOfData();
      break;
    }
    if (type == LcvReader::ChunkType::Video) {
      if (size == 0 || size > self->header_.maxFrameBytes) {
        failed = true;  // size lies about the header contract: desynced
        break;
      }
      // Wait for a free slot; the ring being full is the normal steady state.
      while (static_cast<uint8_t>(self->head_ - self->tail_) >= VideoPlayer::kRingSlots &&
             !self->stopReq_) {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      if (self->stopReq_) {
        break;
      }
      const uint8_t s = self->head_ % VideoPlayer::kRingSlots;
      if (!r.readChunk(self->slots_[s], size)) {
        failed = true;
        break;
      }
      self->slotBytes_[s] = size;
      self->slotFrame_[s] = frame;
      self->head_ = self->head_ + 1;
      frame++;
    } else {  // Audio
      if (size == 0 || size > self->audioBufBytes_) {
        failed = true;
        break;
      }
      if (!r.readChunk(reinterpret_cast<uint8_t*>(self->audioBuf_), size)) {
        failed = true;
        break;
      }
      // Blocks on DMA backpressure — that backpressure is the pacing.
      self->audio_->writePcm(self->audioBuf_, size / sizeof(int16_t));
    }
  }

  self->taskEof_ = r.endOfData() && !failed;
  self->taskFailed_ = failed;
  xSemaphoreGive(self->done_);
  vTaskDelete(nullptr);
}

void VideoPlayer::begin(AudioAdapter* audio, DisplayAdapter* display, SdStorage* storage) {
  audio_ = audio;
  display_ = display;
  storage_ = storage;
  done_ = xSemaphoreCreateBinary();
}

bool VideoPlayer::allocBuffers() {
  for (uint8_t i = 0; i < kRingSlots; i++) {
    slots_[i] = static_cast<uint8_t*>(
        heap_caps_malloc(header_.maxFrameBytes, MALLOC_CAP_SPIRAM));
    if (slots_[i] == nullptr) {
      freeBuffers();
      return false;
    }
  }
  // One frame group of audio, with headroom for a short final group.
  audioBufBytes_ = (header_.audioRateHz / header_.fps) * sizeof(int16_t) + 64;
  audioBuf_ = static_cast<int16_t*>(heap_caps_malloc(audioBufBytes_, MALLOC_CAP_SPIRAM));
  if (audioBuf_ == nullptr) {
    freeBuffers();
    return false;
  }
  return true;
}

void VideoPlayer::freeBuffers() {
  for (uint8_t i = 0; i < kRingSlots; i++) {
    heap_caps_free(slots_[i]);
    slots_[i] = nullptr;
  }
  heap_caps_free(audioBuf_);
  audioBuf_ = nullptr;
  audioBufBytes_ = 0;
}

bool VideoPlayer::startTask(uint32_t startFrame) {
  head_ = 0;
  tail_ = 0;
  stopReq_ = false;
  taskEof_ = false;
  taskFailed_ = false;
  taskDone_ = false;
  taskStartFrame_ = startFrame;
  baseFrame_ = startFrame;
  if (startFrame > 0 && !reader_.seekToFrame(startFrame)) {
    return false;
  }
  if (xTaskCreatePinnedToCore(videoReaderTask, "vidread", 6144, this, 3, nullptr, 0) !=
      pdPASS) {
    return false;
  }
  taskRunning_ = true;
  return true;
}

bool VideoPlayer::play(const char* path, uint32_t startMs, char* reasonOut, size_t reasonLen) {
  if (state_ != State::Idle) {
    reason(reasonOut, reasonLen, "already playing");
    return false;
  }
  if (audio_ == nullptr || audio_->isRecording()) {
    reason(reasonOut, reasonLen, "recording in progress");
    return false;
  }
  String safe;
  if (storage_ == nullptr || !storage_->sanitizePath(path, safe)) {
    reason(reasonOut, reasonLen, "path refused");
    return false;
  }
  if (!reader_.open(safe.c_str(), reasonOut, reasonLen)) {
    return false;
  }
  header_ = reader_.header();
  if (!allocBuffers()) {
    reader_.close();
    reason(reasonOut, reasonLen, "out of memory");
    return false;
  }
  if (!audio_->beginPcmStream(header_.audioRateHz, 1)) {
    freeBuffers();
    reader_.close();
    reason(reasonOut, reasonLen, "audio busy");
    return false;
  }
  uint32_t startFrame = static_cast<uint32_t>(
      static_cast<uint64_t>(startMs) * header_.fps / 1000);
  if (startFrame >= header_.frameCount) {
    startFrame = 0;
  }
  if (!startTask(startFrame)) {
    audio_->endPcmStream();
    freeBuffers();
    reader_.close();
    reason(reasonOut, reasonLen, "cannot start reader");
    return false;
  }
  strncpy(path_, safe.c_str(), sizeof(path_) - 1);
  path_[sizeof(path_) - 1] = '\0';
  paused_ = false;
  completed_ = false;
  framesShown_ = 0;
  framesDropped_ = 0;
  consecutiveBad_ = 0;
  state_ = State::Playing;
  return true;
}

void VideoPlayer::requestStop() {
  if (state_ != State::Playing) {
    return;
  }
  seekPending_ = false;
  stopReq_ = true;
  if (paused_) {
    setPaused(false);  // un-park writePcm so the task can reach its exit
  }
  state_ = State::Stopping;
}

void VideoPlayer::requestSeek(int32_t deltaMs) {
  if (state_ != State::Playing) {
    return;
  }
  const int64_t target = static_cast<int64_t>(positionMs()) + deltaMs;
  seekTargetMs_ = target < 0 ? 0
                : target >= header_.durationMs ? header_.durationMs - 1
                                               : static_cast<uint32_t>(target);
  seekPending_ = true;
  stopReq_ = true;
  if (paused_) {
    setPaused(false);
  }
  state_ = State::Stopping;
}

void VideoPlayer::setPaused(bool paused) {
  if (state_ != State::Playing && !(state_ == State::Stopping && paused_)) {
    return;
  }
  paused_ = paused;
  audio_->pcmPause(paused);
}

uint32_t VideoPlayer::positionMs() const {
  if (state_ == State::Idle) {
    return 0;
  }
  const uint64_t baseMs = static_cast<uint64_t>(baseFrame_) * 1000 / header_.fps;
  const uint64_t audioMs =
      static_cast<uint64_t>(audio_->pcmSamplesPlayed()) * 1000 / header_.audioRateHz;
  const uint64_t pos = baseMs + audioMs;
  return pos > header_.durationMs ? header_.durationMs : static_cast<uint32_t>(pos);
}

void VideoPlayer::finishPlayback() {
  audio_->endPcmStream();
  reader_.close();
  freeBuffers();
  state_ = State::Idle;
}

void VideoPlayer::consumeFrames() {
  if (paused_ || static_cast<uint8_t>(head_ - tail_) == 0) {
    return;
  }
  // Audio is the master clock; subtract the DMA depth estimate so the frame
  // on screen matches what the speaker is saying, not what was buffered.
  const uint32_t played = audio_->pcmSamplesPlayed();
  const uint32_t dma = played > kDmaDepthSamples ? kDmaDepthSamples : played;
  const uint32_t target =
      baseFrame_ + static_cast<uint32_t>(static_cast<uint64_t>(played - dma) * header_.fps /
                                         header_.audioRateHz);
  // Drop everything older than the clock, keeping at least the newest.
  while (static_cast<uint8_t>(head_ - tail_) > 1 &&
         slotFrame_[tail_ % kRingSlots] < target) {
    tail_ = tail_ + 1;
    framesDropped_++;
  }
  const uint8_t s = tail_ % kRingSlots;
  if (slotFrame_[s] > target) {
    return;  // newest frame is still in the future; hold the current image
  }
  // The panel flush is capped ~30 fps; decoding while a flush is pending
  // would draw a frame that is overwritten before it is ever seen.
  if (uiActive_ && display_ != nullptr && !display_->flushPending()) {
    if (decodeFrame(s)) {
      framesShown_++;
      consecutiveBad_ = 0;
    } else {
      consecutiveBad_++;
      if (consecutiveBad_ >= kMaxConsecutiveBad) {
        requestStop();
        return;
      }
    }
    tail_ = tail_ + 1;
  } else if (!uiActive_) {
    tail_ = tail_ + 1;  // no screen: keep position/audio honest, drop video
    framesDropped_++;
  }
}

bool VideoPlayer::decodeFrame(uint8_t slot) {
  (void)slot;
  return true;  // Task 6 replaces this with the JPEGDEC decode
}

void VideoPlayer::update(uint32_t deltaMs) {
  (void)deltaMs;
  switch (state_) {
    case State::Idle:
      return;
    case State::Playing:
      if (taskRunning_ && xSemaphoreTake(done_, 0) == pdTRUE) {
        taskRunning_ = false;
        taskDone_ = true;
        if (taskFailed_) {
          // Card yanked or file corrupt mid-play: stop now, not completed.
          completed_ = false;
          finishPlayback();
          return;
        }
      }
      consumeFrames();
      if (taskDone_ && static_cast<uint8_t>(head_ - tail_) == 0) {
        completed_ = taskEof_;  // natural end: the ring drained after EOF
        finishPlayback();
      }
      return;
    case State::Stopping:
      if (taskRunning_ && xSemaphoreTake(done_, 0) == pdTRUE) {
        taskRunning_ = false;
        completed_ = false;
        if (seekPending_) {
          // Restart at the seek target, new audio-clock epoch.
          seekPending_ = false;
          audio_->endPcmStream();
          uint32_t frame = static_cast<uint32_t>(
              static_cast<uint64_t>(seekTargetMs_) * header_.fps / 1000);
          if (frame >= header_.frameCount) {
            frame = header_.frameCount - 1;
          }
          if (audio_->beginPcmStream(header_.audioRateHz, 1) && startTask(frame)) {
            state_ = State::Playing;
          } else {
            finishPlayback();
          }
        } else {
          finishPlayback();
        }
      }
      return;
  }
}

#endif  // FEATURE_VIDEO
```

- [ ] **Step 3: Compile** — `./scripts/build.sh`. Expected: clean. (`consumeFrames` and the task reference `kRingSlots` from outside the class via `VideoPlayer::kRingSlots`; if the compiler objects to the private static in the friend function, hoist `kRingSlots` to `public:` — it is a harmless constant.)

- [ ] **Step 4: Commit**

```bash
git add little-cube-os/src/video/
git commit -m "Video: VideoPlayer engine — reader task, PSRAM ring, audio-master clock"
```

---

### Task 6: VideoPlayer — JPEGDEC decode

**Files:**
- Modify: `little-cube-os/src/video/VideoPlayer.cpp`

- [ ] **Step 1: Add the decoder.** At the top of `VideoPlayer.cpp` (inside the `#if FEATURE_VIDEO`), add the include and file-scope decoder state:

```cpp
#include <JPEGDEC.h>
```

and in the anonymous namespace:

```cpp
// File-scope: JPEGDEC's state struct is large (~17 KB) and lives in .bss
// rather than on any stack. Decode runs on the loop task only.
JPEGDEC jpegDecoder;
Arduino_GFX* jpegTarget = nullptr;
int16_t jpegOffsetX = 0;

int jpegDrawBlock(JPEGDRAW* d) {
  jpegTarget->draw16bitRGBBitmap(d->x, d->y, d->pPixels, d->iWidth, d->iHeight);
  return 1;
}
```

(`Arduino_GFX` comes in via `DisplayAdapter.h`'s forward declaration; add `#include <Arduino_GFX_Library.h>` to `VideoPlayer.cpp` as well.)

- [ ] **Step 2: Replace the `decodeFrame` stub body:**

```cpp
bool VideoPlayer::decodeFrame(uint8_t slot) {
  Arduino_GFX* gfx = display_->canvas();
  if (gfx == nullptr) {
    return false;
  }
  jpegTarget = gfx;
  // Frames are pre-rotated by the packer; center on the panel's short axis.
  jpegOffsetX = static_cast<int16_t>((DISPLAY_WIDTH - header_.width) / 2);
  if (!jpegDecoder.openRAM(slots_[slot], static_cast<int>(slotBytes_[slot]),
                           jpegDrawBlock)) {
    return false;
  }
  // If colors come out wrong on device, switch to RGB565_BIG_ENDIAN — the
  // canvas framebuffer byte order is the only open question here.
  jpegDecoder.setPixelType(RGB565_LITTLE_ENDIAN);
  const int ok = jpegDecoder.decode(jpegOffsetX, 0, 0);
  jpegDecoder.close();
  if (ok != 1) {
    return false;
  }
  display_->markDirty();
  return true;
}
```

- [ ] **Step 3: Compile** — `./scripts/build.sh`. Expected: clean, with JPEGDEC found via the `--library` flag added in Task 1.

- [ ] **Step 4: Commit**

```bash
git add little-cube-os/src/video/VideoPlayer.cpp
git commit -m "Video: JPEGDEC frame decode into the frame canvas"
```

---

### Task 7: VideoService — library, resume, queue

**Files:**
- Create: `little-cube-os/src/services/VideoService.h`
- Create: `little-cube-os/src/services/VideoService.cpp`

- [ ] **Step 1: Write `VideoService.h`:**

```cpp
#pragma once

#include <Arduino.h>

#include "../feature_flags.h"

#if FEATURE_VIDEO

#include <Preferences.h>

class SdStorage;

// Video library + resume positions + folder queue. Pull-only like
// MusicService: no task, no update(deltaMs) slot — VideoApp and the serial
// family call in, nothing runs from the render path.
struct VideoInfo {
  char path[160] = "";
  char name[64] = "";
  uint32_t durationMs = 0;  // 0 for directories
  bool isDir = false;
};

class VideoService {
 public:
  void begin(SdStorage* storage);

  // Same contract as MusicService::list: sorted case-insensitively with an
  // exact-bytes tiebreak, keyset paging via `after`, totalOut = the TRUE
  // count. Directories sort before files ("seasons" one level deep).
  // Duration is read from each listed file's 64-byte header.
  size_t list(const char* dir, VideoInfo* out, size_t maxItems, size_t* totalOut = nullptr,
              const char* after = nullptr);

  // Next / previous .lcv sibling of currentPath in sort order. False at the
  // ends of the folder.
  bool nextInFolder(const char* currentPath, char* outPath, size_t outLen);
  bool prevInFolder(const char* currentPath, char* outPath, size_t outLen);

  // Resume store: one NVS blob of up to 32 {crc32(path), posMs, seq}
  // records, LRU-evicted by seq. Positions under 30 s are dropped;
  // >= 95 % of durationMs counts as finished and clears the record.
  uint32_t resumeMs(const char* path);  // 0 = start from the beginning
  void savePosition(const char* path, uint32_t posMs, uint32_t durationMs);
  void clearPosition(const char* path);

 private:
  struct ResumeRec {
    uint32_t pathCrc = 0;
    uint32_t posMs = 0;
    uint32_t seq = 0;
  };
  static constexpr size_t kMaxResume = 32;
  static constexpr uint32_t kMinSaveMs = 30000;

  void loadResume();
  void storeResume();
  int findResume(uint32_t crc) const;
  bool sibling(const char* currentPath, bool forward, char* outPath, size_t outLen);

  SdStorage* storage_ = nullptr;
  Preferences prefs_;
  bool prefsReady_ = false;
  bool resumeLoaded_ = false;
  ResumeRec resume_[kMaxResume];
  size_t resumeCount_ = 0;
  uint32_t seq_ = 0;
};

#endif  // FEATURE_VIDEO
```

- [ ] **Step 2: Write `VideoService.cpp`:**

```cpp
#include "VideoService.h"

#if FEATURE_VIDEO

#include <SD_MMC.h>

#include <ctype.h>
#include <string.h>

#include "../board_config.h"
#include "../hardware/SdCardAdapter.h"
#include "../storage/SdStorage.h"
#include "../storage/StoragePaths.h"
#include "../video/LcvReader.h"

namespace {

constexpr const char* kResumeKey = "vidresume";

// Same total order as MusicService: case-insensitive, exact bytes tiebreak.
int nameCompare(const char* a, const char* b) {
  const char* pa = a;
  const char* pb = b;
  for (;; ++pa, ++pb) {
    const unsigned char ca = static_cast<unsigned char>(*pa);
    const unsigned char cb = static_cast<unsigned char>(*pb);
    const int la = tolower(ca);
    const int lb = tolower(cb);
    if (la != lb) {
      return la - lb;
    }
    if (ca == '\0') {
      break;
    }
  }
  return strcmp(a, b);
}

bool sortsBefore(const char* a, const char* b) { return nameCompare(a, b) < 0; }

bool isLcvName(const char* name) {
  if (name == nullptr || name[0] == '.') {
    return false;  // hidden + AppleDouble entries
  }
  const char* dot = strrchr(name, '.');
  if (dot == nullptr) {
    return false;
  }
  const char* ext = dot + 1;
  return (tolower(static_cast<unsigned char>(ext[0])) == 'l' &&
          tolower(static_cast<unsigned char>(ext[1])) == 'c' &&
          tolower(static_cast<unsigned char>(ext[2])) == 'v' && ext[3] == '\0');
}

bool isDirName(const char* name) { return name != nullptr && name[0] != '.'; }

uint32_t crc32Path(const char* s) {
  uint32_t crc = 0xFFFFFFFFu;
  while (*s != '\0') {
    crc ^= static_cast<uint8_t>(*s++);
    for (int k = 0; k < 8; k++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (-(static_cast<int32_t>(crc & 1))));
    }
  }
  return ~crc;
}

}  // namespace

void VideoService::begin(SdStorage* storage) {
  storage_ = storage;
  prefsReady_ = prefs_.begin(PREF_NAMESPACE, false);
}

size_t VideoService::list(const char* dir, VideoInfo* out, size_t maxItems, size_t* totalOut,
                          const char* after) {
  if (totalOut != nullptr) {
    *totalOut = 0;
  }
  if (dir == nullptr || out == nullptr || maxItems == 0 || storage_ == nullptr) {
    return 0;
  }
  SdCardAdapter* card = storage_->card();
  if (card == nullptr || !card->mounted()) {
    return 0;
  }
  fs::File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) {
    if (d) {
      d.close();
    }
    return 0;
  }

  // Directories sort before files: prefix the sort key with 0/1. The key
  // buffer mirrors pageAnchors_ usage in VideoApp (after uses the same form).
  auto makeKey = [](bool isDir, const char* name, char* key, size_t keyLen) {
    key[0] = isDir ? '0' : '1';
    strncpy(key + 1, name, keyLen - 2);
    key[keyLen - 1] = '\0';
  };

  const bool paged = after != nullptr && after[0] != '\0';
  size_t total = 0;
  size_t count = 0;
  char keys[8][66];  // sort keys of the visible window; maxItems <= 8
  if (maxItems > 8) {
    maxItems = 8;
  }
  for (fs::File entry = d.openNextFile(); entry; entry = d.openNextFile()) {
    const char* name = entry.name();
    const bool dirEntry = entry.isDirectory();
    // Subfolders only at the top level: /littlecube/video/<Show>/ ("seasons
    // one level deep" — the design's library rule).
    const bool topLevel = strcmp(dir, paths::kVideo) == 0;
    const bool wanted = dirEntry ? (topLevel && isDirName(name)) : isLcvName(name);
    if (wanted) {
      total++;
      char key[66];
      makeKey(dirEntry, name, key, sizeof(key));
      if (!paged || sortsBefore(after, key)) {
        size_t pos = count < maxItems ? count : maxItems;
        while (pos > 0 && sortsBefore(key, keys[pos - 1])) {
          pos--;
        }
        if (pos < maxItems) {
          for (size_t j = (count < maxItems ? count : maxItems - 1); j > pos; j--) {
            out[j] = out[j - 1];
            memcpy(keys[j], keys[j - 1], sizeof(keys[0]));
          }
          VideoInfo& info = out[pos];
          snprintf(info.path, sizeof(info.path), "%s/%s", dir, name);
          strncpy(info.name, name, sizeof(info.name) - 1);
          info.name[sizeof(info.name) - 1] = '\0';
          info.isDir = dirEntry;
          info.durationMs = 0;
          memcpy(keys[pos], key, sizeof(keys[0]));
          if (count < maxItems) {
            count++;
          }
        }
      }
    }
    entry.close();
  }
  d.close();

  // Durations for the visible files only — one 64-byte header read per row.
  for (size_t i = 0; i < count; i++) {
    if (!out[i].isDir) {
      LcvHeader h;
      char why[8];
      if (LcvReader::readHeader(out[i].path, h, why, sizeof(why))) {
        out[i].durationMs = h.durationMs;
      }
    }
  }
  if (totalOut != nullptr) {
    *totalOut = total;
  }
  return count;
}

bool VideoService::sibling(const char* currentPath, bool forward, char* outPath,
                           size_t outLen) {
  if (currentPath == nullptr || storage_ == nullptr) {
    return false;
  }
  const char* slash = strrchr(currentPath, '/');
  if (slash == nullptr) {
    return false;
  }
  char dir[160];
  const size_t dirLen = static_cast<size_t>(slash - currentPath);
  if (dirLen == 0 || dirLen >= sizeof(dir)) {
    return false;
  }
  memcpy(dir, currentPath, dirLen);
  dir[dirLen] = '\0';
  const char* leaf = slash + 1;

  SdCardAdapter* card = storage_->card();
  if (card == nullptr || !card->mounted()) {
    return false;
  }
  fs::File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) {
    if (d) {
      d.close();
    }
    return false;
  }
  char best[64] = "";
  for (fs::File entry = d.openNextFile(); entry; entry = d.openNextFile()) {
    if (!entry.isDirectory() && isLcvName(entry.name())) {
      const char* name = entry.name();
      const bool candidate = forward
          ? (sortsBefore(leaf, name) && (best[0] == '\0' || sortsBefore(name, best)))
          : (sortsBefore(name, leaf) && (best[0] == '\0' || sortsBefore(best, name)));
      if (candidate) {
        strncpy(best, name, sizeof(best) - 1);
        best[sizeof(best) - 1] = '\0';
      }
    }
    entry.close();
  }
  d.close();
  if (best[0] == '\0') {
    return false;
  }
  snprintf(outPath, outLen, "%s/%s", dir, best);
  return true;
}

bool VideoService::nextInFolder(const char* currentPath, char* outPath, size_t outLen) {
  return sibling(currentPath, true, outPath, outLen);
}

bool VideoService::prevInFolder(const char* currentPath, char* outPath, size_t outLen) {
  return sibling(currentPath, false, outPath, outLen);
}

void VideoService::loadResume() {
  if (resumeLoaded_ || !prefsReady_) {
    return;
  }
  resumeLoaded_ = true;
  resumeCount_ = 0;
  const size_t bytes = prefs_.getBytes(kResumeKey, resume_, sizeof(resume_));
  resumeCount_ = bytes / sizeof(ResumeRec);
  seq_ = 0;
  for (size_t i = 0; i < resumeCount_; i++) {
    if (resume_[i].seq > seq_) {
      seq_ = resume_[i].seq;
    }
  }
}

void VideoService::storeResume() {
  if (prefsReady_) {
    prefs_.putBytes(kResumeKey, resume_, resumeCount_ * sizeof(ResumeRec));
  }
}

int VideoService::findResume(uint32_t crc) const {
  for (size_t i = 0; i < resumeCount_; i++) {
    if (resume_[i].pathCrc == crc) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

uint32_t VideoService::resumeMs(const char* path) {
  loadResume();
  const int i = findResume(crc32Path(path));
  return i < 0 ? 0 : resume_[i].posMs;
}

void VideoService::savePosition(const char* path, uint32_t posMs, uint32_t durationMs) {
  loadResume();
  // Early positions are noise; near-complete counts as finished.
  if (posMs < kMinSaveMs ||
      (durationMs > 0 && posMs >= durationMs - durationMs / 20)) {
    clearPosition(path);
    return;
  }
  const uint32_t crc = crc32Path(path);
  int i = findResume(crc);
  if (i < 0) {
    if (resumeCount_ < kMaxResume) {
      i = static_cast<int>(resumeCount_++);
    } else {
      i = 0;  // evict the least recently played
      for (size_t j = 1; j < resumeCount_; j++) {
        if (resume_[j].seq < resume_[i].seq) {
          i = static_cast<int>(j);
        }
      }
    }
    resume_[i].pathCrc = crc;
  }
  resume_[i].posMs = posMs;
  resume_[i].seq = ++seq_;
  storeResume();
}

void VideoService::clearPosition(const char* path) {
  loadResume();
  const int i = findResume(crc32Path(path));
  if (i < 0) {
    return;
  }
  resume_[i] = resume_[resumeCount_ - 1];
  resumeCount_--;
  storeResume();
}

#endif  // FEATURE_VIDEO
```

- [ ] **Step 3: Compile** — `./scripts/build.sh`. Expected: clean.

- [ ] **Step 4: Commit**

```bash
git add little-cube-os/src/services/VideoService.h little-cube-os/src/services/VideoService.cpp
git commit -m "Video: VideoService library scan, NVS resume blob, folder queue"
```

---

### Task 8: System wiring — AppId, Services, Kernel, icon

**Files:**
- Modify: `little-cube-os/src/core/App.h` (AppId enum, kAppCount)
- Modify: `little-cube-os/src/core/AppRouter.cpp` (appName)
- Modify: `little-cube-os/src/core/Services.h`
- Modify: `little-cube-os/src/core/Kernel.cpp`
- Modify: `little-cube-os/src/ui/Icons.h`, `little-cube-os/src/ui/Icons.cpp`

- [ ] **Step 1: The app id.** In `App.h`: append `Video,` after `Assistant,` in the `AppId` enum and change `kAppCount` to `16`. In `AppRouter.cpp` `appName()`: add `case AppId::Video: return "Video";`. (Unconditional — with `FEATURE_VIDEO 0` the id simply never registers; `AppRouter::find()` null-checks everywhere, so an unregistered id is inert.)

- [ ] **Step 2: Services.** In `Services.h`, add to the forward declarations:

```cpp
class VideoService;
class VideoPlayer;
```

and to the struct, after `radio`:

```cpp
  VideoService* video = nullptr;
  VideoPlayer* videoPlayer = nullptr;
```

- [ ] **Step 3: Kernel.** In `Kernel.cpp`:
  - Includes, with the other service includes:

```cpp
#if FEATURE_VIDEO
#include "../services/VideoService.h"
#include "../video/VideoPlayer.h"
#endif
```

  - File-scope instances, next to the other service singletons:

```cpp
#if FEATURE_VIDEO
VideoService videoService;
VideoPlayer videoPlayer;
#endif
```

  - Wiring, in the services block (~line 294, after `services.radio`):

```cpp
#if FEATURE_VIDEO
  services.video = &videoService;
  services.videoPlayer = &videoPlayer;
#endif
```

  - Begin, after `radioService.begin(&sdStorage);`:

```cpp
#if FEATURE_VIDEO
  videoService.begin(&sdStorage);  // pull-only, no update() slot
  videoPlayer.begin(&audioAdapter, &displayAdapter, &sdStorage);
#endif
```

  - Update slot, immediately after `audioAdapter.update(deltaMs);`:

```cpp
#if FEATURE_VIDEO
  videoPlayer.update(deltaMs);
#endif
```

- [ ] **Step 4: Icon.** In `Icons.h` add `Video,` to the `IconId` enum (before `Settings`). In `Icons.cpp` add a case following the file's `q`/`t`/`cy` idiom:

```cpp
    case IconId::Video: {
      // A screen with a play triangle.
      gfx.drawRoundRect(x, y + q / 2, size, size - q, t * 2, color);
      gfx.drawRoundRect(x + 1, y + q / 2 + 1, size - 2, size - q - 2, t * 2, color);
      gfx.fillTriangle(x + q + t, cy - q + t, x + q + t, cy + q - t, x + size - q, cy,
                       color);
      break;
    }
```

(If the local variable names in `Icons.cpp` differ from `q`/`t`/`cy`, use whatever the neighboring cases use — the shape is what matters.)

- [ ] **Step 5: Compile** — `./scripts/build.sh`. Expected: clean.

- [ ] **Step 6: Commit**

```bash
git add little-cube-os/src/core/ little-cube-os/src/ui/Icons.h little-cube-os/src/ui/Icons.cpp
git commit -m "Video: AppId, Services pointers, kernel wiring, home-card icon"
```

---

### Task 9: VideoApp — library browser (portrait)

**Files:**
- Create: `little-cube-os/src/apps/VideoApp.h`
- Create: `little-cube-os/src/apps/VideoApp.cpp`
- Modify: `little-cube-os/src/apps/AppRegistry.cpp`
- Modify: `little-cube-os/src/ui/Carousel.cpp` (card table, after the Audio row)

- [ ] **Step 1: Write `VideoApp.h`:**

```cpp
#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../feature_flags.h"

#if FEATURE_VIDEO

#include "../services/VideoService.h"
#include "../ui/StatusBar.h"
#include "../ui/widgets/Widgets.h"

class Arduino_Canvas;

// Video (design: docs/superpowers/specs/2026-07-30-video-playback-design.md).
// Two screens: a portrait library over /littlecube/video (folders one level
// deep, resume offers, battery warning), and a rotated-landscape player —
// the user turns the device sideways; frames are pre-rotated in the file,
// chrome is drawn landscape into a small canvas and transposed on.
class VideoApp : public App {
 public:
  explicit VideoApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override;
  void onResume() override;

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  enum class Screen {
    Library,
    ConfirmStart,  // battery warning and/or resume offer for pendingPath_
    Player,
  };

  void refreshList();
  void showPage(uint8_t page);
  void openItem(size_t index);
  void beginPlayback(uint32_t startMs);
  void stopAndSavePosition();
  void renderLibrary(Arduino_GFX& gfx);
  void renderConfirm(Arduino_GFX& gfx);
  void renderPlayer(Arduino_GFX& gfx);
  void renderChrome(Arduino_GFX& gfx);
  bool playerInput(const InputEvent& event);
  void formatMs(uint32_t ms, char* out, size_t len) const;

  static constexpr int16_t kChromeW = 448;  // landscape chrome canvas
  static constexpr int16_t kChromeH = 58;   // = the panel strip width
  static constexpr uint32_t kChromeHideMs = 4000;
  static constexpr uint32_t kSaveEveryMs = 5000;
  static constexpr int32_t kSeekStepMs = 15000;

  Services& services_;
  StatusBar statusBar_;
  Screen screen_ = Screen::Library;
  bool dirty_ = true;
  uint32_t lastStateVersion_ = 0xFFFFFFFF;

  // Library, mirroring AudioApp's paging pattern.
  static constexpr size_t kMaxListed = 4;
  static constexpr uint8_t kMaxPages = 8;
  VideoInfo items_[kMaxListed];
  size_t itemCount_ = 0;
  size_t totalItems_ = 0;
  char pageAnchors_[kMaxPages][66] = {};
  uint8_t page_ = 0;
  char dir_[160] = "";  // current folder; paths::kVideo at the root
  widgets::Rect rowRects_[kMaxListed];

  // Pending start (ConfirmStart screen).
  char pendingPath_[160] = "";
  uint32_t pendingResumeMs_ = 0;
  bool batteryWarned_ = false;
  widgets::Rect confirmRect_;
  widgets::Rect cancelRect_;

  // Player chrome. The canvas is heavy (~52 KB) — allocated on open,
  // deleted on close per the App lifecycle contract.
  Arduino_Canvas* chrome_ = nullptr;
  bool chromeVisible_ = true;
  bool chromeDirty_ = true;
  uint32_t chromeMs_ = 0;
  uint32_t saveMs_ = 0;
  uint32_t posShownS_ = 0xFFFFFFFF;  // last second drawn, to redraw chrome 1 Hz
  bool wasPlaying_ = false;
  // Chrome hit rects in PORTRAIT coordinates (the strip is vertical).
  widgets::Rect backRect_;
  widgets::Rect prevRect_;
  widgets::Rect playRect_;
  widgets::Rect nextRect_;
  widgets::Rect stopRect_;
  widgets::Rect scrubRect_;
};

#endif  // FEATURE_VIDEO
```

- [ ] **Step 2: Write `VideoApp.cpp` — library half.** (The player methods land in Task 10; in this task `renderPlayer`, `renderChrome`, and `playerInput` are minimal-but-real: `renderPlayer` fills black, `playerInput` handles only Back, `renderChrome` is empty. Playback already works underneath; only its chrome is missing.)

```cpp
#include "VideoApp.h"

#if FEATURE_VIDEO

#include <Arduino_GFX_Library.h>

#include "../core/SystemState.h"
#include "../hardware/BatteryAdapter.h"
#include "../hardware/DisplayAdapter.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../storage/StoragePaths.h"
#include "../ui/AmoledProtection.h"
#include "../ui/Theme.h"
#include "../video/VideoPlayer.h"

void VideoApp::onOpen() {
  screen_ = Screen::Library;
  strncpy(dir_, paths::kVideo, sizeof(dir_) - 1);
  dir_[sizeof(dir_) - 1] = '\0';
  page_ = 0;
  pageAnchors_[0][0] = '\0';
  refreshList();
  if (chrome_ == nullptr) {
    chrome_ = new Arduino_Canvas(kChromeW, kChromeH, nullptr);
    chrome_->begin(GFX_SKIP_OUTPUT_BEGIN);
  }
  dirty_ = true;
}

void VideoApp::onClose() {
  stopAndSavePosition();
  delete chrome_;  // heavy buffer; the next onOpen re-allocates
  chrome_ = nullptr;
}

void VideoApp::onPause() { stopAndSavePosition(); }

void VideoApp::onResume() {
  screen_ = Screen::Library;
  refreshList();
  dirty_ = true;
}

void VideoApp::stopAndSavePosition() {
  VideoPlayer* player = services_.videoPlayer;
  if (player != nullptr && player->playing()) {
    services_.video->savePosition(player->path(), player->positionMs(),
                                  player->durationMs());
    player->setUiActive(false);
    player->requestStop();
  }
}

void VideoApp::refreshList() {
  const char* after = page_ > 0 ? pageAnchors_[page_] : nullptr;
  itemCount_ = services_.video->list(dir_, items_, kMaxListed, &totalItems_, after);
  dirty_ = true;
}

void VideoApp::showPage(uint8_t page) {
  if (page >= kMaxPages) {
    return;
  }
  if (page > page_ && itemCount_ > 0) {
    // Anchor = sort key of the last visible row (dirs prefix '0', files '1').
    const VideoInfo& last = items_[itemCount_ - 1];
    pageAnchors_[page][0] = last.isDir ? '0' : '1';
    strncpy(pageAnchors_[page] + 1, last.name, sizeof(pageAnchors_[page]) - 2);
    pageAnchors_[page][sizeof(pageAnchors_[page]) - 1] = '\0';
  }
  page_ = page;
  refreshList();
}

void VideoApp::formatMs(uint32_t ms, char* out, size_t len) const {
  const uint32_t s = ms / 1000;
  if (s >= 3600) {
    snprintf(out, len, "%lu:%02lu:%02lu", static_cast<unsigned long>(s / 3600),
             static_cast<unsigned long>((s / 60) % 60), static_cast<unsigned long>(s % 60));
  } else {
    snprintf(out, len, "%lu:%02lu", static_cast<unsigned long>(s / 60),
             static_cast<unsigned long>(s % 60));
  }
}

void VideoApp::openItem(size_t index) {
  if (index >= itemCount_) {
    return;
  }
  const VideoInfo& item = items_[index];
  if (item.isDir) {
    strncpy(dir_, item.path, sizeof(dir_) - 1);
    dir_[sizeof(dir_) - 1] = '\0';
    page_ = 0;
    refreshList();
    return;
  }
  strncpy(pendingPath_, item.path, sizeof(pendingPath_) - 1);
  pendingPath_[sizeof(pendingPath_) - 1] = '\0';
  pendingResumeMs_ = services_.video->resumeMs(pendingPath_);
  const SystemState* st = services_.state;
  const bool lowBattery = st != nullptr && st->batteryPresent && st->batteryPercent >= 0 &&
                          st->batteryPercent < 10;
  batteryWarned_ = false;
  if (lowBattery || pendingResumeMs_ > 0) {
    batteryWarned_ = lowBattery;
    screen_ = Screen::ConfirmStart;
    dirty_ = true;
  } else {
    beginPlayback(0);
  }
}

void VideoApp::beginPlayback(uint32_t startMs) {
  char why[48];
  VideoPlayer* player = services_.videoPlayer;
  if (player == nullptr || !player->play(pendingPath_, startMs, why, sizeof(why))) {
    widgets::toast(*services_.display->canvas(), why);
    services_.display->markDirty();
    screen_ = Screen::Library;
    dirty_ = true;
    return;
  }
  player->setUiActive(true);
  screen_ = Screen::Player;
  chromeVisible_ = true;
  chromeDirty_ = true;
  chromeMs_ = 0;
  saveMs_ = 0;
  wasPlaying_ = true;
  // Full black once; the decoder owns the image area from here on.
  services_.display->canvas()->fillScreen(RGB565_BLACK);
  services_.display->markDirty();
}

void VideoApp::update(uint32_t deltaMs) {
  VideoPlayer* player = services_.videoPlayer;
  if (screen_ != Screen::Player || player == nullptr) {
    return;
  }
  // Periodic position save — this is what makes resume survive battery death.
  if (player->playing() && !player->paused()) {
    saveMs_ += deltaMs;
    if (saveMs_ >= kSaveEveryMs) {
      saveMs_ = 0;
      services_.video->savePosition(player->path(), player->positionMs(),
                                    player->durationMs());
    }
  }
  // Chrome auto-hide.
  if (chromeVisible_) {
    chromeMs_ += deltaMs;
    if (chromeMs_ >= kChromeHideMs && player->playing() && !player->paused()) {
      chromeVisible_ = false;
      chromeDirty_ = true;
    }
  }
  // Redraw the timeline once per second while visible.
  if (chromeVisible_ && player->playing()) {
    const uint32_t s = player->positionMs() / 1000;
    if (s != posShownS_) {
      posShownS_ = s;
      chromeDirty_ = true;
    }
  }
  // Natural end -> auto-advance; user stop -> library. (Queue rule: only a
  // completed episode advances, mirroring lastPlayCompleted() semantics.)
  const bool playingNow = player->playing();
  if (wasPlaying_ && !playingNow) {
    if (player->completed()) {
      services_.video->clearPosition(pendingPath_);
      char next[160];
      if (services_.video->nextInFolder(pendingPath_, next, sizeof(next))) {
        strncpy(pendingPath_, next, sizeof(pendingPath_) - 1);
        pendingPath_[sizeof(pendingPath_) - 1] = '\0';
        beginPlayback(0);
        wasPlaying_ = player->playing();
        return;
      }
    }
    screen_ = Screen::Library;
    refreshList();
    dirty_ = true;
  }
  wasPlaying_ = playingNow;
}

void VideoApp::render() {
  Arduino_GFX* gfx = services_.display->canvas();
  if (gfx == nullptr) {
    return;
  }
  const SystemState* st = services_.state;
  const bool stateChanged = st != nullptr && st->version != lastStateVersion_;
  if (st != nullptr) {
    lastStateVersion_ = st->version;
  }
  switch (screen_) {
    case Screen::Library:
      if (dirty_ || stateChanged) {
        renderLibrary(*gfx);
        services_.display->markDirty();
        dirty_ = false;
      }
      return;
    case Screen::ConfirmStart:
      if (dirty_ || stateChanged) {
        renderConfirm(*gfx);
        services_.display->markDirty();
        dirty_ = false;
      }
      return;
    case Screen::Player:
      renderPlayer(*gfx);  // frame pixels come from VideoPlayer directly
      return;
  }
}

void VideoApp::renderLibrary(Arduino_GFX& gfx) {
  gfx.fillScreen(theme::kBg);
  statusBar_.render(gfx, *services_.state, services_.amoled->shiftX(),
                    services_.amoled->shiftY());
  const bool atRoot = strcmp(dir_, paths::kVideo) == 0;
  const char* slash = strrchr(dir_, '/');
  int16_t y = widgets::header(gfx, atRoot ? "Video" : (slash ? slash + 1 : dir_),
                              services_.amoled->shiftX(), services_.amoled->shiftY());
  if (itemCount_ == 0) {
    widgets::textBlock(gfx, theme::kPadding, y + theme::kPadding,
                       DISPLAY_WIDTH - 2 * theme::kPadding,
                       "No episodes.\n\nPack one on the Mac:\n"
                       "./scripts/pack-video.sh show.mkv\n"
                       "then copy the .lcv to\n/littlecube/video/ on the card.",
                       widgets::TextStyle::Body, theme::kTextDim);
    return;
  }
  char sub[24];
  for (size_t i = 0; i < itemCount_; i++) {
    if (items_[i].isDir) {
      snprintf(sub, sizeof(sub), "folder");
    } else {
      formatMs(items_[i].durationMs, sub, sizeof(sub));
    }
    rowRects_[i] = widgets::listItem(gfx, theme::kPadding, y,
                                     DISPLAY_WIDTH - 2 * theme::kPadding, items_[i].name,
                                     sub, false);
    y = rowRects_[i].y + rowRects_[i].h + 6;
  }
  if (totalItems_ > itemCount_) {
    char more[32];
    snprintf(more, sizeof(more), "+%u more — swipe up",
             static_cast<unsigned>(totalItems_ - itemCount_));
    widgets::textCentered(gfx, 0, y + 4, DISPLAY_WIDTH, more, widgets::TextStyle::Caption,
                          theme::kTextDim);
  }
}

void VideoApp::renderConfirm(Arduino_GFX& gfx) {
  gfx.fillScreen(theme::kBg);
  char body[96];
  if (batteryWarned_) {
    snprintf(body, sizeof(body), "Battery is low.\nVideo lasts ~30-45 min per charge.");
    confirmRect_ = widgets::modalConfirm(gfx, "Low battery", body, cancelRect_);
  } else {
    char at[16];
    formatMs(pendingResumeMs_, at, sizeof(at));
    snprintf(body, sizeof(body), "Resume at %s?\nCancel starts over.", at);
    confirmRect_ = widgets::modalConfirm(gfx, "Resume", body, cancelRect_);
  }
}

bool VideoApp::handleInput(const InputEvent& event) {
  switch (screen_) {
    case Screen::Player:
      return playerInput(event);
    case Screen::ConfirmStart:
      if (event.action == InputAction::Tap) {
        if (confirmRect_.contains(event.x, event.y)) {
          if (batteryWarned_) {
            // Battery acknowledged; fall through to the resume question.
            batteryWarned_ = false;
            if (pendingResumeMs_ > 0) {
              dirty_ = true;
              return true;
            }
            beginPlayback(0);
            return true;
          }
          beginPlayback(pendingResumeMs_);
          return true;
        }
        if (cancelRect_.contains(event.x, event.y)) {
          if (batteryWarned_) {
            screen_ = Screen::Library;  // declined at the battery warning
          } else {
            beginPlayback(0);  // "start over"
          }
          dirty_ = true;
          return true;
        }
      }
      if (event.action == InputAction::Back) {
        screen_ = Screen::Library;
        dirty_ = true;
        return true;
      }
      return false;
    case Screen::Library:
      break;
  }
  switch (event.action) {
    case InputAction::Tap:
      for (size_t i = 0; i < itemCount_; i++) {
        if (rowRects_[i].contains(event.x, event.y)) {
          openItem(i);
          return true;
        }
      }
      return false;
    case InputAction::SwipeUp:
      if (totalItems_ > static_cast<size_t>(page_ + 1) * kMaxListed) {
        showPage(page_ + 1);
      }
      return true;
    case InputAction::SwipeDown:
      if (page_ > 0) {
        showPage(page_ - 1);
      }
      return true;
    case InputAction::Back:
      if (strcmp(dir_, paths::kVideo) != 0) {
        onOpen();  // leave the folder, back to the root listing
        return true;
      }
      return false;  // at the root: fall through to the router
    default:
      return false;
  }
}

// ---- Player rendering & input: completed in the next task -------------------

void VideoApp::renderPlayer(Arduino_GFX& gfx) { renderChrome(gfx); }

void VideoApp::renderChrome(Arduino_GFX& gfx) { (void)gfx; }

bool VideoApp::playerInput(const InputEvent& event) {
  if (event.action == InputAction::Back) {
    stopAndSavePosition();
    screen_ = Screen::Library;
    refreshList();
    dirty_ = true;
    return true;
  }
  return false;
}

#endif  // FEATURE_VIDEO
```

- [ ] **Step 3: Register.** In `AppRegistry.cpp`:

```cpp
#include "../feature_flags.h"
#if FEATURE_VIDEO
#include "VideoApp.h"
#endif
```

and inside `registerApps`, after the `news` lines:

```cpp
#if FEATURE_VIDEO
  static VideoApp video(services);
  router.registerApp(AppId::Video, &video);
#endif
```

- [ ] **Step 4: Carousel card.** In `Carousel.cpp`, the card table, after the Audio row (include `../feature_flags.h` if not already):

```cpp
#if FEATURE_VIDEO
    {AppId::Video, "Video", "tv & anime", icons::IconId::Video, false},
#endif
```

- [ ] **Step 5: Compile** — `./scripts/build.sh`. Expected: clean. (If `widgets::modalConfirm` or `widgets::toast` signatures differ from the calls above, match the real signatures in `Widgets.h` — both exist there.)

- [ ] **Step 6: Commit**

```bash
git add little-cube-os/src/apps/ little-cube-os/src/ui/Carousel.cpp
git commit -m "Video: VideoApp library browser, registration, home card"
```

---

### Task 10: VideoApp — player screen, rotated chrome, gestures

**Files:**
- Modify: `little-cube-os/src/apps/VideoApp.cpp` (replace the three player stubs)

- [ ] **Step 1: Replace `renderPlayer` and `renderChrome`:**

```cpp
void VideoApp::renderPlayer(Arduino_GFX& gfx) {
  // The video image area is owned by VideoPlayer::decodeFrame(); this method
  // touches ONLY the right-hand strip (the "bottom" once the device is
  // turned). Never clear the whole screen here — it would fight the decoder.
  if (!chromeDirty_) {
    return;
  }
  chromeDirty_ = false;
  const int16_t stripX = DISPLAY_WIDTH - kChromeH;  // 310
  if (!chromeVisible_) {
    gfx.fillRect(stripX, 0, kChromeH, DISPLAY_HEIGHT, RGB565_BLACK);
    services_.display->markDirty();
    return;
  }
  renderChrome(gfx);
  services_.display->markDirty();
}

void VideoApp::renderChrome(Arduino_GFX& gfx) {
  VideoPlayer* player = services_.videoPlayer;
  if (chrome_ == nullptr || player == nullptr) {
    return;
  }
  // 1) Draw the chrome in LANDSCAPE into the 448x58 canvas using the normal
  //    text helpers. Layout left->right: back, prev, play/pause, next, stop,
  //    then the scrub bar with the time readout above it.
  Arduino_GFX& c = *chrome_;
  c.fillScreen(RGB565_BLACK);
  struct Btn {
    const char* label;
    widgets::Rect* rect;
  };
  const char* playLabel = player->paused() ? ">" : "||";
  Btn btns[5] = {{"<-", &backRect_}, {"|<", &prevRect_}, {playLabel, &playRect_},
                 {">|", &nextRect_}, {"[]", &stopRect_}};
  int16_t bx = theme::kSafeInset;  // inset from the panel's rounded corner
  for (auto& b : btns) {
    const widgets::Rect r = widgets::button(c, bx, 4, 44, kChromeH - 8, b.label);
    // Store the PORTRAIT-space hit rect now (see the mapping note below).
    b.rect->x = DISPLAY_WIDTH - kChromeH;
    b.rect->y = r.x;
    b.rect->w = kChromeH;
    b.rect->h = r.w;
    bx += 44 + 6;
  }
  // Scrub bar in the remaining width.
  const int16_t sx = bx + 4;
  const int16_t sw = kChromeW - theme::kSafeInset - sx;
  char pos[16];
  char dur[16];
  formatMs(player->positionMs(), pos, sizeof(pos));
  formatMs(player->durationMs(), dur, sizeof(dur));
  char times[36];
  snprintf(times, sizeof(times), "%s / %s", pos, dur);
  widgets::textCentered(c, sx, 6, sw, times, widgets::TextStyle::Caption, theme::kText);
  const int16_t barY = kChromeH - 18;
  c.fillRect(sx, barY, sw, 6, theme::kPanelAlt);
  if (player->durationMs() > 0) {
    const int16_t fill = static_cast<int16_t>(
        static_cast<int64_t>(sw) * player->positionMs() / player->durationMs());
    c.fillRect(sx, barY, fill, 6, theme::kAccent);
  }
  scrubRect_ = {static_cast<int16_t>(DISPLAY_WIDTH - kChromeH), sx, kChromeH, sw};

  // 2) Transpose onto the panel strip. Same handedness as the frames
  //    (ffmpeg transpose=1, 90 deg CW): dst(x, y) = chrome(y, kChromeH-1-x).
  //    If chrome text reads upside-down relative to the video on device,
  //    change the source index to src[x * kChromeW + (kChromeW - 1 - y)].
  DisplayAdapter* display = services_.display;
  if (!display->hasCanvas()) {
    return;  // no framebuffer to transpose into (degraded direct-draw mode)
  }
  uint16_t* dst = static_cast<Arduino_Canvas*>(display->canvas())->getFramebuffer();
  const uint16_t* src = chrome_->getFramebuffer();
  const int16_t stripX = DISPLAY_WIDTH - kChromeH;
  for (int16_t y = 0; y < DISPLAY_HEIGHT; y++) {
    uint16_t* row = dst + y * DISPLAY_WIDTH + stripX;
    for (int16_t x = 0; x < kChromeH; x++) {
      row[x] = src[(kChromeH - 1 - x) * kChromeW + y];
    }
  }
}
```

Hit-rect mapping rationale (also the comment to keep in the code): a chrome element drawn landscape at x∈[L,R) spans portrait y∈[L,R) in the strip after this transpose, and the strip itself is portrait x∈[310,368) — so every rect is `{x: 310, y: L, w: 58, h: R−L}`, which is what the loop stores.

- [ ] **Step 2: Replace `playerInput`:**

```cpp
bool VideoApp::playerInput(const InputEvent& event) {
  VideoPlayer* player = services_.videoPlayer;
  if (player == nullptr) {
    return false;
  }
  // Any interaction (re)shows the chrome and rearms the hide timer.
  auto poke = [&]() {
    chromeMs_ = 0;
    if (!chromeVisible_) {
      chromeVisible_ = true;
      chromeDirty_ = true;
    }
  };
  switch (event.action) {
    case InputAction::Tap:
      if (!chromeVisible_) {
        poke();
        return true;
      }
      poke();
      if (playRect_.contains(event.x, event.y)) {
        player->setPaused(!player->paused());
        chromeDirty_ = true;
        return true;
      }
      if (stopRect_.contains(event.x, event.y) || backRect_.contains(event.x, event.y)) {
        stopAndSavePosition();
        screen_ = Screen::Library;
        refreshList();
        dirty_ = true;
        return true;
      }
      if (nextRect_.contains(event.x, event.y) || prevRect_.contains(event.x, event.y)) {
        const bool fwd = nextRect_.contains(event.x, event.y);
        char sib[160];
        if ((fwd ? services_.video->nextInFolder(pendingPath_, sib, sizeof(sib))
                 : services_.video->prevInFolder(pendingPath_, sib, sizeof(sib)))) {
          services_.video->savePosition(player->path(), player->positionMs(),
                                        player->durationMs());
          player->requestStop();
          strncpy(pendingPath_, sib, sizeof(pendingPath_) - 1);
          pendingPath_[sizeof(pendingPath_) - 1] = '\0';
          // beginPlayback() once the stop lands: reuse the natural-end path
          // by waiting for idle in update() — simplest is to poll here:
          pendingResumeMs_ = 0;
          // Mark so update()'s idle edge starts the pending sibling.
          wasPlaying_ = true;
          nextQueued_ = true;
        }
        return true;
      }
      if (scrubRect_.contains(event.x, event.y)) {
        // Portrait y within the scrub rect maps to landscape x = fraction.
        const int32_t frac = event.y - scrubRect_.y;
        const uint32_t target = static_cast<uint32_t>(
            static_cast<int64_t>(player->durationMs()) * frac / scrubRect_.h);
        player->requestSeek(static_cast<int32_t>(target) -
                            static_cast<int32_t>(player->positionMs()));
        return true;
      }
      chromeVisible_ = false;  // tap on the picture: hide the chrome
      chromeDirty_ = true;
      return true;
    case InputAction::DoubleTap:
      poke();
      player->setPaused(!player->paused());
      chromeDirty_ = true;
      return true;
    // Rotated-90 gesture map (device turned CCW to watch): an in-hand
    // horizontal swipe arrives as portrait Up/Down = seek; an in-hand
    // vertical swipe arrives as portrait Left/Right = volume.
    case InputAction::SwipeDown:
      poke();
      player->requestSeek(kSeekStepMs);
      return true;
    case InputAction::SwipeUp:
      poke();
      player->requestSeek(-kSeekStepMs);
      return true;
    case InputAction::SwipeRight:
      poke();
      services_.audio->setVolumePercent(
          services_.audio->volumePercent() >= 90 ? 100 : services_.audio->volumePercent() + 10);
      return true;
    case InputAction::SwipeLeft:
      poke();
      services_.audio->setVolumePercent(
          services_.audio->volumePercent() <= 10 ? 0 : services_.audio->volumePercent() - 10);
      return true;
    case InputAction::Back:
      stopAndSavePosition();
      screen_ = Screen::Library;
      refreshList();
      dirty_ = true;
      return true;
    default:
      return false;  // Home falls through: the router homes, onPause saves+stops
  }
}
```

- [ ] **Step 3: The `nextQueued_` handshake.** Add `bool nextQueued_ = false;` to the private members in `VideoApp.h`, and in `update()`'s playing→idle edge, check it *before* the `completed()` branch:

```cpp
  if (wasPlaying_ && !playingNow) {
    if (nextQueued_) {
      nextQueued_ = false;
      beginPlayback(0);
      wasPlaying_ = services_.videoPlayer->playing();
      return;
    }
    if (player->completed()) {
      // ... (existing auto-advance branch unchanged)
```

- [ ] **Step 4: Compile** — `./scripts/build.sh`. Expected: clean.

- [ ] **Step 5: Commit**

```bash
git add little-cube-os/src/apps/VideoApp.h little-cube-os/src/apps/VideoApp.cpp
git commit -m "Video: player screen — rotated chrome, gestures, scrub, episode prev/next"
```

---

### Task 11: Serial command family

**Files:**
- Create: `little-cube-os/src/serial/commands/VideoCommands.h`
- Create: `little-cube-os/src/serial/commands/VideoCommands.cpp`
- Modify: `little-cube-os/src/serial/SerialCommandService.cpp`
- Modify: `docs/serial-interface.md`

- [ ] **Step 1: Write `VideoCommands.h`:**

```cpp
#pragma once

#include "../../feature_flags.h"

#if FEATURE_VIDEO

struct Services;

// `video ...` family: list/play/pause/resume/seek/stop/status/queue.
// Commands act through VideoService/VideoPlayer, never on hardware.
bool handleVideoCommand(Services& services, const char* verb, char* args);
void printVideoHelp();

#endif  // FEATURE_VIDEO
```

- [ ] **Step 2: Write `VideoCommands.cpp`:**

```cpp
#include "VideoCommands.h"

#if FEATURE_VIDEO

#include <Arduino.h>
#include <string.h>

#include "../../core/AppRouter.h"
#include "../../core/Services.h"
#include "../../services/VideoService.h"
#include "../../storage/StoragePaths.h"
#include "../../video/VideoPlayer.h"
#include "../CmdArgs.h"

namespace {

void fmtMs(uint32_t ms, char* out, size_t len) {
  const uint32_t s = ms / 1000;
  snprintf(out, len, "%lu:%02lu", static_cast<unsigned long>(s / 60),
           static_cast<unsigned long>(s % 60));
}

// "mm:ss", "+N", "-N" (seconds) -> relative delta against `fromMs`.
bool parseSeek(const char* tok, uint32_t fromMs, int32_t& deltaOut) {
  if (tok == nullptr || tok[0] == '\0') {
    return false;
  }
  if (tok[0] == '+' || tok[0] == '-') {
    deltaOut = static_cast<int32_t>(strtol(tok, nullptr, 10)) * 1000;
    return true;
  }
  const char* colon = strchr(tok, ':');
  if (colon == nullptr) {
    return false;
  }
  const long m = strtol(tok, nullptr, 10);
  const long s = strtol(colon + 1, nullptr, 10);
  if (m < 0 || s < 0 || s >= 60) {
    return false;
  }
  deltaOut = static_cast<int32_t>((m * 60 + s) * 1000) - static_cast<int32_t>(fromMs);
  return true;
}

}  // namespace

bool handleVideoCommand(Services& services, const char* verb, char* args) {
  VideoService* video = services.video;
  VideoPlayer* player = services.videoPlayer;
  if (video == nullptr || player == nullptr) {
    Serial.println("video: unavailable (FEATURE_VIDEO off)");
    return true;
  }

  if (strcmp(verb, "list") == 0) {
    char* cursor = args;
    const char* dir = cmdargs::nextToken(cursor);
    VideoInfo items[8];
    size_t total = 0;
    const size_t n =
        video->list(dir != nullptr ? dir : paths::kVideo, items, 8, &total);
    for (size_t i = 0; i < n; i++) {
      if (items[i].isDir) {
        Serial.printf("  [dir]  %s\n", items[i].name);
      } else {
        char d[16];
        fmtMs(items[i].durationMs, d, sizeof(d));
        Serial.printf("  %-8s %s\n", d, items[i].name);
      }
    }
    Serial.printf("%u item(s)%s\n", static_cast<unsigned>(total),
                  total > n ? " (first page shown)" : "");
    return true;
  }
  if (strcmp(verb, "play") == 0) {
    char* cursor = args;
    const char* path = cmdargs::rest(cursor);
    if (path == nullptr) {
      Serial.println("usage: video play <path.lcv>");
      return true;
    }
    char full[160];
    if (path[0] == '/') {
      strncpy(full, path, sizeof(full) - 1);
      full[sizeof(full) - 1] = '\0';
    } else {
      snprintf(full, sizeof(full), "%s/%s", paths::kVideo, path);
    }
    char why[48];
    if (!player->play(full, video->resumeMs(full), why, sizeof(why))) {
      Serial.printf("video: refused — %s\n", why);
      return true;
    }
    // Decode needs the screen: playing over serial opens the app like a tap
    // would. VideoApp's onOpen lands on the library; entering the player
    // screen stays a UI concern, so headless playback shows frames only
    // while the app's player screen is up — audio and position run regardless.
    if (services.router != nullptr) {
      services.router->open(AppId::Video);
    }
    player->setUiActive(true);
    Serial.printf("video: playing %s\n", full);
    return true;
  }
  if (strcmp(verb, "pause") == 0) {
    player->setPaused(true);
    Serial.println("video: paused");
    return true;
  }
  if (strcmp(verb, "resume") == 0) {
    player->setPaused(false);
    Serial.println("video: resumed");
    return true;
  }
  if (strcmp(verb, "seek") == 0) {
    char* cursor = args;
    const char* tok = cmdargs::nextToken(cursor);
    int32_t delta = 0;
    if (!parseSeek(tok, player->positionMs(), delta)) {
      Serial.println("usage: video seek <mm:ss | +sec | -sec>");
      return true;
    }
    player->requestSeek(delta);
    Serial.println("video: seeking");
    return true;
  }
  if (strcmp(verb, "stop") == 0) {
    player->requestStop();
    Serial.println("video: stopping");
    return true;
  }
  if (strcmp(verb, "status") == 0) {
    if (player->idle()) {
      Serial.println("video: idle");
      return true;
    }
    char pos[16];
    char dur[16];
    fmtMs(player->positionMs(), pos, sizeof(pos));
    fmtMs(player->durationMs(), dur, sizeof(dur));
    Serial.printf("video: %s %s/%s %ux%u@%ufps shown=%lu dropped=%lu ring=%u%s\n",
                  player->path(), pos, dur, player->header().width,
                  player->header().height, player->header().fps,
                  static_cast<unsigned long>(player->framesShown()),
                  static_cast<unsigned long>(player->framesDropped()),
                  player->ringDepth(), player->paused() ? " [paused]" : "");
    return true;
  }
  if (strcmp(verb, "queue") == 0) {
    if (player->idle()) {
      Serial.println("video: nothing playing");
      return true;
    }
    char next[160];
    if (services.video->nextInFolder(player->path(), next, sizeof(next))) {
      Serial.printf("video: next up %s\n", next);
    } else {
      Serial.println("video: last episode in its folder");
    }
    return true;
  }
  return false;
}

void printVideoHelp() {
  Serial.println("video list [dir]            list /littlecube/video (or a subfolder)");
  Serial.println("video play <path.lcv>       play (relative paths resolve under video/)");
  Serial.println("video pause | resume        pause / resume playback");
  Serial.println("video seek <mm:ss|+s|-s>    absolute or relative seek");
  Serial.println("video stop                  stop (position is saved by the app)");
  Serial.println("video status                path, position, fps, drops, ring depth");
  Serial.println("video queue                 show the next episode in the folder");
  Serial.println("Pack episodes on a computer: ./scripts/pack-video.sh input.mkv");
}

#endif  // FEATURE_VIDEO
```

- [ ] **Step 3: Wire the family.** In `SerialCommandService.cpp`:
  - Include (with the other command includes): `#include "commands/VideoCommands.h"`
  - In `handleLine()`, after the `podcast` block:

```cpp
#if FEATURE_VIDEO
  if (strcmp(family, "video") == 0) {
    const char* verb = cmdargs::nextToken(cursor);
    if (verb == nullptr || !handleVideoCommand(*services_, verb, cursor)) {
      Serial.printf("error: unknown command 'video %s' — try 'help video'\n",
                    verb != nullptr ? verb : "");
    }
    return;
  }
#endif
```

  - Add `video` to the help-topic dispatch (`printVideoHelp()` for `help video`, guarded the same way) and mention `video` in the families line of the general `help` output, matching how `radio`/`podcast` appear.

- [ ] **Step 4: Document.** Append to `docs/serial-interface.md`, matching the existing per-family section format:

```markdown
## video

Playback of `.lcv` episodes from `/littlecube/video` (format:
`docs/lcv-format.md`; produced by `scripts/pack-video.sh`).

| command | effect |
|---|---|
| `video list [dir]` | list the library root or a season subfolder |
| `video play <path.lcv>` | play; relative paths resolve under `/littlecube/video`; resumes if a position is saved; opens the Video app |
| `video pause` / `video resume` | pause / resume |
| `video seek <mm:ss\|+sec\|-sec>` | absolute or relative seek |
| `video stop` | stop playback (never auto-advances) |
| `video status` | path, position/duration, fps, frames shown/dropped, ring depth |
| `video queue` | print the next episode in the current folder |

Video refuses to start while recording is active, and recording refuses
while video plays (half-duplex audio, one owner at a time).
```

- [ ] **Step 5: Compile** — `./scripts/build.sh`. Expected: clean.

- [ ] **Step 6: Commit**

```bash
git add little-cube-os/src/serial/ docs/serial-interface.md
git commit -m "Serial: video command family (list/play/seek/status/queue)"
```

---

### Task 12: Docs, validation checklist, final verification

**Files:**
- Modify: `docs/hardware-validation.md`
- Modify: `technical.md`

- [ ] **Step 1: Add the validation section.** Append to `docs/hardware-validation.md`, matching its existing checklist format. Every box stays UNCHECKED — the repo rule is absolute: no box is ticked without physically testing on the cube.

```markdown
## Video (FEATURE_VIDEO)

Prereq: at least one real `.lcv` on the card under `/littlecube/video/`
(`./scripts/pack-video.sh episode.mkv`), verified `OK` by
`scripts/lcv_mux.py inspect` on the computer first.

- [ ] `video list` over serial shows the file with the correct duration
- [ ] Library UI lists it; tap starts playback; device turned sideways shows
      the picture upright and filling the panel's long axis
- [ ] Colors correct (if red/blue swap: flip the `setPixelType` endianness
      in `VideoPlayer.cpp::decodeFrame`)
- [ ] Chrome reads the same way up as the picture (if upside-down: apply the
      transpose flip noted in `VideoApp.cpp::renderChrome`)
- [ ] Lip-sync: dialogue matches mouths after 5+ minutes of playback
      (audio-master check; tune `kDmaDepthSamples` if video leads/trails)
- [ ] High-motion scene: frames drop (`video status` dropped counter rises)
      while audio stays clean and unbroken
- [ ] Pause: instant silence + frozen frame; resume continues in sync
- [ ] Seek via scrub bar and `video seek +60`; picture + audio land together
- [ ] Stop, reopen the episode: resume offer at the right position
- [ ] Watch an episode to the end: position record cleared, next `.lcv` in
      the folder auto-plays; the LAST file in a folder returns to the library
- [ ] `video status` reports sane fps/shown/dropped/ring during playback
- [ ] Recording refusal both directions (`video play` during a recording;
      recording start during playback)
- [ ] Card yank mid-play: specific error state, no hang; recorder still
      works afterwards; reinserted card lists again
- [ ] Battery < 10 %: warning screen appears before playback starts
- [ ] Back (BOOT short) in the player exits to the library; long-press Home
      leaves the app, playback stops, position saved
- [ ] Leave the cube on the Home screen 10+ min after watching: no video
      chrome burn-in artifacts (AmoledProtection shifts applied)
```

- [ ] **Step 2: Update `technical.md`:** change "registers 15 apps" to "registers 16 apps" and add Video to the app list; add a sentence to the services paragraph: "Video playback decodes `.lcv` (MJPEG + PCM) files from the SD card — see `docs/lcv-format.md`."

- [ ] **Step 3: Flag-off build check.** Set `#define FEATURE_VIDEO 0` in `feature_flags.h`, run `./scripts/build.sh` — expected: clean compile with the whole subsystem out. Set it back to `1`, compile again — clean. This proves the flag actually gates everything.

- [ ] **Step 4: Commit**

```bash
git add docs/hardware-validation.md technical.md
git commit -m "Docs: video validation checklist, technical guide update"
```

- [ ] **Step 5: Hardware validation (requires the physical cube + a packed episode).** Flash with `./scripts/upload.sh`, open `./scripts/monitor.sh`, and work through the new checklist section, ticking ONLY what is physically observed. Record results in `docs/hardware-validation.md` and commit separately as e.g. `Docs: video hardware validation results`. Known tuning knobs, in likely order of need: `kDmaDepthSamples` (lip-sync offset), `setPixelType` endianness (color swap), the chrome transpose flip (text orientation), `-q:v` in `pack-video.sh` (frame size vs. quality).

---

## Plan self-review notes (already applied)

- **Spec coverage:** container+index (T1–T3), PCM/half-duplex (T4), engine+clock+drop (T5), decode (T6), library/resume/queue (T7), wiring+flag (T8), portrait library+modals (T9), rotated player+gestures (T10), serial family (T11), degradation+validation (T12). Subtitles are v1-out per spec; the decode loop isolates where an `.srt` overlay would slot in (chrome strip render).
- **Type consistency spot-checks:** `pcmSamplesPlayed()` returns `uint32_t` everywhere; `LcvReader::endOfData()` (not `endOfFile`) is the name used by `videoReaderTask`; `VideoInfo`/`ResumeRec` fields match between tasks; `kChromeH` = 58 = strip width in both render and hit-rect code.
- **Known verify-on-device items** (explicitly marked in code comments, not silent): RGB565 endianness, chrome transpose handedness, `kDmaDepthSamples`.
