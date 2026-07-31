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
