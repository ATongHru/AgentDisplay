#!/usr/bin/env python3
"""Build animations.bin (flash partition) and anim_size.h from emoji GIFs."""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

from PIL import Image

root = Path(__file__).resolve().parents[1]
source_dir = root / "third_party" / "emoji-gif"
out_bin = root / "firmware" / "data" / "animations.bin"
out_size = root / "main" / "anim_size.h"
ANIM_PARTITION_MAX = 0x470000
names = [
    "idle", "thinking", "coding", "reading", "testing", "waiting",
    "done", "error", "offline", "stale", "unknown",
    "tool", "ear", "speaking",
]
WIDTH = HEIGHT = 90
MAX_FRAMES = 40
MIN_FRAMES = 15
TARGET_FPS = 15
FRAME_MS = max(20, int(1000 / TARGET_FPS))
BG = (0x10, 0x15, 0x1B)
MAGIC = b"AGIF"
VERSION = 1


def center_square_crop(image: Image.Image) -> Image.Image:
    w, h = image.size
    side = min(w, h)
    left = (w - side) // 2
    top = (h - side) // 2
    return image.crop((left, top, left + side, top + side))


def sample_animation(frames, durations):
    count = len(frames)
    target = max(MIN_FRAMES, min(MAX_FRAMES, count))
    if count != target:
        picked_frames = []
        picked_durations = []
        for i in range(target):
            src = round(i * (count - 1) / (target - 1)) if target > 1 else 0
            src = min(count - 1, max(0, src))
            picked_frames.append(frames[src])
            picked_durations.append(FRAME_MS)
        return picked_frames, picked_durations
    return frames, [FRAME_MS] * count


def blend_channel(src: int, alpha: int, bg: int) -> int:
    return (src * alpha + bg * (255 - alpha)) // 255


def to_rgb565_pixel(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def composite_frame(frame: Image.Image):
    rgba = frame.convert("RGBA")
    pixels = []
    for y in range(HEIGHT):
        for x in range(WIDTH):
            r, g, b, a = rgba.getpixel((x, y))
            if a == 0:
                rr, gg, bb = BG
            elif a == 255:
                rr, gg, bb = r, g, b
            else:
                rr = blend_channel(r, a, BG[0])
                gg = blend_channel(g, a, BG[1])
                bb = blend_channel(b, a, BG[2])
            pixels.append(to_rgb565_pixel(rr, gg, bb))
    return pixels


def rgb565_rle(values):
    out = bytearray()
    i = 0
    while i < len(values):
        value = values[i]
        count = 1
        while i + count < len(values) and values[i + count] == value and count < 255:
            count += 1
        out += bytes([count, value >> 8, value & 0xFF])
        i += count
    return bytes(out)


def build_blob(source_dir: Path) -> tuple[bytes, int]:
    anims = []
    total_frames = 0
    for name in names:
        source_path = source_dir / f"{name.upper()}.gif"
        if not source_path.is_file():
            raise FileNotFoundError(f"missing animation source: {source_path}")
        with Image.open(source_path) as source:
            frames = []
            durations = []
            for index in range(source.n_frames):
                source.seek(index)
                rgba = center_square_crop(source).convert("RGBA").resize(
                    (WIDTH, HEIGHT), Image.Resampling.LANCZOS
                )
                frames.append(rgba)
                durations.append(max(20, source.info.get("duration", 100)))
            frames, durations = sample_animation(frames, durations)
        encoded = [rgb565_rle(composite_frame(frame)) for frame in frames]
        avg = sum(len(x) for x in encoded) // len(encoded)
        print(f"{name}: {len(encoded)} frames, avg {avg} bytes")
        anims.append((durations, encoded))
        total_frames += len(encoded)

    header_size = 12 + 6 * len(anims)
    table_size = sum(8 * len(enc) for _, enc in anims)
    payload_start = header_size + table_size

    frame_tables = bytearray()
    payload = bytearray()
    anim_index = []
    table_offset = header_size
    for durations, encoded in anims:
        anim_index.append((len(encoded), table_offset))
        for dur, chunk in zip(durations, encoded):
            frame_tables += struct.pack("<HHI", dur, len(chunk), payload_start + len(payload))
            payload += chunk
        table_offset += 8 * len(encoded)

    blob = bytearray()
    blob += struct.pack("<4sHHHH", MAGIC, VERSION, len(anims), WIDTH, HEIGHT)
    for count, off in anim_index:
        blob += struct.pack("<HI", count, off)
    blob += frame_tables
    blob += payload

    return bytes(blob), total_frames


def main() -> int:
    parser = argparse.ArgumentParser(description="Build animations.bin for ESP32 animations partition")
    parser.add_argument("--source-dir", type=Path, default=source_dir)
    parser.add_argument("--out-bin", type=Path, default=out_bin)
    parser.add_argument("--out-size", type=Path, default=out_size)
    parser.add_argument("--max-bytes", type=int, default=ANIM_PARTITION_MAX)
    args = parser.parse_args()

    try:
        blob, total_frames = build_blob(args.source_dir)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if len(blob) > args.max_bytes:
        print(
            f"animations.bin too large: {len(blob)} bytes > partition {args.max_bytes} (0x{args.max_bytes:X})",
            file=sys.stderr,
        )
        return 3

    args.out_bin.parent.mkdir(parents=True, exist_ok=True)
    args.out_bin.write_bytes(blob)
    args.out_size.parent.mkdir(parents=True, exist_ok=True)
    args.out_size.write_text(
        "#pragma once\n"
        f"// Auto-generated by scripts/gen_frame_player.py\n"
        f"#define ANIM_BIN_SIZE {len(blob)}u\n"
        f"#define ANIM_FACE_SIZE {WIDTH}u\n"
        f"#define ANIM_FRAME_COUNT {total_frames}u\n",
        encoding="utf-8",
    )
    print(f"generated {args.out_bin.name} ({len(blob)} bytes, {total_frames} frames, face {WIDTH}px)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
