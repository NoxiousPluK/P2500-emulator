# Graphics demos

Three small programs for the P2500's 512 × 256 high-resolution mode, written
to exercise it with something harder than a test pattern.

```
make demos                       # assembles, verifies, and builds the disk
./p2500-gui --disk demos/P2500DEMO.raw
A>LOGO          A>STARS          A>SPIRO
```

Any key returns to CP/M. `make demos` needs a bootable P2500 image to take
the CP/M system files from; it defaults to `P25K_B` and takes
`BOOT_DONOR=path` otherwise.

| | |
|---|---|
| **LOGO** | The P2000 wordmark, bouncing. Eight pre-shifted copies of the sprite give single-pixel horizontal motion — video memory is byte-addressed, so a sprite drawn from one bitmap could only move eight pixels at a time. |
| **STARS** | 48 stars flying outward from the centre. Each has a fixed direction and a distance that grows every field; position is `centre + direction × z / 32`, so the apparent speed grows with the distance, which is what sells the forward motion. |
| **SPIRO** | Lissajous figures, one point at a time. Two phase accumulators step by co-prime amounts, so the only multiply left is amplitude × sine. The figure changes every 45 fields. |

## How they draw

Not through CBIOS. Its set-point call takes four bytes through BDOS and the
screen driver per dot — fine for the probes that measured the layout
(`TODO.md` T47), about four hundred times too slow to animate anything.

Instead they map the video card's own DRAM over `$8000`–`$BFFF` with port
`$05` and write to it directly, which is certainly what any real P2500
graphics software did:

```
di
ld a,$08        ; video DRAM in, EPROM still out of low memory
out ($05),a
...             ; draw
ld a,$0F        ; main DRAM back
out ($05),a
ei
```

Interrupts are off inside that window: CBIOS's own ISRs live above `$CC00`
and would not notice, but its console output would write characters into the
middle of the picture.

The layout is the awkward part, and `demos/p2500.inc` exists to hide it. The
screen is not a linear framebuffer — the CRTC's raster address is wired to
address bits 12–13, so it selects one of four 4 KB banks:

```
addr = $8000 + (y & 3) * 4096 + (y >> 2) * 64 + x / 8     bit = 7 - (x & 7)
```

which is why stepping down one pixel row is `+$1000` three times and then
`-12224`. See `TODO.md` T47 for how that was established.

There is no vertical-blank line software can poll, so `wait_frame` counts
T-states instead — 18 × 256 `djnz` ≈ 60,000 of the 80,000 in a field, leaving
room for the drawing. Without it a demo redraws continuously and is never
seen anywhere but mid-blit.

## Building

`tools/z80asm.py` assembles them and checks its own work: `--verify`
disassembles the output with `p2500-emu --disasm` and compares it,
instruction by instruction, against the source. The decoder it checks against
was itself cross-checked against `z80dasm` over ~34,000 instructions, and the
two were written from opposite directions — so an encoder bug has to survive
a decoder that has no reason to share it. It caught two real ones while these
demos were being written: `ld a,(label)` assembling as an immediate, and a
constant silently overwritten by a same-named routine label.

`tools/mk_sprite.py` turns a PNG into the eight pre-shifted copies LOGO uses.
