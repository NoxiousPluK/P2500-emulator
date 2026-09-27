# Roadmap — P2500 emulator

Where this is going and in what order. `TODO.md` is the work queue with
the evidence behind each item; this file is the shape of the project and
the reasoning about sequencing. Read `TODO.md` first if you are here to do
work.

## Goal

Boot an unmodified Philips P2500 / P2000B into CP/M 2.2 from a real disk
image, with a working screen and keyboard, on a hardware model that is
*decoded from firmware* rather than approximated with tuning constants.
The UCSD p-System disks in `../Disk Images/extracted/` are the second
target and a good independent check on the disk path.

## Guiding principle

**Every mechanism in this emulator should trace to a specific byte in a
real dump.** The project's recurring failure mode has been introducing a
plausible-sounding mechanism (a free-running counter, a synthetic `$3019`
handoff, a step-count cooldown), tuning it until something moved, and then
building further work on top of it. Each of those later turned out to be
covering for a misread port. When a busy-wait will not clear, the answer
has so far *always* been in the ROM, not in a constant.

Corollary: **address-diversity is not evidence of progress.** Zeroed RAM
decodes as `NOP` and produces an ever-growing distinct-address count that
is indistinguishable from real execution. Landmarks must be validated
against the opcode actually present at that address (`TODO.md` T8).

Corollary: **"it got further" is not evidence the mechanism is right.**
`TODO.md` T16's CTC channel chaining cleared a real stall and is very
likely still wrong (`TODO.md` T19) — and the `channel3_rx_ready`
parameter added on top of it is the tell: a second special case needed to
keep the first one from misbehaving. When a fix needs a qualifier, look
for the mechanism it is standing in for before building on it.

Corollary: **check how far "derived from firmware" actually reaches before
declaring something unmeasurable.** The IM2 daisy-chain order looked like a
pure wiring fact that only a multimeter could settle. It is not: the IPL's
DMA and PIO handlers share one saved-SP word and one private stack, so only
one of the two possible orders lets them coexist, and getting it wrong kills
the machine in an `RST 38` loop (`TODO.md` T17). Two of the three "hardware
only" questions this project has raised turned out to be answerable from
bytes already in the repository.

Corollary: **a silent drop is worse than a stall.** The single-slot
interrupt model (`TODO.md` T17) discards one device's entire interrupt
stream without any diagnostic, and did so for the whole of Phase 1. Every
model that can refuse to act should say so when it does.

---

## Phase 0 — historical: the state before Phase 1

Kept because `TODO.md`'s P0–P2 work queue is written against it. For where
the emulator actually is now, skip to "Current state" below.

Working and validated:

- A vendored `superzazu/z80` core wired to flat 64 KB RAM.
- MC6845 CRTC model at `$08`/`$09` — register values decode to 80×24 at
  12 scanlines/row, matching the official P2219 manual's 640×288 mode.
- SESAM dongle / bootable-cartridge model at `$0F`, with a **byte-exact
  regression test**: `roms/sesam_banner_test.bin` + `--dump-vram` still
  reproduces the Python-era `vram_after_banner.bin` byte for byte. This is
  the project's only hard regression test. Keep it green.
- A µPD765 command/result-phase model at `$14`/`$15` that correctly
  decodes SPECIFY, SENSE INTERRUPT STATUS, SENSE DRIVE STATUS,
  RECALIBRATE and SEEK.
- A headless CLI harness with landmark tracking, stuck-loop detection,
  `--poke`, and a bounded per-step trace.

Broken or fictional (see `TODO.md` for the evidence):

- The Z80-CTC model at `$12`/`$13` — that is a Z80-PIO.
- The absent Z80-DMA at `$16` — currently mis-modelled as an unhandled
  "stepper pulse" port, and it is the reason `READ DATA` has no data path.
- The `$3019` disk intercept — that address is a relocated RAM test.
- No port `$05` bank switching, so the ROM image is erased mid-run.

