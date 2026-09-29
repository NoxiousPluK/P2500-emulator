# P2500 emulator

An emulator for the Philips P2000B / P2500 business computer, built from a
Z80 CPU card and video card ROM dump and a reverse-engineered hardware model
combined with a CP/M manual.

Manages to run CP/M seemingly without big issues.
Not yet tested with UCSD p-System (for lack of media) and properly SESAM-
protected files.

```
Philips P2500
58K CP/M Ver. 2.2

A>DIR
A: PIP      COM : SYSGEN   COM : SYSCBI   PHI : SYSLOAD  PHI
A: SYSPBI   PHI : SYSCPM   PHI

A>
```

Named "P2500-emulator" for brevity and to stay distinct from the unrelated
P2000T/P2000M/P2000C machines — the P2000B and P2500 are the same hardware
in slightly different case designs, so one name covers both.

## Features

- Full Z80 CPU emulation with a real IM2 interrupt daisy chain (CTC, PIO,
  DMA, FDC)
- CP/M 2.2 boot from floppy disk images, including mid-run disk swapping
- MC6845 CRTC-driven text video (80×24, 8×12 cells) and 512×256
  high-resolution graphics mode
- A live keyboard, with the P2500's own key mapping
- An SDL3 + Dear ImGui GUI with adjustable emulation speed (0.25×–8× or
  unlimited) and a four-panel debugger: device state, memory, disassembly,
  and a filterable diagnostic log
- A headless CLI front-end (`p2500-emu`) for scripted runs, disk-image
  probing, and automated testing — no display required
- Tools to build and inspect P2500 CP/M disk images from the host

## Building

```sh
./build.sh   # or: make
```

Needs a C11 compiler. The GUI additionally needs SDL3 (`extra/sdl3` on
Arch) and a C++17 compiler; Dear ImGui is vendored. The core has no
dependencies beyond libc, so `p2500-emu` and `make test` run with no
display at all — the GUI build and its checks are skipped if SDL3 is
not found by `pkg-config`.

```sh
./clean.sh   # or: make clean
```

Removes build artefacts (objects, the library, both binaries, the built
demo disk) and Python's `__pycache__`.

```sh
./publish.sh
```

Publishes ready to use binaries for Windows and Linux in `publish/`.

## Running it

```sh
./p2500-gui --disk disks/P25K_B.raw --disk-b disks/P2500GAM.raw
```

Or on Windows:

```cmd
p2500-gui.cmd --disk disks/P25K_B.raw --disk-b disks/P2500GAM.raw
```

`disks/` holds known working disk images — a CP/M system disk, SuperCalc2,
a development disk with MACRO-80 and MBASIC, and a disk of BASIC games —
see `disks/README.md`. Nothing else is needed to run the emulator.

The guest runs at the speed of the real machine by default: paced to 50
fields a second against the wall clock, independent of the display's
refresh rate. Speed is adjustable from 0.25× to 8×, or unlimited, via
*Machine ▸ Speed* or the status-bar speed cell (click to toggle unlimited,
right-click for the list).

### Keyboard

| Key | Sends |
|---|---|
| any printable key | ASCII on port `$06` |
| Return / Backspace / Tab / Esc / Delete | `$0D` / `$08` / `$09` / `$1B` / `$7F` |
| arrow keys | decoded through CBIOS's own key table |
| Ctrl+letter | `^A`–`^Z` (Ctrl-C warm-boots CP/M) |
| Ctrl+O / Ctrl+R / Ctrl+Q | load disk / reset / quit |
| F1 / F2 / F3 / F4 | device state / memory / disassembly / log panel |
| F10 / F11 / F12 | screenshot (BMP) / unlimited speed (toggle) / pause (toggle) |

The machine boots with **capitals lock engaged**, matching how these disks
are configured — unshifted keys produce capitals.

### Menu bar

**File**: Load Disk A/B/C, Reset, Pause, Screenshot, Quit.
**Machine**: capitals lock, speed.
**Debug**: the four debugger panels.

At the right of the bar, a row of status lamps doubles as controls:

| Lamp | Shows | Click |
|---|---|---|
| `A B C` | a disk is attached in that drive | load a disk; right-click ejects |
| ▶ / ⏸ | running / paused | toggle pause |
| ⊓⊔ | capitals lock | toggle |
| `1x` | current speed (lit when not 1×) | toggle unlimited; right-click for the speed list |

