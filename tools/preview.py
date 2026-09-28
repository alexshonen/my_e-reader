#!/usr/bin/env python3
"""
tools/preview.py: Binary to PNG Visualizer and Inspector
for LilyGo T5 4.7" S3 E-Paper Display (ED047TC1).

Reads a 259,200-byte raw 4-bit binary file and reconstructs an 8-bit grayscale
PNG image (540x960) using the reverse linear scaling formula: pixel_8bit = v * 17.
"""

import argparse
import os
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("[ERROR] Pillow is not installed. Run: pip install Pillow", file=sys.stderr)
    sys.exit(1)

TARGET_WIDTH = 540
TARGET_HEIGHT = 960
EXPECTED_BYTE_SIZE = (TARGET_WIDTH * TARGET_HEIGHT) // 2  # 259,200 bytes


def unpack_bin_to_image(bin_data: bytes) -> Image.Image:
    """
    Unpacks raw 4-bit packed binary stream into an 8-bit Pillow Image.
    Each byte contains 2 pixels:
      - High nibble: Pixel 2k
      - Low nibble:  Pixel 2k + 1
    Scaled to 8-bit range [0..255] via v * 17 (0 -> 0, 15 -> 255).
    """
    if len(bin_data) != EXPECTED_BYTE_SIZE:
        raise ValueError(
            f"Invalid file size: expected exactly {EXPECTED_BYTE_SIZE:,} bytes, "
            f"got {len(bin_data):,} bytes."
        )

    unpacked_pixels = bytearray(TARGET_WIDTH * TARGET_HEIGHT)
    for i, b in enumerate(bin_data):
        high_val = ((b >> 4) & 0x0F) * 17
        low_val = (b & 0x0F) * 17
        unpacked_pixels[2 * i] = high_val
        unpacked_pixels[2 * i + 1] = low_val

    return Image.frombytes("L", (TARGET_WIDTH, TARGET_HEIGHT), bytes(unpacked_pixels))


def main():
    parser = argparse.ArgumentParser(
        description="Inspect and convert a 4-bit raw .bin page to a viewable PNG image."
    )
    parser.add_argument(
        "input",
        help="Path to the .bin page file (e.g. pages/page_0000.bin)."
    )
    parser.add_argument(
        "--output", "-o",
        default=None,
        help="Path to save the preview PNG (default: <input_filename>.png)."
    )
    parser.add_argument(
        "--show", "-s",
        action="store_true",
        help="Open image immediately using system default viewer."
    )

    args = parser.parse_args()

    input_path = Path(args.input)
    if not input_path.is_file():
        print(f"[ERROR] File not found: {input_path}", file=sys.stderr)
        sys.exit(1)

    print(f"[INFO] Reading binary file: {input_path} ({input_path.stat().st_size:,} bytes)...")
    try:
        with open(input_path, "rb") as f:
            data = f.read()
        image = unpack_bin_to_image(data)
    except Exception as e:
        print(f"[ERROR] Failed to unpack binary file: {e}", file=sys.stderr)
        sys.exit(1)

    output_path = Path(args.output) if args.output else input_path.with_suffix(".png")
    image.save(output_path, format="PNG")
    print(f"[SUCCESS] Saved preview image to: {output_path} ({image.width}x{image.height})")

    if args.show:
        print("[INFO] Opening image preview in system viewer...")
        image.show()


if __name__ == "__main__":
    main()