Net: the emulator reaches real ROM logic as far as the `$0003` dispatch,
but partly for the wrong reasons, and cannot get further without the
above.

---

## Current state (2026-09-27)

**CP/M 2.2 boots to the `A>` prompt and runs commands.** `make test` is the
proof and the guard — 17 checks, ~8 s, exit 1 on any failure. Reproduce the
headline result with:

```
./p2500-emu --disk "../Disk Images/extracted/P25K_B/P25K_B.raw" \
            --max-steps 20000000 --type 'dir\r' --dump-vram /tmp/vram.bin
```

which renders:

```
Philips P2500
58K CP/M Ver. 2.2

A>DIR
A: PIP      COM : SYSGEN   COM : SYSCBI   PHI : SYSLOAD  PHI
A: SYSPBI   PHI : SYSCPM   PHI

A>
```

Those six files are exactly what `../Disk Images/extracted/P25K_B/` holds,
so the listing is fixed by the disk image rather than by anything in this
emulator. Phase 1's SESAM banner regression is still byte-exact.

What that took, over and above the Phase 1 model:

- **A real IM2 daisy chain** (`src/intctl.{c,h}`, `TODO.md` T17). The
  vendored core's single pending-interrupt slot had been silently discarding
  one device's entire interrupt stream: the IPL's DMA end-of-block handler
  `$07F3` had never executed in any run of this emulator. It now runs 87
  times, once per transfer, and every request is acknowledged 1:1 (the exit
  report prints the counts, so a future drop cannot be silent). This alone
  cleared the `$EB02` deadlock — it was the disk completion that was never
  arriving, not the clock.
- **The chain order, derived rather than guessed.** The DMA has to be
  upstream of the PIO; the IPL's own stack discipline says so and the other
  order dies in an `RST 38` loop. See the guiding principle's third
  corollary.
- **A T-state-driven CTC with a real prescaler** (`TODO.md` T18). The ×4
  fudge is gone. The payoff is a falsifiable check: CBIOS's table at `$F546`
  now decodes to 75/110/150/300/600/1200/2400 baud, each within 0.3%, which
  it cannot do under a wrong prescaler or a per-instruction tick.
- **The CTC's actual per-channel wiring** (`TODO.md` T19). The
  `ch0 ZC/TO → ch1/2/3` chain did not exist. ch0 is the serial transmit bit
  clock (TIMER, no CLK/TRG at all); ch1's CLK/TRG is the serial RXD line;
  ch2's is an unidentified 50 Hz strobe feeding the real-time clock; ch3's is
  the keyboard's "byte ready" strobe. `channel3_rx_ready` is gone.
- **Port `$05` as an input** (`TODO.md` T20): bit 7 is RXD, bit 6 is a
  transmit handshake. The review's first reading had the bit numbers swapped.
- **`tools/disasm_ram.sh`**, which is the reason any of the CBIOS findings
  are checkable. The `.phi` files are sector-interleaved on disk, so the
  `org 0` listings in `../Disk Images/disassembly/` have no usable
  addresses; a `--dump-ram` image does.

Known-imperfect, deliberately:

- **Video attributes are not modelled** (`TODO.md` T27). The flat
  `$8000`-`$BFFF` byte bank renders everything printed so far correctly
  because none of it uses attributes; the card's 12× MB8116E says the real
  organisation is 16 K × 12 bits.
- **No write path** (`TODO.md` T30). Reading works; `WRITE DATA` and
  `FORMAT A TRACK` are decoded but not implemented, and the DMA↔FDC data
  flow is still inverted, which is what blocks them.
- **CTC channel 2's 50 Hz is an assumption** (`TODO.md` T29) — the one
  frequency in the emulator not derived from the 4 MHz crystal.

---

## Phase 1 — a correct floppy path (`TODO.md` T1–T12) — COMPLETE

