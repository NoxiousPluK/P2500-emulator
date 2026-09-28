# P2500 emulator

*A from-scratch emulator for the Philips P2000B / P2500 CPU card.*

**CP/M 2.2 boots to the `A>` prompt and runs typed commands, in a window
you can type into.** `p2500-gui` is an SDL3 front-end with a CRTC-driven
renderer and live keyboard input; `p2500-emu` is the headless harness that
`make test` drives. The remaining front-end work is the ImGui debugger
panels (`TODO.md` T39).

```
Philips P2500
58K CP/M Ver. 2.2

A>DIR
A: PIP      COM : SYSGEN   COM : SYSCBI   PHI : SYSLOAD  PHI
A: SYSPBI   PHI : SYSCPM   PHI

A>
```

Named "P2500" for brevity and to stay distinct from the unrelated
P2000T/P2000M/P2000C machines (a different, if related, product family). The
P2000B and P2500 are the same hardware under different case colours
(`../P2500-general-findings.md`), so one name covers both.

- **`TODO.md`** — the work queue, the decoded hardware model, and the
  address references. Read it first if you are here to do work.
- **`ROADMAP.md`** — the shape of the project and the reasoning about
  sequencing.

## Building

```
make        # libp2500.a, p2500-emu, and p2500-gui if SDL3 is present
make test   # the regression suite
```

`make` needs a C11 compiler and nothing else — the Z80 core is vendored and
there are no external libraries. Only `make gui` needs SDL3 (`extra/sdl3` on
Arch), and the core deliberately stays dependency-free so `make test` runs
with no display at all: 23 checks in ~25 s, exit 1 on any failure. The two
GUI checks skip themselves if `p2500-gui` has not been built.

## Running it as a machine

```
make gui
./p2500-gui --disk "../Disk Images/extracted/P25K_B/P25K_B.raw"
```

It boots to `A>` in a window and you can type at it — far enough that real
CP/M applications run: **SuperCalc2** (an OEM build whose splash reads
`PHILIPS P2000`) loads from `P25K_S` and opens files, and MBASIC-80 runs
from `P25TEST`. Geometry, cursor
position and cursor shape all come from the MC6845's registers rather than
being hardcoded, so the window follows whatever the guest programs.

| Key | |
|---|---|
| any printable key | sent as ASCII on port `$06` |
| Return / Backspace / Tab / Esc / Delete | `$0D` / `$08` / `$09` / `$1B` / `$7F` |
| arrow keys | `$8B` `$87` `$89` `$85` — the WordStar diamond CBIOS's own table at `$E274` decodes |
| Ctrl+letter | `^A`–`^Z`, so Ctrl-C warm-boots CP/M |
| F11 | turbo (8x) |
| F12 | pause |

`--scale N` sets the initial zoom; the window is resizable and letterboxes
with integer scaling rather than stretching. `--frames N --screenshot f.ppm`
runs a fixed number of video fields and saves what is on screen, and
`--push-at MS:STRING` scripts keystrokes — together they let the front-end
be driven to a real application and compared against the CLI with no display
involved. Driven to SuperCalc2, the two renders are byte-identical.

## Running

```
./p2500-emu                                          # IPL only
./p2500-emu --disk "../Disk Images/extracted/P25K_B/P25K_B.raw" \
            --max-steps 20000000 --type 'dir\r' --dump-vram /tmp/vram.bin
python3 tools/render_vram.py /tmp/vram.bin /tmp/screen.png
```

| Flag | |
|---|---|
| `--rom PATH` | boot EPROM image, default `roms/ipl.bin` |
| `--disk PATH` | attach a raw disk image |
| `--swap-at MS:PATH` | change disks at an emulated-time offset |
| `--sesam PATH` | attach a byte stream to the SESAM dongle port |
| `--type STRING` | queue keystrokes; `\r` `\n` `\t` `\xHH` understood |
| `--type-after MS` | hold the first keystroke until CBIOS has initialised its ring buffer (default 4000) |
| `--type-at MS:STRING` | queue keystrokes at a specific emulated-time offset |
| `--max-steps N` | instruction budget, default 2,000,000 |
| `--dump-vram PATH` / `--dump-ram PATH` | write video RAM / all 64 KB at exit |
| `--dump-vram-attr PATH` | write the 4-bit video attribute plane (TODO.md T27) |
| `--dump-screen f.ppm` | render the screen through the core's own renderer — the same call the GUI makes |
| `--charrom PATH` | character generator ROM, default `roms/charrom.bin` |
| `--push-at MS:STRING` | push keystrokes into the **live** keyboard ring, the path a GUI keypress takes |
| `--peek ADDR:LEN` / `--poke ADDR:HEXBYTES` | inspect / patch memory |
| `--watch ADDR[:LEN]` / `--count ADDR` / `--break ADDR` | trace writes, count executions, stop at an address |
| `--verbose-io` | log every I/O port access (very noisy) |
| `--no-stuck-detect` | disable the state-hash cycle detector |

