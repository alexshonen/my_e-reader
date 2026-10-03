#!/usr/bin/env python3
"""
tools/convert.py: High-Quality E-Book (EPUB & PDF) to Raw 4-bit Binary Converter
for LilyGo T5 4.7" S3 E-Paper Display (ED047TC1).

Supported Formats:
  - EPUB (and reflowable e-books: MOBI, FB2, HTML, TXT):
      Automatically detected and reflowed to the exact 540x960 portrait screen.
      Text size can be customized dynamically just like on a Kindle (--font-size).
  - PDF:
      Vector rendering with 2x supersampling and proportional fitting.

Binary Format Specification:
  - Resolution: 540 (width) x 960 (height) portrait
  - Color Depth: 4 bits per pixel (16 levels of gray: 0x0=black, 0xF=white)
  - Packing: 2 pixels per byte (Pixel 2k in high nibble, Pixel 2k+1 in low nibble)
  - File Size: (540 * 960) / 2 = 259,200 bytes
  - Naming: page_0000.bin, page_0001.bin, ...
"""

import argparse
import os
import sys
from pathlib import Path

try:
    import pymupdf as fitz
except ImportError:
    try:
        import fitz  # Legacy PyMuPDF import
    except ImportError:
        print("[ERROR] PyMuPDF is not installed. Run: pip install pymupdf", file=sys.stderr)
        sys.exit(1)

try:
    from PIL import Image, ImageEnhance
except ImportError:
    print("[ERROR] Pillow is not installed. Run: pip install Pillow", file=sys.stderr)
    sys.exit(1)

# Display hardware constants
TARGET_WIDTH = 540
TARGET_HEIGHT = 960
EXPECTED_BYTE_SIZE = (TARGET_WIDTH * TARGET_HEIGHT) // 2  # 259,200 bytes

# Font size presets (scaled up for 4.7" 234 PPI e-paper visibility)
FONT_PRESETS = {
    "xsmall": 14.0,
    "small": 17.0,
    "medium": 21.0,
    "default": 21.0,
    "large": 28.0,
    "xlarge": 36.0,
    "xxlarge": 48.0,
}


def parse_font_size(size_str: str) -> float:
    """
    Parses font size argument, accepting either a numeric point size (e.g. '14', '16.5')
    or a Kindle-style named preset ('small', 'medium', 'large', 'xlarge').
    """
    key = size_str.strip().lower()
    if key in FONT_PRESETS:
        return FONT_PRESETS[key]
    try:
        val = float(size_str)
        if val <= 4.0 or val > 72.0:
            raise ValueError
        return val
    except ValueError:
        presets_list = ", ".join(f"'{k}' ({v:.0f}pt)" for k, v in FONT_PRESETS.items() if k != "default")
        raise argparse.ArgumentTypeError(
            f"Invalid font size '{size_str}'. Choose a point size between 5 and 72, or a preset: {presets_list}"
        )


def pack_4bit_pixels(raw_pixels: bytes) -> bytes:
    """
    Packs 8-bit values (in range 0..15) into 4-bit nibbles.
    High nibble (byte >> 4): Pixel 2k
    Low nibble (byte & 0x0F): Pixel 2k + 1
    """
    assert len(raw_pixels) == TARGET_WIDTH * TARGET_HEIGHT, (
        f"Expected {TARGET_WIDTH * TARGET_HEIGHT} pixels, got {len(raw_pixels)}"
    )

    p_even = raw_pixels[0::2]
    p_odd = raw_pixels[1::2]

    packed = bytes(((e & 0x0F) << 4) | (o & 0x0F) for e, o in zip(p_even, p_odd))
    assert len(packed) == EXPECTED_BYTE_SIZE, (
        f"Packed size mismatch: {len(packed)} vs expected {EXPECTED_BYTE_SIZE}"
    )
    return packed


