# P2500 emulator

*A from-scratch emulator for the Philips P2000B / P2500 CPU card.*

**CP/M 2.2 boots to the `A>` prompt and runs typed commands.** Headless so
far — there is no display and no live keyboard yet; the screen is inspected
by dumping video RAM and rendering it to a PNG. A real SDL3 + Dear ImGui
front-end is planned and is the next major piece of work (`TODO.md` P1).

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
make
make test
```

Needs a C11 compiler and nothing else — the Z80 core is vendored, there are
no external libraries. `make test` is the regression suite: 19 checks in
~8 s, exit 1 on any failure.

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
- `tools/imd_tool.py` — ImageDisk verifier and normalizer. `verify` reports
  whether an image is a complete dump (four of this project's nine are not —
  they were double-stepped and are missing every other track); `convert`
  writes a `.raw` indexed by sector-ID cylinder, which for a healthy image is
  byte-identical to the existing one
- `shots/` — rendered screens, with their source VRAM dumps