`P2500_TRACE_FROM` / `P2500_TRACE_TO` in the environment give a bounded
per-step instruction trace.

Disks can be changed mid-run, and CP/M handles it the way it does on real
hardware — swap, then Ctrl-C at the prompt to force a warm boot and re-read
the directory:

```
./p2500-emu --disk "../Disk Images/extracted/P25K_B/P25K_B.raw" \
  --max-steps 120000000 \
  --type-at '4000:dir\r' \
  --swap-at '11000:../Disk Images/extracted/P25TEST/P25TEST.raw' \
  --type-at '13000:\x03' --type-at '20000:dir\r' \
  --dump-vram /tmp/vram.bin
```

## Validation

Two independent guards, both run by `make test`.

**The SESAM banner is byte-exact.** `roms/sesam_banner_test.bin` is the same
synthetic bootable-cartridge stream the Python-era `emulate_sesam_cartridge.py`
used (16-byte header + a 7-byte payload that calls the ROM's own `$0174`
banner-print routine against its own real banner table, then halts). Its
`--dump-vram` output is **byte-for-byte identical** to the Python-era
`vram_after_banner.bin`, so the CPU core, the SESAM port, the memory map and
the video write path all still agree with previously-validated behaviour.
This is the oldest test in the project. Keep it green.

**CP/M is checked by observable state, not by "it didn't crash".** Page zero
must hold CP/M's real vectors, CBIOS's `$E200` jump table must be `JP`
instructions, the screen must read `58K CP/M Ver. 2.2` with an `A>` prompt,
and typing `DIR` must list exactly the six files the disk image contains.
That last check exercises the whole machine at once: CTC channel 3 and port
`$06` in, the interrupt daisy chain, the FDC/DMA read path, and CONOUT. The
exit report also asserts that every interrupt request is acknowledged exactly
once — the failure mode that cost this project the most time was a *silent*
interrupt drop, so it is now impossible for one to go unreported.

## Why this exists, and the projects that informed it

Built to continue the dynamic-emulation work in the parent research
project's `ROM Dumps/CPU-Card-Boot-EPROM/emulation/` once that hit a hard
limit: the Python `z80` package used there has no clean way to inject real
IM2 interrupts, and the ROM's own floppy driver depends on them for real.