Confirmed live: the boot sector's own `READ DATA` runs for real, the DMA
delivers a real transfer, and execution reaches `$4A00` un-gated. See
`TODO.md`'s ISSUE-1 through ISSUE-4 for the four compounding bugs that
sat between "wired up" (T1–T12 all checked off) and "actually runs" -
none were in the T1-T12 wiring itself. The boot goes on to load and jump
into CP/M's own CBIOS (`SYSPBI.PHI`), i.e. Phase 2 has already begun.

The one milestone that matters: **the boot sector's own
`LD DE,$1030 / CALL $0003` produces a real `READ DATA`, the DMA delivers
32 sectors to `$1000`–`$2FFF`, and execution reaches `$4A00`.**

That is a hard, falsifiable success criterion — `$4A00` is the address the
real boot sector jumps to after its own `LDIR`, and nothing can reach it
by accident.

Order matters here. T1 (bank switching) and T2 (delete the `$3019`
intercept) must land together or the build regresses: today the intercept
is the only thing stopping the RAM test from erasing the ROM. T3–T6 (PIO,
FDC interrupt routing, DMA, READ DATA) are one coherent change and are
hard to land incrementally, because the PIO is what decides when the FDC's
interrupt is allowed through.

**Expected side effect:** every tuning constant in the codebase
disappears. If Phase 1 lands and something still needs a magic number,
that is a signal a mechanism is still mis-modelled.

## Phase 2 — CP/M actually running — COMPLETE

Once sector data lands, control passes out of the IPL entirely and into
`SYSLOAD.PHI` → `SYSCBI.PHI` / `SYSPBI.PHI` / `SYSCPM.PHI`, all of which
are already extracted in `../Disk Images/extracted/P25K_B/`. This code is
much larger than the IPL and exercises more hardware:

- **Multi-sector, multi-track transfers.** The IPL only ever reads from
  track 0. The CP/M BIOS seeks. The P2500-specific quirk matters here: the
  logical track number in each sector's ID field is **physical track + 1**
  (`../Disk Images/findings/Image info.md`). Sector skew is
  odd-then-even (1,3,5…,2,4,6…).
- **Write and format paths.** Already decoded (`$05A1` → WRITE DATA via
  `$0B43`; `$05A6` → FORMAT via `$0B19`), just never exercised.
- **The `+8` port cluster** (`$18`/`$19`/`$1A`/`$1C`/`$1D`/`$1E`) appears
  in `SYS09.PHI` and is most likely the optional 8" drive interface.
  Ignore it unless a disk image needs it.
- **Port `$05` reads.** `SYSPBI.PHI` reads port `$05` at four points; the
  IPL never does. Something is readable there and the emulator will have
  to answer correctly.

- **The interrupt model has to be right first.** Phase 1 survived a
  broken one by luck; CBIOS runs several interrupt sources concurrently
  and does not (`TODO.md` T17).

**Milestone met (2026-09-27): the CP/M `A>` prompt in a `--dump-vram`
render, and `DIR` listing the disk's real contents.** The order that worked
was T17 (interrupt controller) → T18/T19 (CTC timing and wiring) → T20
(port `$05` read), with T24 (`make test`) and T25 (harness) landing
alongside rather than last — T25's `--dump-ram` plus `tools/disasm_ram.sh`
is what made T19 and T20 answerable at all. ISSUE-5 needed no separate fix;
T17 dissolved it.

Of the two items left on this list, neither turned out to be on the path to
the prompt, and both moved to P4: **write and format** (`TODO.md` T30, which
needs the DMA↔FDC inversion first) and **multi-track reads past `EOT`**
(`TODO.md` T31). CP/M's directory reads stay inside one track, which is why
`DIR` is correct without them. Port `$05` reads are done (T20). The `+8`
port cluster is still untouched and still looks like the 8" interface.

## Phase 3 — console

Needed before the machine is interactive, not before it boots.

