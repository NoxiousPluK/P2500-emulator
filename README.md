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

## Building

```
make        # libp2500.a, p2500-emu, and p2500-gui if SDL3 is present
make test   # the regression suite
```

`make` needs a C11 compiler; the GUI additionally needs SDL3 (`extra/sdl3`
on Arch) and a C++17 compiler, and Dear ImGui is vendored. The core stays
dependency-free so `make test` runs with no display at all: **31 checks**,
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

**File** menu: Load Disk A/B/C, Reset, Pause, Screenshot, Quit. **Machine**
menu: capitals lock. Both are drawn by Dear ImGui and styled on the
emulator's own phosphor palette rather than being a native menu bar — see
`TODO.md` for why a native one is not viable on Wayland. "Load Disk" uses
`SDL_ShowOpenFileDialog`, so on Linux it is the desktop's own portal dialog.

The machine boots with **capitals lock engaged**, which is how these disks
are configured — unshifted keys produce capitals. The indicator at the right
of the menu bar is the P2219 manual's own keycap symbol, drawn rather than
typed because no Unicode character matches it.

`--scale N` sets the initial zoom; the window is resizable and letterboxes
with integer scaling. `--frames N --screenshot f.ppm`, `--shot-window f.ppm`
and `--push-at MS:STRING` let the front-end be driven and captured with no
display, which is how `make test` checks it.

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
| `--verbose-io` | log every I/O port access (very noisy) |
| `--no-stuck-detect` | disable the state-hash cycle detector |

`P2500_TRACE_FROM` / `P2500_TRACE_TO` give a bounded per-step trace.

Disks can be changed mid-run, and CP/M handles it as on real hardware —
swap, then Ctrl-C at the prompt to force a warm boot and re-read the
directory. A disk with no system tracks cannot be warm-booted from, which is
also authentic.

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
- `ctc` / `pio` / `dma` / `fdc` / `keyboard` / `sesam` — the devices
- `vendor/superzazu_z80/` — the vendored CPU core

**`src/cli/` → `p2500-emu`**, **`src/gui/` → `p2500-gui`**

**Tools**

- `run_tests.sh` — the suite behind `make test`
- `render_vram.py` — video-RAM-dump-to-PNG, 8×12 cells and attributes;
  `--demo-attrs` synthesises an attribute plane
- `disasm_ram.sh` — disassembles a `--dump-ram` image at real addresses
- `cpm_extract.py` — extracts files from a P2500 CP/M image. Necessary
  rather than convenient: the sector skew means a naive extractor reads half
  the disk from the wrong place. Derives the format from the image and then
  verifies what it produced. `--double-step` salvages a half dump
- `imd_tool.py` — ImageDisk verifier and normalizer; reports whether an
  image is a complete dump
- `ocr_manual.py` — OCRs a scanned manual, trying three orientations per
  page so sideways tables are found