- **Z80 core**: [`superzazu/z80`](https://github.com/superzazu/z80) (MIT),
  vendored in `src/core/vendor/superzazu_z80/`. Chosen over fighting the
  Python `z80` binding because it exposes a single clean call,
  `z80_gen_int(z, vector_byte)`, to fire a real maskable interrupt at the
  exact moment we want — no global state, no polling callback. Also plain
  8-bit port numbers, avoiding a gotcha the earlier Python work had to work
  around (that core exposed the real Z80 16-bit port bus, including the
  accumulator's high byte).
- **FDC design informed by [`ifilot/p2000m-emulator`](https://github.com/ifilot/p2000m-emulator)**:
  that project emulates the P2000M, a real sibling machine from the same
  Philips engineering lineage with a genuine µPD765-based floppy driver, and
  ships an excellent primary-source reference (`P2000M_FLOPPY_CONTROLLER.md`,
  transcribed from a real Philips Field Support Manual) plus a clean,
  complete `P2000Fdc` C++ class. **Not reused verbatim** — the P2500's ports,
  control-latch bits and disk geometry are all different, and this project's
  own reverse-engineered ROM command tables were the source of truth for
  `src/core/fdc.c` — but that document independently confirmed this project's
  command decode (same command family, same order: Sense Interrupt Status →
  Specify → Recalibrate → Seek) and the phase-state-machine design. Their
  bundled `p2000.rom` was checked byte-for-byte against this project's P2500
  IPL dump: completely different firmware (35/4096 bytes coincidentally
  match — statistical noise), as expected for a different machine class.

## Files

The tree is split three ways. `src/core/` is the machine model and depends
on nothing but libc; `src/cli/` is the headless harness `make test` drives;
`src/gui/` will be the SDL3 + Dear ImGui front-end (`TODO.md` T36+) and does
not exist yet. **Nothing in `core/` may depend on either front-end.**

**`src/core/` → `libp2500.a`**

- `machine.{c,h}` — memory map, port dispatch, Z80 core wiring, and the
  per-instruction step that advances every device and arbitrates interrupts
- `intctl.{c,h}` — the IM2 daisy chain: hold-until-acknowledged, priority by
  chain position, release on `RETI`
- `video.{c,h}` — the text renderer, driven by the MC6845 registers. 8x12
  cells read from the character ROM's 16-byte stride, cursor from R14/R15
  with its shape from R10/R11, and the card's 4-bit attribute plane — which
  the CPU fills by latching a nibble in port `$0A` rather than by addressing
  it (`TODO.md` T27)
- `ctc.{c,h}` — Z80A-CTC at `$00`–`$03`, T-state driven with a real 16/256
  prescaler and per-channel CLK/TRG sources decoded from CBIOS's own ISRs
- `pio.{c,h}` — Z80A-PIO at `$10`–`$13` (the FDD card)
- `dma.{c,h}` — Z80A-DMA at `$16`, a real self-describing register-stream
  parser
- `fdc.{c,h}` — the µPD765 model (`$14`/`$15`)
- `keyboard.{c,h}` — port `$06` keyboard in, port `$04` serial out
- `sesam.{c,h}` — the SESAM dongle / bootable-cartridge port (`$0F`)
- `vendor/superzazu_z80/` — the vendored Z80 core. One local addition, marked
  as such: an optional `on_reti` callback, without which the daisy chain
  cannot see `RETI` and so cannot model IEO release

**`src/gui/` → `p2500-gui`**

- `main.c` — SDL3 front-end on the callback app model
  (`SDL_AppInit`/`SDL_AppIterate`/`SDL_AppEvent`), one streaming texture,
  integer scaling, single-threaded. One video field of emulation per
  presented frame, so the frame loop and the guest's own 50 Hz clock strobe
  are the same event by construction

**`src/cli/` → `p2500-emu`**

- `main.c` — the headless harness: landmark tracking, state-hash stuck-loop
  detection, all the debug flags above, and the exit report

**Elsewhere**

- `roms/` — `ipl.bin` and `charrom.bin`, copies of this project's own dumped
  ROMs (see `../ROM Dumps/`), plus two SESAM cartridge fixtures:
  `sesam_banner_test.bin` (the byte-exact regression) and
  `sesam_bank_probe.bin` (selects an undecoded video bank, guarding the T27
  tripwire)
- `tools/run_tests.sh` — the regression suite behind `make test`
- `tools/render_vram.py` — video-RAM-dump-to-PNG renderer. Renders the real
  8×12 character cell and the card's four attributes; `--demo-attrs`
  synthesises an attribute plane, which is the only way to exercise that
  path since no surviving software sets one
- `tools/disasm_ram.sh` — disassembles a `--dump-ram` image at its real
  addresses. The only usable way to read the CP/M system files, whose
  on-disk `.phi` form is sector-interleaved, so the `org 0` listings in
  `../Disk Images/disassembly/` have no meaningful addresses
- `tools/cpm_extract.py` — extracts files from a P2500 CP/M disk image.
  Necessary rather than convenient: P2500 tracks store their logical sectors
  in the physical order `0,2,…,14,1,3,…,15`, and a `.raw` is in physical
  order, so an extractor that ignores the interleave reads half the disk
  from the wrong place. Derives reserved tracks, block size and extent mask
  from the image, then verifies what it produced — block-boundary continuity
  is the check that actually catches a bad sector map. Validated by
  producing exactly the six files `DIR` reports on `P25K_B`, and a
  byte-identical `PIP.COM` from three independently dumped floppies
- `tools/imd_tool.py` — ImageDisk verifier and normalizer. `verify` reports
  whether an image is a complete dump (four of this project's nine are not —
  they were double-stepped and are missing every other track); `convert`
  writes a `.raw` indexed by sector-ID cylinder, which for a healthy image is
  byte-identical to the existing one
- `shots/` — rendered screens, with their source VRAM dumps
