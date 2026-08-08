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
| 8   | 2    | width | frame width as stored on disk; bound depends on `orientation` (below) |
| 10  | 2    | height | frame height as stored on disk; bound depends on `orientation` (below) |
| 12  | 2    | fps | 1–30 (15) |
| 14  | 2    | audioChannels | 1 |
| 16  | 4    | audioRateHz | 22050; must be divisible by fps |
| 20  | 4    | frameCount | > 0 |
| 24  | 4    | durationMs | frameCount * 1000 / fps |
| 28  | 4    | indexOffset | must satisfy indexOffset > dataOffset and indexOffset + 4*frameCount <= file size |
| 32  | 4    | dataOffset | >= 64 |
| 36  | 4    | maxFrameBytes | largest video payload; <= 98304 (96 KB) |
| 40  | 1    | orientation | 0 = rotated, 1 = upright |
| 41  | 23   | reserved | zero |

## Orientation

Byte 40 was claimed from the reserved block without a version bump, under the
rule the block exists for: every packer written before it wrote zero there, and
zero is defined as `rotated` — the behavior those files already had. Bytes
41..63 stay reserved on the same terms.

| orientation | packed for | max stored | 16:9 lands at | 4:3 lands at |
|---|---|---|---|---|
| 0 rotated | 448×310 landscape box, then `transpose=1` (90° CW) | 310 × 448 | 252×448 | 310×414 |
| 1 upright | 368×312 box above the horizontal chrome band, no rotation | 368 × 312 | 368×208 | 368×276 |

Frames are fit by aspect ratio, never padded and never cropped.
`scripts/lcv_mux.py` derives width/height from the first frame's JPEG SOF
marker rather than trusting a caller-supplied value, and rejects a file whose
geometry exceeds the box its declared orientation was packed for.

The device picks its layout from the `videoorient` setting, not from the file.
When the two disagree it turns and scales each frame during decode, so a file
packed either way plays in either mode; matching them just skips that work.

## Chunks

4-byte chunk header: byte 0 = type (1 = video, 2 = audio), bytes 1–3 =
payload size, LE24. Payload follows, padded to a 4-byte boundary (padding
bytes are not counted in the size).

A **frame group** = one video chunk (one complete baseline JPEG, stored in the
orientation the header declares) followed by one audio chunk holding exactly
audioRateHz/fps mono s16 samples
(the final group may be shorter; the muxer zero-pads it).

## Index

frameCount × uint32 LE at indexOffset: the file offset of each frame
group's video chunk header. Never loaded into RAM on device — a seek reads
4 bytes at indexOffset + 4*N.
