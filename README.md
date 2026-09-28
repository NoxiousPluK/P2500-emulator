# P2500 emulator

*A from-scratch emulator for the Philips P2000B / P2500 CPU card.*

**CP/M 2.2 boots in a window you can type into, and runs real applications.**
SuperCalc2 — an OEM build whose splash reads `PHILIPS P2000` — loads from one
disk and opens files; MBASIC-80 runs from another; a 1983 adventure game runs
off drive B with working screen attributes.

```
Philips P2500
58K CP/M Ver. 2.2

A>DIR
A: PIP      COM : SYSGEN   COM : SYSCBI   PHI : SYSLOAD  PHI
A: SYSPBI   PHI : SYSCPM   PHI

A>
```

Named "P2500" for brevity and to stay distinct from the unrelated
P2000T/P2000M/P2000C machines. The P2000B and P2500 are the same hardware in
different case colours (`../P2500-general-findings.md`), so one name covers
both.

- **`TODO.md`** — the work queue, the decoded hardware model, and the address
  references. Read it first if you are here to do work.
- **`ROADMAP.md`** — the shape of the project and the reasoning about
  sequencing.
- **`docs/porting-cpm-software.md`** — what it takes to move a CP/M program
  from another machine to this one. The binary runs unmodified; the screen
  control codes are what differ.

## Building

```
make        # libp2500.a, p2500-emu, and p2500-gui if SDL3 is present
make test   # the regression suite
```

`make` needs a C11 compiler; the GUI additionally needs SDL3 (`extra/sdl3`
on Arch) and a C++17 compiler, and Dear ImGui is vendored. The core stays
dependency-free so `make test` runs with no display at all: **55 checks**,
exit 1 on any failure. The GUI checks skip themselves if SDL3 is absent.

## Running it as a machine

```
./p2500-gui --disk "../Disk Images/extracted/P25K_B/P25K_B.raw" \
            --disk-b "../Disk Images/extracted/P2500GAM/P2500GAM.raw"
```

Boots to `A>` in a window. Geometry, cursor position and cursor shape all
come from the MC6845's registers, so the display follows whatever the guest
programs.

| Key | |
|---|---|
| any printable key | sent as ASCII on port `$06` |
| Return / Backspace / Tab / Esc / Delete | `$0D` / `$08` / `$09` / `$1B` / `$7F` |
| arrow keys | the WordStar diamond CBIOS's own table at `$E274` decodes |
| Ctrl+letter | `^A`–`^Z`, so Ctrl-C warm-boots CP/M |
| Ctrl+O / Ctrl+R / Ctrl+Q | load disk / reset / quit |
| F10 / F11 / F12 | screenshot (BMP) / turbo / pause |
| F1 / F2 / F3 / F4 | device state / memory / disassembly / log |

**File** menu: Load Disk A/B/C, Reset, Pause, Screenshot, Quit. **Machine**
menu: capitals lock. **Debug** menu: the four panels below. All drawn by Dear
ImGui and styled on the emulator's own phosphor palette rather than being a
native menu bar — see `TODO.md` for why a native one is not viable on
Wayland. "Load Disk" uses `SDL_ShowOpenFileDialog`, so on Linux it is the
desktop's own portal dialog.

At the right of the bar, lamps in fixed positions — each always drawn,
bright when it applies and faint when it does not, so nothing moves and an
unlit lamp is still readable. **Each one is also a button**, and hovering
inverts it (bright ground, dark glyph) to say so; the ground keeps the
lamp's own brightness, so the state stays readable under the pointer.

| | shows | click |
|---|---|---|
| `A B C` | a disk is attached in that drive | load a disk; right-click ejects |
| ▶ / ⏸ | running (dim) or paused (bright — it is the state you can forget you are in) | toggle |
| ⊓⊔ | capitals lock, the P2219 manual's own keycap symbol, drawn rather than typed because no Unicode character matches it | toggle |

The machine boots with **capitals lock engaged**, which is how these disks
are configured — unshifted keys produce capitals.

