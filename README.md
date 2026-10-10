# P2500 emulator

An emulator for the Philips P2000B / P2500 business computer, built from a
Z80 CPU card and video card ROM dump and a reverse-engineered hardware model
combined with a CP/M manual.

Manages to run CP/M seemingly without big issues.
Not yet tested with UCSD p-System (for lack of media) and properly SESAM-
protected files.

![Screenshot of the P2500 emulator booted into CP/M and showing a directory listing](docs/screenshot.png?raw=true)

Named "P2500-emulator" for brevity and to stay distinct from the unrelated
P2000T/P2000M/P2000C machines — the P2000B and P2500 are the same hardware
in slightly different case designs, so one name covers both.

**Note:** This emulator is far from complete, and likely contains a sizable
amount of wrong assumptions, missing features or other errors. This is very
much a work in progress, and real hardware needs to be used to verify against.

During research and implementation, LLMs were used to speed up the process
and make informed decisions. I could not have done it without the utility
that LLMs provide, and it doesn't look like anyone else had plans to build
this software or do this research.

## Features

- Full Z80 CPU emulation with a real IM2 interrupt daisy chain (CTC, PIO, DMA, FDC)
- CP/M 2.2 boot from floppy disk images, including disk swapping
- MC6845 CRTC-driven text video (80×24, 8×12 cells) and 512×256 high-resolution graphics mode
- Cartridge support including SESAM-key emulation
- Keyboard input
- Adjustable emulation speed
- An SDL3 + Dear ImGui GUI with debugger: device state, memory, disassembly, watches with auto-pause, and rewind
- A headless CLI front-end (`p2500-emu`) for scripted runs, disk-image probing, and automated testing
- Some tools to build and inspect P2500 CP/M disk images

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
p2500-gui.exe --disk disks/P25K_B.raw --disk-b disks/P2500GAM.raw
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
**Machine**: capitals lock, Cartridge..., speed.
**Debug**: the four debugger panels.

At the right of the bar, a row of status lamps doubles as controls:

| Lamp | Shows | Click |
|---|---|---|
| `A B C` | a disk is attached in that drive | load a disk; right-click ejects |
| `Cartridge: ...` | the port $0F cartridge slot's contents (SESAM key ID / loaded ROM) | opens the Cartridge window; right-click toggles insert/eject |
| ▶ / ⏸ | running / paused | toggle pause |
| ⊓⊔ | capitals lock | toggle |
| `1x` | current speed (lit when not 1×) | toggle unlimited; right-click for the speed list |

