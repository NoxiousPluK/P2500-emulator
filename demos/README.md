# Graphics demos

Three small programs for the P2500's 512 × 256 high-resolution mode, written
to exercise it with something harder than a test pattern.

Also a small benchmark application.

```
make demos                       # assembles, verifies, and builds the disk
./p2500-gui --disk demos/P2500DEMO.raw
A>LOGO          A>STARS          A>SPIRO          A>BENCH
```

Any key returns to CP/M. `make demos` needs a bootable P2500 image to take
the CP/M system files from; it defaults to `P25K_B` and takes
`BOOT_DONOR=path` otherwise.

| | |
|---|---|
| **LOGO** | The P2000 wordmark, bouncing — 144 × 45 pixels, drawn from `p2000logo.bmp` at 1:1 so nothing is resampled. Eight pre-shifted copies of the sprite give single-pixel horizontal motion: video memory is byte-addressed, so a sprite drawn from one bitmap could only move eight pixels at a time. Direction and starting position come from the tick counter, so the angle differs every run. |
| **STARS** | 48 stars flying outward from the centre. Each has a fixed direction and a distance that grows every field; position is `centre + direction × z / 32`, so the apparent speed grows with the distance, which is what sells the forward motion. The distance is 8.8 fixed point so the step can be a fraction of a unit — the only way to slow it without dropping to half the frame rate. |
| **SPIRO** | Lissajous figures, one point at a time. Two phase accumulators step by co-prime amounts, so the only multiply left is amplitude × sine. 60 points a field, and the figure changes every 110. |
| **BENCH** | Not a demo — eleven timings, for comparing this emulator against real hardware. See below. |

## Drawing a mover without flicker

LOGO writes each byte exactly once per frame rather than erasing the old
position and then drawing the new one — an erase pass would leave the logo
off-screen for the length of it. Instead the sprite carries its own
background: `mk_sprite --halo` surrounds it with one byte of blank margin
left and right and one blank row top and bottom, so a plain store covers
wherever the sprite just was and there is no erase pass.

The catch is that the step has to stay inside that margin, and that is what
bounds the movement: eight pixels horizontally, and `--halo-rows` vertically
(two here, which is why `|dy|` is 1 or 2). Go further and the sprite leaves a
trail, because nothing else clears the screen.

## BENCH

```
A>BENCH

P2500 BENCH - ticks at 50 Hz, so x20 = ms
T = text mode, G = graphics mode

CPU regs      92        registers only: instruction fetch and execute
RAM write     81        24 x 16 KB of byte stores
RAM read      80        24 x 16 KB of byte loads
RAM ldir      52        12 x 16 KB block copy
VID write T   81        the same stores into the video window, text mode
VID ldir  T   52
VID write G   81        and again after ESC 3, with the CRTC reprogrammed
VID ldir  G   52
Firmware  .   33        192 dots through CBIOS's set-point call
Console   .   28        512 characters through BDOS 2
Disk read .    9        64 records read sequentially from SYSCPM.PHI
```

Everything is timed against CBIOS's 50 Hz tick counter, so the same binary
gives comparable numbers on the emulator and on a real P2500. Results are
collected first and printed afterwards, because console output costs about a
millisecond a character here and would otherwise be inside the timings.

**The rows worth staring at are the VID ones.** This emulator charges a write
to the video window exactly what it charges main RAM — it models no CRTC
contention at all — so `VID write` equals `RAM write` above by construction,
and `make test` asserts that so nobody changes it by accident. A real video
card shares that DRAM with the CRTC's display fetches and may well stall the
CPU during active display. If the real machine's VID rows come out higher
than its RAM rows, that ratio *is* the missing wait-state model. Text and
graphics modes are timed separately because the CRTC fetches on a different
rhythm in each (64 columns of 4 scanlines against 80 of 12).

`Disk read` is the other likely mismatch: the emulator hands over a whole
sector in one `memcpy`, with no seek and no rotational latency.

Interrupts stay enabled throughout — they have to, since the clock being
measured is one — so every figure includes the 50 Hz ISR. That is the same on
both sides and is a fraction of a percent.

For scale, from the numbers above: a full 512 × 256 screen costs about
**0.17 s** written directly and about **437 s** a dot at a time through the
firmware. Nearly all of that is BDOS and console overhead rather than
plotting — a plain character costs *more* per byte than a graphics command
byte does. It is why the demos bypass CBIOS, and why anything drawing through
the documented set-point call will feel glacial.

## How the demos draw

Not through CBIOS. Its set-point call takes four bytes through BDOS and the
screen driver per dot — fine for probing, about four hundred times too slow
to animate anything.

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
`-12224`.

## Keeping time

There is no vertical-blank line software can poll — the MC6845's status
register is not wired anywhere the CPU can see it. But **CBIOS keeps a 16-bit
counter at `$F436`** that its CTC channel-2 interrupt advances once per field,
and measured against the emulator's own clock it runs at 50.00 Hz exactly. So
`wait_field` watches its low byte change, which is a true field sync.

Interrupts have to be on for it, so `wait_field` is called after `vid_out`.
The address belongs to this CP/M build; a disk built from a different donor
may put CBIOS elsewhere, so the routine gives up after about three fields
rather than spinning for ever.

## Building

`tools/z80asm.py` assembles them and checks its own work: `--verify`
disassembles the output with `p2500-emu --disasm` and compares it,
instruction by instruction, against the source. The decoder it checks against
was itself cross-checked against `z80dasm` over ~34,000 instructions, and the
two were written from opposite directions — so an encoder bug has to survive
a decoder that has no reason to share it. It caught two real ones while these
demos were being written: `ld a,(label)` assembling as an immediate, and a
constant silently overwritten by a same-named routine label.

`tools/mk_sprite.py` turns an image into the eight pre-shifted copies LOGO
uses. `--invert` picks which of light and dark is ink — for this wordmark the
inverted reading is much the clearer, because the stripes then break up the
background rather than the letters. `--halo` adds the margin described above,
with `--halo-rows` setting how many blank rows above and below — i.e. the
largest vertical step the sprite can cover on its own.
