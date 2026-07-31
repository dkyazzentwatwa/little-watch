#!/usr/bin/env python3
"""Mux ffmpeg output into a .lcv container (docs/lcv-format.md).

mux:     concatenated-MJPEG stream + raw s16le mono PCM  ->  .lcv
inspect: print and sanity-check a .lcv header and index
"""
import argparse
import os
import struct
import sys

MAGIC = b"LCV1"
VERSION = 1
HEADER_BYTES = 64
CHUNK_VIDEO = 1
CHUNK_AUDIO = 2
MAX_FRAME_BYTES = 96 * 1024

# The panel's picture area, left of the player chrome strip (see
# little-cube-os/src/board_config.h, "video player layout"): rotated frames
# stored on disk must fit within 310 wide x 448 tall.
MAX_STORED_WIDTH = 310
MAX_STORED_HEIGHT = 448

# JPEG SOF (start-of-frame) marker family: baseline (C0), extended
# sequential (C1), progressive (C2). ffmpeg's mjpeg encoder emits C0.
SOF_MARKERS = {0xC0, 0xC1, 0xC2}


def jpeg_dimensions(data: bytes):
    """Parse a JPEG's SOF marker and return (width, height).

    Walks segments from the start of image: skip the SOI (FFD8), then for
    each marker read its 2-byte length and skip the payload, until an SOF
    marker is found. The SOF payload is
    [precision:1][height:2][width:2], big-endian.
    """
    if len(data) < 4 or data[0:2] != b"\xff\xd8":
        sys.exit("error: frame does not start with a JPEG SOI marker")
    i = 2
    n = len(data)
    while i + 4 <= n:
        if data[i] != 0xFF:
            sys.exit(f"error: expected marker at offset {i}, found 0x{data[i]:02x}")
        marker = data[i + 1]
        if marker in SOF_MARKERS:
            if i + 9 > n:
                sys.exit("error: truncated SOF segment")
            height = (data[i + 5] << 8) | data[i + 6]
            width = (data[i + 7] << 8) | data[i + 8]
            return width, height
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            # No length field on SOI/TEM/RSTn — shouldn't recur here, but
            # skip just the marker rather than mis-reading a length.
            i += 2
            continue
        if i + 4 > n:
            sys.exit("error: truncated marker segment")
        seg_len = (data[i + 2] << 8) | data[i + 3]
        i += 2 + seg_len
    sys.exit("error: no SOF marker found in first frame")


def split_jpegs(data: bytes):
    """Split a concatenated MJPEG stream on SOI/EOI markers.

    Inside JPEG entropy-coded data every 0xFF is stuffed with 0x00, so a
    literal FFD9 is a real end-of-image — this holds for ffmpeg's plain
    mjpeg encoder output (short APP0/COM only), not for arbitrary JPEGs
    that may carry FFD9 bytes inside large APPn/EXIF/ICC payloads.
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
    if args.fps < 1:
        sys.exit("error: --fps must be >= 1")
    if args.rate < 1:
        sys.exit("error: --rate must be >= 1")
    try:
        with open(args.video, "rb") as f:
            frames = split_jpegs(f.read())
        with open(args.audio, "rb") as f:
            pcm = f.read()
    except OSError as e:
        sys.exit(f"error: {e}")
    if not frames:
        sys.exit("error: no JPEG frames found in video stream")
    if args.rate % args.fps != 0:
        sys.exit(f"error: rate {args.rate} not divisible by fps {args.fps}")
    max_frame = max(len(f) for f in frames)
    if max_frame > MAX_FRAME_BYTES:
        sys.exit(f"error: largest frame is {max_frame} B (> {MAX_FRAME_BYTES}); "
                 "raise -q:v (lower quality) and re-run ffmpeg")

    width, height = jpeg_dimensions(frames[0])
    if args.width is not None and args.width != width:
        sys.exit(f"error: --width {args.width} does not match parsed frame width {width}")
    if args.height is not None and args.height != height:
        sys.exit(f"error: --height {args.height} does not match parsed frame height {height}")
    if width > MAX_STORED_WIDTH or height > MAX_STORED_HEIGHT:
        sys.exit(f"error: frame is {width}x{height}, exceeds the panel's picture area "
                 f"({MAX_STORED_WIDTH}x{MAX_STORED_HEIGHT} max) — check the packer's scale filter")

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
        width, height, args.fps, 1,
        args.rate, len(frames), len(frames) * 1000 // args.fps,
        index_offset, HEADER_BYTES, max_frame)
    assert len(header) == HEADER_BYTES
    out[:HEADER_BYTES] = header

    tmp_path = args.output + ".tmp"
    try:
        with open(tmp_path, "wb") as f:
            f.write(out)
        os.replace(tmp_path, args.output)
    except OSError as e:
        sys.exit(f"error: {e}")
    print(f"wrote {args.output}: {width}x{height}, {len(frames)} frames, "
          f"{len(frames) * 1000 // args.fps} ms, maxFrame {max_frame} B, "
          f"{len(out)} B total")


def inspect(args):
    try:
        with open(args.file, "rb") as f:
            data = f.read()
    except OSError as e:
        sys.exit(f"error: {e}")
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
          and index_offset > data_offset
          and index_offset + 4 * frame_count <= len(data)
          and 0 < max_frame <= MAX_FRAME_BYTES)
    # Every index entry must point at a video chunk header.
    if ok:
        for n in range(frame_count):
            entry_off = index_offset + 4 * n
            if entry_off + 4 > len(data):
                print(f"BAD index entry {n}: entry out of bounds")
                ok = False
                break
            off = struct.unpack_from("<I", data, entry_off)[0]
            if off < data_offset or off + 4 > index_offset or data[off] != CHUNK_VIDEO:
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
    m.add_argument("--width", type=int, default=None,
                    help="optional: must match the width parsed from the first JPEG frame")
    m.add_argument("--height", type=int, default=None,
                    help="optional: must match the height parsed from the first JPEG frame")
    m.add_argument("-o", "--output", required=True)
    m.set_defaults(func=mux)
    i = sub.add_parser("inspect")
    i.add_argument("file")
    i.set_defaults(func=inspect)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