### GUI command-line options

| Flag | |
|---|---|
| `--disk PATH` / `--disk-b` / `--disk-c` | attach images in drives A:, B:, C: |
| `--rom PATH` / `--charrom PATH` | boot EPROM, character generator |
| `--scale N` | initial window zoom (the window is resizable and letterboxes with integer scaling) |
| `--speed X` \| `unlimited` | initial emulation speed |
| `--no-caps-lock` | boot with capitals lock off |
| `--panels LIST` | open specific debugger panels at startup |
| `--frames N --screenshot f.ppm` | run headless for N frames and capture the screen |
| `--shot-window f.ppm` | capture the whole window, including the menu bar and any open panels |
| `--push-at MS:STRING` | scripted keystrokes via the live input ring, at an emulated-time offset |
| `--mouse [FRAME:]X,Y[,left\|right]` | drive a synthetic pointer click (repeatable) |
| `--ui-type FRAME:TEXT` | type into whichever panel widget has focus |
| `--break ADDR` / `--watch ADDR` / `--watch-break ADDR` | stop / trace / stop-on-change |
| `--verbose-io` | arm every device's diagnostic log output |

The last five make the GUI fully scriptable with no display, which is how
`make test` exercises it.

## The debugger

Everything the panels show comes from `core/debug.c`, the same code
`p2500-emu` prints from — a panel cannot show something the headless
harness can't also report.

- **Device state** (F1) — every device at once: the interrupt daisy chain,
  the four CTC channels, PIO mode and masks, DMA registers, FDC phase and
  command/result bytes, CRTC registers, the video attribute latch.
  "Copy all" puts the same text `--state` prints on the clipboard.
- **Memory** (F2) — bank-aware, with separate views of the video character
  and attribute planes, and watches. A watch reports to the log when its
  byte changes and names the instruction that wrote it; ticking `stop`
  turns it into a watchpoint that pauses the machine there.
- **Disassembly** (F3) — around PC, with a clickable breakpoint gutter and
  Step / Step 100 / Step field.
- **Log** (F4) — every diagnostic the core produces, filterable by level
  and by substring. *Debug ▸ Verbose device logging* arms the noisiest
  per-device traces (off by default).

## The headless harness

`p2500-emu` is what `make test` drives, and is useful on its own for
scripted runs, probing, and disk-image inspection.

| Flag | |
|---|---|
| `--rom PATH` / `--charrom PATH` | boot EPROM, character generator |
| `--disk PATH` / `--disk-b` / `--disk-c` | attach images in drives A:, B:, C: |
| `--swap-at MS:PATH` | change disks at an emulated-time offset; `MS:B:PATH` targets a drive |
| `--sesam PATH` | attach a byte stream to the SESAM dongle port |
| `--type STRING` / `--type-after MS` / `--type-at MS:STRING` | scripted keystrokes via the queue |
| `--push-at MS:STRING` | keystrokes via the live ring — the same path a GUI keypress takes |
| `--max-steps N` | instruction budget (default 2,000,000) |
| `--speed X` \| `unlimited` | emulation speed; **unlimited is the default here**, unlike the GUI |
| `--dump-vram` / `--dump-vram-attr` / `--dump-ram` PATH | dump the video plane, attribute plane, or all 64 KB |
| `--dump-screen f.ppm` | render through the core's own renderer — the same call the GUI makes |
| `--peek ADDR:LEN` / `--poke ADDR:HEX` | inspect / patch memory |
| `--watch ADDR[:LEN]` / `--count ADDR` / `--break ADDR` | trace writes, count executions, stop |
| `--watch-break ADDR[:LEN]` | a watchpoint — stop the run when that byte changes |
| `--state` | print every device's live state |
| `--disasm ADDR[:COUNT]` | disassemble, bank-aware |
| `--verbose-io` | arm every device's diagnostics (very noisy) |
| `--no-stuck-detect` | disable the state-hash cycle detector |

`P2500_TRACE_FROM` / `P2500_TRACE_TO` (environment variables) give a
bounded per-step instruction trace.

Every run ends with a `Host speed:` line — the emulated-to-wall-clock
ratio and the instruction rate the host managed.

