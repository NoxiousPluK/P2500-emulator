#!/usr/bin/env python3
"""
Render a video-RAM dump (as produced by ../p2000b-emu --dump-vram) as an
actual image, using the real dumped character ROM and the CRTC geometry
established for this platform: 80 columns x 24 rows, 8x12 character cells
(8x8 glyph bitmap, confirmed by three independent sources - see the parent
research project's ROM Dumps/CPU-Card-Boot-EPROM/emulation/findings.md).

Video RAM layout assumed: flat 1 byte per character cell, row-major,
starting at $8000 (offset 0 in the 16KB video RAM dump) - matches the
MC6845's R12/R13 "start address = 0" and is what the banner-print trace
actually produced (all four table entries landed at exactly the addresses
predicted by treating $8000+row*80+col as the addressing scheme). Whether
real video RAM also carries a separate attribute nibble per character is
still unconfirmed project-wide; this renderer ignores attributes entirely
(plain glyph render, foreground on background) since none of the captured
content exercises them.

Usage: python3 render_vram.py <vram_dump.bin> [out.png]
"""

import sys
from pathlib import Path
from PIL import Image

ROM_PATH = Path(__file__).parent.parent / "roms" / "charrom.bin"

COLS, ROWS = 80, 24
GLYPH_W, GLYPH_H = 8, 8
CELL_W, CELL_H = 8, 12  # confirmed CRTC cell: 8-pixel-wide, 12 scanlines tall
PX = 6  # upscale factor for a legible PNG

FG = (0, 255, 70)   # green phosphor - period-plausible for this class of terminal
BG = (8, 12, 8)


def glyph_bitmap(rom: bytes, code: int) -> bytes:
    slot = 2 * code
    off = slot * 8
    return rom[off:off + 8]


def main() -> None:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <vram_dump.bin> [out.png]")
        raise SystemExit(1)
    vram_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else vram_path.with_suffix(".png")

    rom = ROM_PATH.read_bytes()
    vram = vram_path.read_bytes()

    img = Image.new("RGB", (COLS * CELL_W * PX, ROWS * CELL_H * PX), BG)
    px = img.load()

    for row in range(ROWS):
        for col in range(COLS):
            addr = row * COLS + col
            if addr >= len(vram):
                continue
            code = vram[addr]
            if code == 0x00:
                continue  # cleared background - not rendered as a glyph
            bitmap = glyph_bitmap(rom, code)
            cell_x0 = col * CELL_W * PX
            cell_y0 = row * CELL_H * PX
            for gy, byte in enumerate(bitmap):
                for gx in range(GLYPH_W):
                    if (byte >> (7 - gx)) & 1:
                        x0 = cell_x0 + gx * PX
                        y0 = cell_y0 + gy * PX  # top-aligned within the 12-line cell
                        for dy in range(PX):
                            for dx in range(PX):
                                px[x0 + dx, y0 + dy] = FG

    img.save(out_path)
    print(f"Wrote {out_path} ({img.width}x{img.height})")

    # Text-mode dump for a quick sanity check without opening the image
    print("\nText-mode preview (blank rows collapsed):")
    for row in range(ROWS):
        line = "".join(
            chr(vram[row * COLS + col]) if 32 <= vram[row * COLS + col] < 127 else
            (" " if vram[row * COLS + col] == 0 else "?")
            for col in range(COLS)
        )
        if line.strip():
            print(f"  {row:2d}: {line.rstrip()}")


if __name__ == "__main__":
    main()