`--scale N` sets the initial zoom; the window is resizable and letterboxes
with integer scaling. `--frames N --screenshot f.ppm`, `--shot-window f.ppm`,
`--push-at MS:STRING`, `--panels LIST`, `--verbose-io`, `--break ADDR` and
`--watch ADDR` let the front-end and its panels be driven and captured with no
display, which is how `make test` checks them. `--mouse X,Y[,left|right]`
parks and clicks a synthetic pointer, so the menu-bar lamps are covered too
— the front-end reports its own lamp geometry to the log rather than the
suite hardcoding pixel columns that a new menu would quietly invalidate.

## The debugger

The reason the GUI exists. Everything the panels show comes out of
`core/debug.c`, which `p2500-emu` prints from too, so a panel cannot drift
away from what the harness reports.

- **Device state** (F1) — every device at once: the IM2 daisy chain with each
  source's vector, request/acknowledge counts and in-service flag; the four
  CTC channels with live down-counters and CLK/TRG levels; PIO mode and
  masks; DMA direction, length and address; FDC phase, unit and command and
  result bytes; the CRTC registers and the geometry they imply; the attribute
  latch. "Copy all" puts the same text `--state` prints on the clipboard.
- **Memory** (F2) — bank-aware through `p2500_peek`, with the video
  character and attribute planes as separate views, and watches that report
  to the log when they change.
- **Disassembly** (F3) — around PC, with a clickable breakpoint gutter and
  Step / Step 100 / Step field. Forwards from an anchor only: a Z80 stream
  cannot be decoded backwards, and guessing would show confident nonsense.
- **Log** (F4) — every diagnostic the core produces: port access, the IM2
  daisy chain, CTC/PIO/DMA/FDC decode, plus watch hits and breakpoint stops.
  Filterable by level and by substring; warnings are amber, deliberately off
  the phosphor palette, because those are the lines saying the emulator
  declined to act. **Debug > Verbose device logging** arms the device
  switches — off by default, since they emit a few thousand lines per
  emulated second.

## The headless harness

`p2500-emu` is what `make test` drives and what every investigation in this
project has used.

| Flag | |
|---|---|
| `--rom PATH` / `--charrom PATH` | boot EPROM, character generator |
| `--disk PATH` / `--disk-b` / `--disk-c` | attach images in drives A:, B:, C: |
| `--swap-at MS:PATH` | change disks at an emulated-time offset; `MS:B:PATH` targets a drive |
| `--sesam PATH` | attach a byte stream to the SESAM dongle port |
| `--type STRING` / `--type-after MS` / `--type-at MS:STRING` | scripted keystrokes via the queue |
| `--push-at MS:STRING` | keystrokes via the **live** ring — the path a GUI keypress takes |
| `--max-steps N` | instruction budget, default 2,000,000 |
| `--dump-vram` / `--dump-vram-attr` / `--dump-ram` | video plane, attribute plane, all 64 KB |
| `--dump-screen f.ppm` | render through the core's own renderer — the same call the GUI makes |
| `--peek ADDR:LEN` / `--poke ADDR:HEX` | inspect / patch memory |
| `--watch ADDR[:LEN]` / `--count ADDR` / `--break ADDR` | trace writes, count executions, stop |
| `--state` | every device's live state — the same lines the GUI's device panel draws |
| `--disasm ADDR[:COUNT]` | disassemble, bank-aware, through the same decoder the GUI uses |
| `--verbose-io` | arm every device's diagnostics — ports, the daisy chain, CTC/PIO/DMA/FDC (very noisy) |
| `--no-stuck-detect` | disable the state-hash cycle detector |

`P2500_TRACE_FROM` / `P2500_TRACE_TO` give a bounded per-step trace.

