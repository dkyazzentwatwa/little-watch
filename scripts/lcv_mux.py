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