- **Video.** The CRTC model already computes the right geometry. What is
  missing is the video card's actual memory organisation: 12× MB8116E
  (16 Kbit×1) is very likely 16 K words × **12 bits** — an 8-bit character
  code plus a 4-bit attribute nibble — not a plain 16 KB byte bank. The
  flat `$8000`–`$BFFF` model renders the boot banner correctly because the
  banner uses no attributes — and it turns out to render CP/M's sign-on,
  prompt and `DIR` output correctly for exactly the same reason, so the
  difference still has not been forced. The first thing that uses reverse
  video or dim text will force it (`TODO.md` T27).
- **Keyboard — DONE.** The long-running confusion here is resolved, and
  both readings were half right. There are **two** input paths, not one:
  - The **keyboard** is port `$06`, byte-wide, announced by a strobe on CTC
    channel 3's CLK/TRG. `$ED2D` does a single `IN A,($06)` per interrupt
    into a 32-byte ring. `src/keyboard.h`'s byte-level model was correct.
  - The **serial port** is the bit-banged one, on CTC channels 0 and 1:
    transmit on port `$04` clocked by channel 0, receive by sampling port
    `$05` **bit 7** once per bit cell under channel 1 (`TODO.md` T19/T20).
    Channel 1's CLK/TRG is the RXD line itself, so the start bit triggers
    the sequence.
  Typing works end to end and `make test` asserts it. The T-state question
  answered itself as predicted: the CTC is driven from `z->cyc` with no
  cycle-accuracy elsewhere (`TODO.md` T18).
- **Printer.** Port `$04`, transmit-only, out DB25 pin 3 — and the rate is
  no longer a guess: it comes from CBIOS's own baud table at `$F546`
  selected by `$F727`, which is 1200 baud on this disk. Everything needed is
  modelled and nothing exercises it yet, so `PIP LST:=FILE.TXT` is both the
  remaining work and its own test (`TODO.md` T28). A wrong bit rate produces
  recognisably mangled characters through the existing `[tx]` logging rather
  than silence, which makes it a real check on T18.

## Phase 4 — everything else

Not on the critical path; listed so it is not forgotten.

- **UCSD p-System boot** (`P2k5_LOGIC`, `P2k5_TKS`) — a genuinely
  different bootstrap, and therefore a real independent test of the disk
  path.
- **SESAM dongle emulation for real software.** The model already handles
  the protocol; what is missing is a real key's 3 bytes. Any P2219 disk
  that refuses to boot with `INIT ERROR` is testing this.
- **The FXD/SASI card.** No driver has been found on any disk image, and
  the card has no confirmed provenance to this unit. Out of scope until
  either a Winchester-configured `PBIx.PHI` turns up or the card is traced
  with a logic analyzer.
- **A real display front-end — now planned, see `TODO.md` P5 (T33–T40).**
  SDL3 (already packaged and installed) plus a vendored Dear ImGui, behind a
  `core` / `cli` / `gui` split that keeps `libp2500.a` dependency-free so
  `make test` never needs a display. The GUI's primary purpose is a
  **debugger** — device-state, memory, disassembly and IRQ-log panels — not
  a settings dialog: every advance in this project has come from
  instrumentation, and a live view of the daisy chain and the CTC channels
  would have turned T17 and T19 from archaeology into inspection. Video
  output depends on T27; live keyboard input retires the last non-derived
  constant in the model (`P2500_KEYSTROKE_HZ`). An Emscripten build falls
  out nearly free if SDL3's callback app model is used from the start.

  A MAME driver is a plausible eventual sibling, but it would reuse the
  *findings* and `make test` as an oracle rather than the code — MAME has
  its own `z80daisy`/`z80ctc`/`z80pio`/`z80dma`/`upd765` devices. Worth
  keeping the core clean for, not worth contorting it for.

---

## What hardware work would help, and what would not