def render_page_to_4bit(
    page: fitz.Page,
    contrast: float = 1.0,
    fit_mode: str = "fit",
    is_reflowable: bool = False
) -> bytes:
    """
    Renders a single document page at 2x supersampling, applies Lanczos downsampling to 540x960,
    adjusts contrast, performs linear 8-bit to 4-bit grayscale quantization,
    and returns exactly 259,200 packed bytes.
    """
    rect = page.rect
    page_w, page_h = rect.width, rect.height

    if is_reflowable:
        # Reflowable document (EPUB): Already formatted to exact 540x960 portrait geometry.
        # Render at 2x supersampling (1080x1920) for crisp typography antialiasing
        scale_x = (TARGET_WIDTH * 2.0) / page_w
        scale_y = (TARGET_HEIGHT * 2.0) / page_h
        mat = fitz.Matrix(scale_x, scale_y)
        pix = page.get_pixmap(matrix=mat, alpha=False)
        img_2x = Image.frombytes("RGB", (pix.width, pix.height), pix.samples).convert("L")
        canvas = img_2x.resize((TARGET_WIDTH, TARGET_HEIGHT), resample=Image.Resampling.LANCZOS)
    elif fit_mode == "stretch":
        # Force stretch to 540x960
        scale_x = (TARGET_WIDTH * 2.0) / page_w
        scale_y = (TARGET_HEIGHT * 2.0) / page_h
        mat = fitz.Matrix(scale_x, scale_y)
        pix = page.get_pixmap(matrix=mat, alpha=False)
        img_2x = Image.frombytes("RGB", (pix.width, pix.height), pix.samples).convert("L")
        canvas = img_2x.resize((TARGET_WIDTH, TARGET_HEIGHT), resample=Image.Resampling.LANCZOS)
    else:
        # Proportional fit centered on 540x960 pure white canvas
        scale = 2.0 * min(TARGET_WIDTH / page_w, TARGET_HEIGHT / page_h)
        mat = fitz.Matrix(scale, scale)
        pix = page.get_pixmap(matrix=mat, alpha=False)
        img_2x = Image.frombytes("RGB", (pix.width, pix.height), pix.samples).convert("L")

        downsampled_w = max(1, round(img_2x.width / 2.0))
        downsampled_h = max(1, round(img_2x.height / 2.0))
        img_downsampled = img_2x.resize((downsampled_w, downsampled_h), resample=Image.Resampling.LANCZOS)

        canvas = Image.new("L", (TARGET_WIDTH, TARGET_HEIGHT), color=255)
        offset_x = (TARGET_WIDTH - downsampled_w) // 2
        offset_y = (TARGET_HEIGHT - downsampled_h) // 2
        canvas.paste(img_downsampled, (offset_x, offset_y))

    # Optional contrast enhancement (default: 1.0 = unchanged)
    if contrast != 1.0:
        enhancer = ImageEnhance.Contrast(canvas)
        canvas = enhancer.enhance(contrast)

    # Linear mapping from 8-bit grayscale (0..255) to 4-bit values (0..15)
    # 0 = pure black (0x0), 255 = pure white (0xF)
    lut = [round(i * 15.0 / 255.0) for i in range(256)]
    img_4bit = canvas.point(lut)

    # Pack into raw 4-bit binary stream (2 pixels per byte)
    raw_pixels = img_4bit.tobytes()
    return pack_4bit_pixels(raw_pixels)


def parse_page_ranges(pages_arg: str, total_pages: int) -> list[int]:
    """Parses page numbers (1-indexed for user friendly CLI) to 0-indexed list."""
    if not pages_arg:
        return list(range(total_pages))

    result = set()
    for part in pages_arg.split(","):
        part = part.strip()
        if "-" in part:
            start_str, end_str = part.split("-", 1)
            start = max(1, int(start_str))
            end = min(total_pages, int(end_str))
            for p in range(start, end + 1):
                result.add(p - 1)
        else:
            p = int(part)
            if 1 <= p <= total_pages:
                result.add(p - 1)
    return sorted(list(result))