Disks can be changed mid-run, and CP/M handles it as on real hardware: a
swapped-in disk is readable immediately (`DIR` lists it with no warm
boot), but write access to it is marked unavailable until a warm boot
(Ctrl-C) clears the flag — writing to disk is not yet implemented, so
that flag currently has no other visible effect.

## Building disk images

`tools/cpm_build.py` writes a P2500 CP/M image from host files:

```sh
# A bootable disk with one program on it.
tools/cpm_build.py yourprog.raw PROGRAM.COM --boot-from disks/P25K_B.raw

# A data disk for drive B:, no boot files.
tools/cpm_build.py games.raw *.BAS
```

`--boot-from` copies the loader (the two reserved tracks) and the CP/M
system files (`SYSCPM.PHI`, `SYSCBI.PHI`, `SYSPBI.PHI`, `SYSLOAD.PHI`)
from a donor image — a disk needs both to boot on its own. Every build is
read back through `cpm_extract.py` and compared byte for byte.

Other tools in `tools/`:

| Tool | |
|---|---|
| `cpm_extract.py` | extracts files from a P2500 CP/M image, deriving the disk format from the image itself |
| `z80asm.py` | a small Z80 assembler; `--verify` disassembles its own output and compares it against the source |
| `mk_sprite.py` | converts a PNG to a pre-shifted 1-bit sprite for smooth motion in a byte-addressed framebuffer |

See `docs/porting-cpm-software.md` for what porting an existing CP/M
program to the P2500 involves — the binary runs unmodified; the screen
control codes are what differ. Its probing example uses
`tests/mk_cpm_probe.py`, which builds a minimal `.COM` that prints an
exact byte sequence, for finding out what a given screen control code
actually does.

## Testing

```sh
make test
```

runs `tests/run_tests.sh`: 77 checks covering the SESAM cartridge boot
path, the IPL and CP/M boot sequence, disk I/O (including mid-run swaps),
the video renderer (checked as pixels, not just state), the debugger
panels, the disassembler (`tests/disasm_crosscheck.py`, cross-checked
against `z80dasm` over ~34,000 instructions), and the speed control. Exit
code 1 on any failure.

`tests/render_vram.py` renders a `--dump-vram` capture to a PNG (`--attr`
for the attribute plane, `--graphics` for the 512×256 mode) — useful for
looking at a capture by hand while chasing a test failure.

## Project layout

```
src/core/   machine model (libp2500.a) — C11, no dependencies beyond libc
src/cli/    headless harness (p2500-emu)
src/gui/    SDL3 + Dear ImGui front-end (p2500-gui) — the only C++ in the tree
tools/      disk-image and asset build tools (Python)
tests/      the regression suite and its fixtures
demos/      three graphics demos + a benchmark, in Z80 assembly
disks/      CP/M disk images that boot on the emulator
roms/       the boot EPROM and character ROM dumps
docs/       user-facing documentation (porting CP/M software, etc.)
```

`src/core/` holds the machine model — CPU wiring, the interrupt daisy
chain, video, and each device (`ctc`, `pio`, `dma`, `fdc`, `keyboard`,
`sesam`) — and nothing in it may depend on either front-end. `src/cli/`
and `src/gui/` are both thin front-ends over the same core, so
`p2500-emu --state`/`--disasm` and the GUI's panels can never disagree
about what the machine is doing.

## Acknowledgements

- **Z80 core**: [`superzazu/z80`](https://github.com/superzazu/z80) (MIT),
  vendored in `src/core/vendor/superzazu_z80/`, with one local addition —
  an `on_reti` callback, needed for the interrupt daisy chain to see
  `RETI` and model IEO release.
- **UI**: [Dear ImGui](https://github.com/ocornut/imgui) 1.92.1 (MIT),
  vendored in `src/gui/vendor/imgui/` with the `sdl3` + `sdlrenderer3`
  backends.
- **Rendering and input handling**: [SDL3](https://libsdl.org/) (zlib).
- **FDC design** informed by [`ifilot/p2000m-emulator`](https://github.com/ifilot/p2000m-emulator),
  which emulates the sibling P2000M machine and ships a primary-source
  reference for its floppy controller. Not reused verbatim — the P2500's
  ports, control bits and disk geometry all differ — but it independently
  confirmed this project's own ROM-derived command decode.
- **Disk images** by [Home Computer Museum](https://download.homecomputer.museum/#Files%2FPhilips%2FP2500).