The short answer has been **nothing is blocked on hardware** through both
Phase 1 and Phase 2, and the Phase 2 review's claim that it had found the
project's first un-derivable questions did not survive contact with the
work. Two of the three were answerable from bytes already here:

- The **daisy-chain order** between the DMA and the PIO is fixed by the
  IPL's own stack discipline (`TODO.md` T17) — the other order kills the
  machine. Only the CTC's position relative to the FDD card is still open,
  and it is only observable when a CTC tick collides with a disk interrupt.
- **What the CTC's CLK/TRG pins are** was answered by reading CBIOS's four
  ISRs (`TODO.md` T19): channel 0 has no CLK/TRG input at all, channel 1's
  is the serial RXD line, channel 3's is the keyboard strobe. Only channel
  2's remains unidentified — and its *rate* may yet be derivable too
  (`TODO.md` T29).

So the list below is shorter than the review expected. Measurement would
still settle these faster than firmware archaeology, and refuting any of
them would be more valuable than confirming it.

Ranked by value to this emulator, with the full checklist in
`../Tracing/P2500-predicted-wiring-from-firmware.md`:

1. **Where the CTC sits in the IM2 daisy chain**, relative to the FDD
   card's PIO and DMA. The DMA-before-PIO half is settled (`TODO.md` T17);
   this half is not, and it decides whether a 50 Hz clock tick can pre-empt
   a disk handler that has released itself early with `EI`/`RETI`. Tracing
   `IEI`/`IEO` between the Z8430, Z8420 and Z8410 is a continuity check.
   The emulator currently puts the CTC last, which is the conservative
   reading.

2. **What pulses CTC channel 2's CLK/TRG** — the 50 Hz real-time clock tick
   (`TODO.md` T29). Mains-derived or the video card's frame rate; both are
   50 Hz, so the emulator is right either way, but this is the one frequency
   in the codebase not derived from the 4 MHz crystal. Probe channel 2's
   CLK/TRG pin and see where it comes from.

3. **Port `$05`'s remaining readable bits** (`TODO.md` T20). Bit 7 is RXD
   and bit 6 is a transmit handshake — both confirmed from the code that
   reads them. Bits 0–5 are unknown and currently read back as 1. This is
   a live polled input now, not a write-only latch.

4. **FDD card: which µPD765 signals reach PIO port A bits 0 and 1.** The
   emulator has to guess this in `TODO.md` T4. A 10-minute continuity
   check settles it. Refuting it would be *more* valuable than confirming
   it.
5. **FDD card: PIO port B direction (`$A1` mask → PB0/PB5/PB7 inputs).**
   One measurement that validates or kills the entire PIO identification.
6. **CPU card: the port `$05` latch and what its outputs gate.** Turns
   `TODO.md` T1 from a reasoned guess into a fact, and is needed properly
   for Phase 3's video-RAM window.
7. **CPU card: the four `515xx` decode PROMs' address inputs.** Their
   truth tables are already dumped but inert without the wiring. Would
   give the complete memory and I/O map in one go.
8. **The Philips P2500 System Reference Manual (`5103 992 30421`).**
   Supersedes all of the above. Still the single biggest documentation gap
   in the whole project.

Things that would *not* help much right now: the SASI/FXD card, the
backplane's glue logic, the DIP switch (all 8 read ON, so there is no bit
pattern to correlate), and the video card's DIN connectors. All of those
matter eventually; none of them unblocks anything.

---

## Cross-references

- `TODO.md` — the work queue, with per-item evidence and gotchas
- `../ROM Dumps/CPU-Card-Boot-EPROM/disassembly/findings.md` — the IPL
- `../Disk Images/findings/BIOS-disassembly-findings.md` — the CP/M BIOS
- `../Tracing/P2500-predicted-wiring-from-firmware.md` — what to measure
- `../P2500-System-Specifications.md` — the consolidated spec sheet