def main():
    parser = argparse.ArgumentParser(
        description="Convert an e-book (EPUB or PDF) into 4-bit raw grayscale binary pages for LilyGo T5 4.7\" S3."
    )
    parser.add_argument(
        "--input", "-i",
        required=True,
        help="Path to the source e-book file (.epub, .pdf, .mobi, .fb2, etc.)."
    )
    parser.add_argument(
        "--output", "-o",
        default=None,
        help="Target output directory (default: 'books/<book_name>/' or 'pages/')."
    )
    parser.add_argument(
        "--title", "-t",
        default=None,
        help="Book title for library display (default: extracted from metadata or input filename)."
    )
    parser.add_argument(
        "--font-size", "-s", "--fontsize",
        type=str,
        default="default",
        help="Text size for EPUB / reflowable books (presets: 'small', 'medium', 'large', 'xlarge', 'xxlarge'). Default: 'default' (medium)."
    )
    parser.add_argument(
        "--contrast", "-c",
        type=float,
        default=1.0,
        help="Contrast multiplier (e.g. 1.0 = normal, 1.25 = higher contrast, default: 1.0)."
    )
    parser.add_argument(
        "--fit-mode",
        choices=["fit", "stretch"],
        default="fit",
        help="PDF page layout: 'fit' keeps aspect ratio with white borders; 'stretch' fills 540x960 (default: fit)."
    )
    parser.add_argument(
        "--pages",
        type=str,
        default=None,
        help="Optional page range (1-indexed, e.g. '1-10,15,20-25'). Default: all pages."
    )

    args = parser.parse_args()

    input_path = Path(args.input)
    if not input_path.is_file():
        print(f"[ERROR] Input file does not exist: {input_path}", file=sys.stderr)
        sys.exit(1)

    print(f"[INFO] Opening document: {input_path.name}")
    doc = fitz.open(str(input_path))

    # Resolve Book Title
    book_title = args.title
    if not book_title:
        meta_title = doc.metadata.get("title") if hasattr(doc, "metadata") else None
        if meta_title and meta_title.strip():
            book_title = meta_title.strip()
        else:
            book_title = input_path.stem.replace("_", " ").title()

    # Resolve Output Directory: default to books/<slug>/
    if args.output:
        output_dir = Path(args.output)
    else:
        # Create a clean folder name for the book
        safe_folder_name = "".join(c if c.isalnum() or c in ("-", "_") else "_" for c in input_path.stem).strip("_")
        output_dir = Path("books") / safe_folder_name

    output_dir.mkdir(parents=True, exist_ok=True)

    # Save title.txt for the on-device library menu
    title_file = output_dir / "title.txt"
    with open(title_file, "w", encoding="utf-8") as tf:
        tf.write(book_title + "\n")
    print(f"[INFO] Book Title: '{book_title}' (saved to {title_file})")

    # Detect whether the document is reflowable (EPUB, MOBI, FB2, etc.) or fixed-layout (PDF)
    ext = input_path.suffix.lower()
    is_reflowable = getattr(doc, "is_reflowable", False) or ext in [".epub", ".mobi", ".fb2", ".xhtml", ".html", ".txt"]

    if is_reflowable:
        font_size = parse_font_size(args.font_size)
        print(f"[INFO] Format detected: EPUB / Reflowable e-book ({ext or 'epub'})")
        print(f"[INFO] Applying Kindle-like layout: {TARGET_WIDTH}x{TARGET_HEIGHT} portrait, font-size={font_size:.1f}pt...")
        # MuPDF recalculates exact page count and line breaks according to screen geometry and font size
        doc.layout(width=TARGET_WIDTH, height=TARGET_HEIGHT, fontsize=font_size)
    else:
        print(f"[INFO] Format detected: Fixed-layout document ({ext or 'pdf'})")
        if args.font_size != "14" and args.font_size != "default":
            print("[NOTICE] '--font-size' is only applicable to reflowable formats (e.g. EPUB). Fixed PDF pages are scaled to fit.")

    total_pages = len(doc)
    print(f"[INFO] Document formatted into {total_pages} page(s).")

    pages_to_convert = parse_page_ranges(args.pages, total_pages)
    print(f"[INFO] Converting {len(pages_to_convert)} page(s) to '{output_dir}/'...")
    print(f"[INFO] Output format: {TARGET_WIDTH}x{TARGET_HEIGHT} 4-bit grayscale ({EXPECTED_BYTE_SIZE:,} bytes/page)")

    successful_count = 0
    for out_idx, page_idx in enumerate(pages_to_convert):
        page = doc[page_idx]
        packed_data = render_page_to_4bit(
            page=page,
            contrast=args.contrast,
            fit_mode=args.fit_mode,
            is_reflowable=is_reflowable
        )

        # Strict validation before saving
        assert len(packed_data) == EXPECTED_BYTE_SIZE, (
            f"Page {page_idx+1} invalid binary size: {len(packed_data)} != {EXPECTED_BYTE_SIZE}"
        )

        out_filename = f"page_{out_idx:04d}.bin"
        out_filepath = output_dir / out_filename

        with open(out_filepath, "wb") as f:
            f.write(packed_data)

        successful_count += 1
        print(f"  [OK] Page {page_idx+1:>4} -> {out_filename} ({len(packed_data):,} bytes)")

    doc.close()
    
    pages_file = output_dir / "pages.txt"
    with open(pages_file, "w", encoding="utf-8") as pf:
        pf.write(str(successful_count) + "\n")

    print(f"\n[DONE] Successfully generated {successful_count} page(s) in '{output_dir}' (Metadata saved)")
    print(f"       To read on your device:")
    print(f"       - Multi-book: Copy '{output_dir}' into '/books/' on your MicroSD card.")
    print(f"       - Single-book: Copy the .bin files directly into '/pages/' on your MicroSD card.")


if __name__ == "__main__":
    main()