The **Cartridge** window (Machine menu, or click its lamp) covers port
$0F; a generic plug-in slot whose best-documented use is the SESAM
copy-protection dongle, though the same slot can hold other cartridges (the
P2500's "Maint" diagnostic plug is a, first-party example). It lets you
type in a SESAM key serial to unblock a disk image that is already SESAM-initialized
against a real key, or load an arbitrary cartridge ROM file.

### GUI command-line options

| Flag | |
|---|---|
| `--disks A[,B[,C[,D]]]` | attach images to the four drives in order; an empty field skips one |
| `--disk PATH` / `--disk-b` / `--disk-c` / `--disk-d` | the same, one drive at a time |
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
| `--symbols FILE` \| `auto` | load names from a symbol file (repeatable), or match the running build — see [Symbol files](#symbol-files) |
| `--verbose-io` | arm every device's diagnostic log output |
| `--help` | print the flag list and exit |

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
  Step / Step over / Step 100 / Step field. Right-click any line for *Run
  to here*. With **symbols** ticked the listing reads `call CONIN` rather
  than `call $E209`: the names are read out of the running machine's own
  page zero and CBIOS jump table, so they belong to the disk that is
  actually booted and there is nothing to show before CP/M is up —
  `--symbols` adds names that a running machine cannot be asked for.
  While paused, **Step back** / **Back 100** / **Back to brk** run the
  machine *backwards* — see below.
- **Log** (F4) — every diagnostic the core produces, filterable by level
  and by substring. *Debug ▸ Verbose device logging* arms the noisiest
  per-device traces (off by default).

## Rewind

The debugger steps backwards. Nothing in the core calls `rand()`, `time()`
or any host service — the 50 Hz clock strobe and the keyboard strobe are
both counters driven off the CPU's T-state count — so a replay from a given
state is bit-identical, and the machine's entire mutable state is one
struct. Rewinding is therefore a snapshot plus a replay: a ring of 64
whole-machine snapshots, one per emulated 50 Hz field (~7 MB, ~1.3 s of
history), and "go to instruction N" restores the newest snapshot at or
before N and replays forward to it. The replay is silent — no breakpoints
fire, no diagnostics are emitted — and `make test` asserts the round trip
is byte-exact rather than assuming it.

In the GUI the buttons are live while paused; the screen rewinds with the
machine, because it is re-rendered from machine state every frame. The log
does not rewind: it is a transcript of what happened, and gets a marker
line instead.

Headlessly, every rewind acts once the run has stopped, so the exit report,
`--state` and `--disasm` all describe wherever it left the machine:

| Flag | |
|---|---|
| `--history` | record snapshots (implied by every flag below; opt-in, because it is not free) |
| `--step-back N` | go back N instructions |
| `--seek N` | go to instruction number N |
| `--back-to ADDR` | the last time the PC was ADDR, anywhere in the recorded history |
| `--back-to-break` | the last time an enabled breakpoint was standing at the PC |
| `--verify-rewind N` | go back N and forward again, and report whether the machine came back bit-identical |

There is no reverse step-*over*, and that is a finding rather than a gap:
the IPL's driver dispatch switches stacks on entry (`$02DA` is
`ld ($FE81),sp` / `ld sp,$FE81`), so inside a `call $0003` neither the SP
nor a CALL/RET count recovers the caller's depth. Both rules were built and
measured against it and both landed inside the dispatcher; `--back-to ADDR`
answers the same question exactly, and `core/history.h` has the numbers.

Media is outside the snapshots — disk images are immutable today — so a
rewind across a disk write will need the writes journalled alongside them
when write support lands.

## Symbol files

The debugger names what it can read out of the running machine: the BDOS
entry, the 17 CBIOS jump-table slots and the routines behind them all come
from the booted disk's own page zero, because that is the only honest
source for an address CP/M chose at link time. Everything *else* worth a
name — the IPL's RAM block, CBIOS's internals, the clock tick, the baud
table — is fixed for a given build and is written down instead:

```
./p2500-emu --symbols symbols/ipl.sym --disasm 02DA:8
./p2500-emu --disk disks/P25K_B.raw --symbols symbols/cpm.sym \
            --max-steps 3000000 --disasm E4B7:4
p2500-gui --disk disks/P25K_B.raw --symbols symbols/cpm.sym --panels disasm
```

```
$E4B7: 3A 4C E3     ld a,(CAPS.MASK)
```

Three files ship in `symbols/`:

- **`ipl.sym`** — the boot EPROM's entry points and the RAM block it
  builds at `$FE00`–`$FF2F`. Good for any run, since the IPL is the same
  ROM every time.
- **`cpm.sym`** — CBIOS and PBIOS internals, read off `P25K_B` and checked
  against every other disk here that boots: **41 of its 45 names are at the
  same address on `P25K_S` and `P25TEST`**, and the four that are not are
  live data (the clock tick, two buffers, the DPH table) that differs
  between two runs of the *same* disk. So it covers that whole BIOS
  generation.
- **`p25k_g.sym`** — the same for `P25K_G`, which carries the older
  `PHILIPS P2000B` generation and is a **different build, not a
  relocation**: both `SYSCBI.PHI`s load at `$E200` but G's is shorter and
  its CBIOS diverges from B's at the first instruction of CONIN, and while
  its `SYSPBI.PHI` loads exactly `$C0` above B's, the routines inside have
  drifted by `+$AF`, `+$BB`, `+$83`, `+$31` and `+$90`. So `cpm.sym` must
  not be used on that disk (it matches 3 of 45 names there, and `make test`
  asserts that too). This file was derived from G itself — its device
  table, its IM2 trampolines, its own `SELDSK` and `CONIN`, and lockstep
  disassembly against B — and it records four of B's names as having **no
  counterpart**, which is a finding rather than a gap: G's clock ISR keeps
  no tick counter, and its CBIOS has no key-translation table.

### Finding the right one by itself

`--symbols auto` (and the GUI, unless you name files yourself) identifies
the running build from its own bytes. Each file carries a fingerprint:

```
# match 0000:0400:1C8DB59E      a CRC32 over memory, as the CPU sees it
# match-rom 0000:1000:4CB66255  the same over the boot EPROM image
```

A file is loaded only if one of its rules holds, so a wrong name cannot
appear — and because `match` reads memory *through the CPU's view*, a rule
expires by itself when what it describes is paged out or overwritten.
`ipl.sym` fingerprints the EPROM at `$0000`, which is true at reset and
false the moment CBIOS banks it away — exactly when the IPL's names stop
being right. The GUI re-checks a few times a second, so one run names the
IPL during the boot and then the CBIOS that replaces it:

```
symbols: 47 name(s) from symbols/ipl.sym
symbols: 96 name(s) from symbols/cpm.sym
```

### Writing one

The format is one `ADDR NAME` per line — hex, `$` or `0x` optional, `:` or
`=` accepted instead of the space, `#` and `;` start a comment, blank lines
ignored. Names are at most 13 characters and are truncated (with a count
reported) rather than refused. A syntax error names the file and line and
is fatal, so a typo cannot quietly cost you a name. Files load in the order
given and the first name for an address wins, which means a name you
supply survives the machine's own scan. The GUI re-applies its `--symbols`
files after a reset, which clears the derived ones.

## The headless harness

`p2500-emu` is what `make test` drives, and is useful on its own for
scripted runs, probing, and disk-image inspection.

| Flag | |
|---|---|
| `--rom PATH` / `--charrom PATH` | boot EPROM, character generator |
| `--disks A[,B[,C[,D]]]` | attach images to the four drives in order; an empty field skips one |
| `--disk PATH` / `--disk-b` / `--disk-c` / `--disk-d` | the same, one drive at a time |
| `--swap-at MS:PATH` | change disks at an emulated-time offset; `MS:B:PATH` targets a drive |
| `--cartridge PATH` | attach a byte stream to the port $0F cartridge slot (e.g. a SESAM dongle stream) |
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
| `--disasm ADDR[:COUNT]` | disassemble, bank-aware, with symbol names |
| `--symbol ADDR:NAME` | name an address yourself; yours wins over a derived name |
| `--symbols FILE` | load names from a symbol file (repeatable) — see [Symbol files](#symbol-files) |
| `--step-over ADDR` | run to ADDR, then run the CALL there to completion and stop after it |
| `--history` / `--step-back N` / `--seek N` / `--back-to ADDR` / `--back-to-break` / `--verify-rewind N` | rewind — see [Rewind](#rewind) |
| `--verbose-io` | arm every device's diagnostics (very noisy) |
| `--no-stuck-detect` | disable the state-hash cycle detector |
| `--help` | print the flag list and exit |

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
| `mk_icon.py` | regenerates `src/gui/icon_data.h` (the embedded window icon) from `assets/icon/p2500icon.png` — only needed if the artwork changes |

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

runs `tests/run_tests.sh`: checks covering the cartridge-slot (SESAM)
boot path, the IPL and CP/M boot sequence, disk I/O (including mid-run swaps),
the video renderer (checked as pixels, not just state), the debugger
panels, the disassembler (`tests/disasm_crosscheck.py`, cross-checked
against `z80dasm` over ~34,000 instructions), the speed control, and
rewind. Exit code 1 on any failure.

Most checks drive `p2500-emu`, but `tests/fdc_eot.c` links `libp2500.a`
directly: it checks that a multi-sector read stops at the track's last
sector, which needs a DMA transfer longer than any real program here
requests.

`tests/render_vram.py` renders a `--dump-vram` capture to a PNG (`--attr`
for the attribute plane, `--graphics` for the 512×256 mode) — useful for
looking at a capture by hand while chasing a test failure.

## Project layout

```
src/core/   machine model (libp2500.a) — C11, no dependencies beyond libc
src/cli/    headless harness (p2500-emu)
src/gui/    SDL3 + Dear ImGui front-end (p2500-gui) — the only C++ in the tree
src/win/    the Windows .exe icon resource, built by windres under MINGW=1
tools/      disk-image and asset build tools (Python)
assets/     build assets like the application icon
tests/      the regression suite and its fixtures
demos/      three graphics demos + a benchmark, in Z80 assembly
disks/      CP/M disk images that boot on the emulator
roms/       the boot EPROM and character ROM dumps
symbols/    symbol files for the debugger (ADDR NAME, hex)
docs/       user-facing documentation (porting CP/M software, etc.)
```

`src/core/` holds the machine model — CPU wiring, the interrupt daisy
chain, video, and each device (`ctc`, `pio`, `dma`, `fdc`, `keyboard`,
`cartridge`) — and nothing in it may depend on either front-end. `src/cli/`
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