Disks can be changed mid-run, and CP/M handles it as on real hardware. A
swapped-in disk is **readable immediately** — `DIR` straight after a swap
lists the new disk, because CP/M re-reads the directory on every search.
What the swap costs is write access: these drives have a non-zero checksum
count in their DPB (`CKS` = 16, i.e. removable media), so BDOS notices the
directory checksum has changed and sets the drive's bit in its read-only
vector at `$E1AD`. Ctrl-C clears it, by way of the BDOS reset at `$E086`.
Until writing is implemented (`TODO.md` T30) that flag has no visible
consequence, which is why Ctrl-C is not needed in practice today. A disk
with no system tracks cannot be warm-booted from at all — Ctrl-C echoes and
the machine sits there, which is also authentic.

## Building disk images

`tools/cpm_build.py` writes a P2500 CP/M image from host files — the exact
inverse of the extractor, and written against its constants so the reader
and the writer cannot drift apart. Every build is read back through
`cpm_extract.py` and compared byte for byte before it is called done.

```
# A bootable disk with one program on it.
tools/cpm_build.py othello.raw OTHELLO.COM \
    --boot-from "../Disk Images/extracted/P25K_B/P25K_B.raw"

# A data disk for drive B:.
tools/cpm_build.py games.raw *.BAS
```

`--boot-from` exists because a bootable P2500 disk needs **two** things: the
loader in the two reserved tracks, *and* the CP/M system as ordinary
directory files (`SYSCPM.PHI`, `SYSCBI.PHI`, `SYSPBI.PHI`, `SYSLOAD.PHI`).
A disk with the loader alone gets nowhere — `make test` asserts that in both
directions.

`tools/mk_cpm_probe.py` builds a nine-byte CP/M program that prints one
string, which is how the screen control codes get settled: put the sequence
on a generated boot disk, run it, and read `--dump-vram-attr`.

## Validation

Three independent guards, all run by `make test`.

**The SESAM banner is byte-exact.** `roms/sesam_banner_test.bin` is the same
synthetic bootable-cartridge stream the Python-era work used; its
`--dump-vram` output is byte-for-byte identical to the Python-era
`vram_after_banner.bin`. The oldest test here. Keep it green.

**CP/M is checked by observable state**, not by "it didn't crash": page zero
must hold CP/M's real vectors, CBIOS's `$E200` table must be `JP`
instructions, the screen must read `58K CP/M Ver. 2.2` with an `A>` prompt,
typing `DIR` must list exactly the six files the image contains, and `DIR B:`
must list drive B's disk and not drive A's. The exit report also asserts that
every interrupt request is acknowledged exactly once — the failure mode that
cost this project the most time was a *silent* interrupt drop.

**The rendered frame is checked as pixels**: 640×288, ink below the `p` of
"Philips" (proving the 8×12 cell rather than 8×8), a solid cursor block
where R14/R15 point, and the menu bar drawn with the screen offset below it.
The UI's own state is checked the same way — the panels must add ink, the
run/pause lamp must change when a breakpoint fires, three mounted disks must
light more of the bar than one, hovering a lamp must fill most of its cell
while not activating it, and clicking must pause, eject and toggle. Each of
those was confirmed to fail when the behaviour it guards is removed.

**The disassembler is cross-checked against `z80dasm`** over the IPL ROM,
every opcode page and random byte streams — about 34,000 instructions per
`make test` run, zero mismatches. Both walk the same bytes, so a single
length disagreement desynchronises them and shows up immediately; that makes
it a test of decoding rather than of spelling. The two differ by design on
one point, counted and reported separately: z80dasm renders bytes it will not
decode as a lone `defb`, while this decoder reports what the CPU really does
— a DD/FD prefix on an opcode with no index form, or an undefined `ED`, is a
two-byte instruction, and the PC has to follow it.

## Why this exists, and the projects that informed it

Built to continue the dynamic-emulation work in the parent research
project's `ROM Dumps/CPU-Card-Boot-EPROM/emulation/` once that hit a hard
limit: the Python `z80` package used there has no clean way to inject real
IM2 interrupts, and the ROM's own floppy driver depends on them.

