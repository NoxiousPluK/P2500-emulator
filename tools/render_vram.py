#!/usr/bin/env python3
"""
Render a video-RAM dump (as produced by ../p2000b-emu --dump-vram) as an
actual image, using the real dumped character ROM and the CRTC geometry
established for this platform: 80 columns x 24 rows, 8x12 character cells.

The glyph is the full 8x12 cell, not an 8x8 bitmap. The character ROM's
stride is 16 bytes per code (4096 bytes / 256 codes), of which rows 0-11
are the glyph the CRTC clocks out (R9 = 11, i.e. 12 scanlines per row) and
rows 12-15 are unused padding - which is where the character ROM's dump
found packed Z80 code stuffed into the low code points. Reading only 8
rows silently truncated every descender: 'p' has its descender at rows
8-9, 'g' at rows 8-10.

Video RAM layout: 1 byte per character cell, row-major,
starting at $8000 (offset 0 in the 16KB video RAM dump) - matches the
MC6845's R12/R13 "start address = 0" and is what the banner-print trace
actually produced (all four table entries landed at exactly the addresses
predicted by treating $8000+row*80+col as the addressing scheme). The card also carries a 4-bit attribute plane per cell (TODO.md T27) -
pass its dump as --attr to render it. No software this project holds ever
writes attributes, so a normal dump has none and the plain path is used.

Usage: python3 render_vram.py <vram_dump.bin> [out.png] [--attr PLANE.bin]
       python3 render_vram.py <vram_dump.bin> out.png --demo-attrs

--demo-attrs synthesises an attribute plane that applies one attribute per
screen row. It exists because no software this project holds writes
attributes, so it is the only way to exercise this path at all.
"""

import sys
from pathlib import Path
from PIL import Image

ROM_PATH = Path(__file__).parent.parent / "roms" / "charrom.bin"

COLS, ROWS = 80, 24
CELL_W, CELL_H = 8, 12  # confirmed CRTC cell: 8 pixels wide, 12 scanlines tall
ROM_STRIDE = 16         # bytes per code in the character ROM; rows 12-15 unused
PX = 6  # upscale factor for a legible PNG

FG = (0, 255, 70)   # green phosphor - period-plausible for this class of terminal
BG = (8, 12, 8)
FG_DIM = (0, 140, 40)

# The four documented attributes (P2219 CP/M manual, via ../Information from
# the internet/findings.md): underline, reverse, flash, low intensity. Which
# nibble bit carries which is NOT established - see TODO.md T27. These
# assignments are a placeholder and are the single place to correct.
ATTR_UNDERLINE = 0x01
ATTR_REVERSE   = 0x02
ATTR_FLASH     = 0x04
ATTR_DIM       = 0x08


def glyph_bitmap(rom: bytes, code: int) -> bytes:
    """The full 12-scanline cell for a character code."""
    off = code * ROM_STRIDE
    return rom[off:off + CELL_H]


def demo_attribute_plane(vram: bytes) -> bytes:
    """One attribute per row, cycling, so each can be checked by eye."""
    order = [0, ATTR_DIM, ATTR_REVERSE, ATTR_UNDERLINE, ATTR_FLASH,
             ATTR_REVERSE | ATTR_UNDERLINE]
    plane = bytearray(len(vram))
    for row in range(ROWS):
        attr = order[row % len(order)]
        for col in range(COLS):
            plane[row * COLS + col] = attr
    return bytes(plane)


def main() -> None:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <vram_dump.bin> [out.png]")
        raise SystemExit(1)
    args = sys.argv[1:]
    demo_attrs = "--demo-attrs" in args
    if demo_attrs:
        args.remove("--demo-attrs")
    attr_path = None
    if "--attr" in args:
        i = args.index("--attr")
        attr_path = Path(args[i + 1])
        del args[i:i + 2]
    vram_path = Path(args[0])
    out_path = Path(args[1]) if len(args) > 1 else vram_path.with_suffix(".png")

    rom = ROM_PATH.read_bytes()
    vram = vram_path.read_bytes()
    attrs = attr_path.read_bytes() if attr_path else b""
    if demo_attrs:
        attrs = demo_attribute_plane(vram)

    img = Image.new("RGB", (COLS * CELL_W * PX, ROWS * CELL_H * PX), BG)
    px = img.load()

    for row in range(ROWS):
        for col in range(COLS):
            addr = row * COLS + col
            if addr >= len(vram):
                continue
            code = vram[addr]
            attr = attrs[addr] & 0x0F if addr < len(attrs) else 0
            if code == 0x00 and attr == 0:
                continue  # cleared background - not rendered as a glyph
            bitmap = glyph_bitmap(rom, code)
            fg = FG_DIM if attr & ATTR_DIM else FG
            bg = BG
            if attr & ATTR_REVERSE:
                fg, bg = bg, fg
            cell_x0 = col * CELL_W * PX
            cell_y0 = row * CELL_H * PX
            for gy in range(CELL_H):
                byte = bitmap[gy] if gy < len(bitmap) else 0
                if (attr & ATTR_UNDERLINE) and gy == CELL_H - 2:
                    byte = 0xFF
                for gx in range(CELL_W):
                    on = (byte >> (7 - gx)) & 1
                    colour = fg if on else bg
                    if colour == BG:
                        continue
                    x0 = cell_x0 + gx * PX
                    y0 = cell_y0 + gy * PX
                    for dy in range(PX):
                        for dx in range(PX):
                            px[x0 + dx, y0 + dy] = colour

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