- **Z80 core**: [`superzazu/z80`](https://github.com/superzazu/z80) (MIT),
  vendored in `src/core/vendor/superzazu_z80/`. Chosen because it exposes a
  single clean call, `z80_gen_int(z, vector_byte)`, to fire a real maskable
  interrupt at the exact moment we want — no global state, no polling
  callback. One local addition, marked as such: an `on_reti` callback,
  without which the daisy chain cannot see `RETI` and so cannot model IEO
  release.
- **UI**: [Dear ImGui](https://github.com/ocornut/imgui) 1.92.1 (MIT),
  vendored in `src/gui/vendor/imgui/` with the `sdl3` + `sdlrenderer3`
  backends.
- **FDC design informed by [`ifilot/p2000m-emulator`](https://github.com/ifilot/p2000m-emulator)**:
  that project emulates the P2000M, a sibling machine from the same Philips
  lineage, and ships a primary-source reference transcribed from a Philips
  Field Support Manual. **Not reused verbatim** — the P2500's ports, control
  bits and disk geometry all differ, and this project's own reverse-
  engineered ROM command tables were the source of truth — but that document
  independently confirmed the command decode and the phase-state-machine
  design. Their bundled `p2000.rom` is completely different firmware from
  this project's IPL dump (35/4096 bytes coincidentally match).

## Files

The tree is split three ways. `src/core/` is the machine model and depends on
nothing but libc; `src/cli/` is the headless harness; `src/gui/` is the SDL3
front-end and the only C++ in the tree. **Nothing in `core/` may depend on
either front-end.**

**`src/core/` → `libp2500.a`**

- `machine.{c,h}` — memory map, port dispatch, Z80 wiring, and the
  per-instruction step that advances every device and arbitrates interrupts
- `intctl.{c,h}` — the IM2 daisy chain: hold-until-acknowledged, priority by
  chain position, release on `RETI`
- `video.{c,h}` — the text renderer, driven by the MC6845 registers. 8×12
  cells from the character ROM's 16-byte stride, cursor from R14/R15 with
  shape from R10/R11, and the 4-bit attribute plane the CPU fills by
  latching a nibble in port `$0A`
- `debug.{c,h}` — a Z80 disassembler, the watch/counter/breakpoint tables
  both front-ends drive, and the live-state lines both of them print
- `log.{c,h}` — the one diagnostics sink every device writes through. The
  core does not know what stderr is; the CLI prints, the GUI fills a panel,
  and an embedding that wants neither installs nothing
- `ctc` / `pio` / `dma` / `fdc` / `keyboard` / `sesam` — the devices
- `vendor/superzazu_z80/` — the vendored CPU core

**`src/cli/` → `p2500-emu`**, **`src/gui/` → `p2500-gui`** (`main.cpp` is the
window, pacing, input and menu bar; `panels.cpp` is the debugger)

**Tools**

- `run_tests.sh` — the suite behind `make test`
- `render_vram.py` — video-RAM-dump-to-PNG, 8×12 cells and attributes;
  `--demo-attrs` synthesises an attribute plane
- `disasm_ram.sh` — disassembles a `--dump-ram` image at real addresses
- `cpm_build.py` — builds a P2500 CP/M image from host files, bootable with
  `--boot-from`. Verifies every build by reading it back through the
  extractor
- `mk_cpm_probe.py` — builds a minimal CP/M `.COM` that prints one string,
  for probing screen behaviour no surviving program exercises
- `cpm_extract.py` — extracts files from a P2500 CP/M image. Necessary
  rather than convenient: the sector skew means a naive extractor reads half
  the disk from the wrong place. Derives the format from the image and then
  verifies what it produced. `--double-step` salvages a half dump
- `imd_tool.py` — ImageDisk verifier and normalizer; reports whether an
  image is a complete dump
- `ocr_manual.py` — OCRs a scanned manual, trying three orientations per
  page so sideways tables are found
- `disasm_crosscheck.py` — checks `core/debug.c`'s disassembler against
  `z80dasm` over generated and real byte streams
