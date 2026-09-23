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

## Current state (reviewed 2026-09-23)

Everything in the "broken or fictional" list above is fixed, and the
machine boots well past Phase 1's success criterion. Reproduce with
`./p2500-emu --disk "../Disk Images/extracted/P25K_B/P25K_B.raw"
--max-steps 5000000`:

- The IPL runs its own RAM test, PIO/DMA/FDC init, `RECALIBRATE` and
  `READ DATA`; the DMA delivers a real transfer; sector 0 runs at `$1000`
  and reaches `$4A00` (step ~801,000).
- `SYSLOAD`/`SYSPBI` load and run. **CP/M page zero is real**: `$0000` =
  `C3 03 E2` (CBIOS warm boot), `$0005` = `C3 06 D4`. CBIOS's `$E200`
  jump table is live; the CCP sits at `$CC00` with its Digital Research
  sign-on string intact; CBIOS installs its own IM2 table at `I=$FF`.
- It then **deadlocks** in a 64-address cycle at `$EB02`, waiting on an
  event semaphore that stops being signalled (`TODO.md` ISSUE-5).

Two things found by the review are load-bearing and change the order of
the remaining work:

1. **The interrupt model silently discards interrupts** (`TODO.md` T17).
   The vendored core has one pending-interrupt slot; whichever device
   asks last wins. Measured: CTC channel 1's ISR runs **0 times against
   19,106 requests**, and the IPL's DMA end-of-block handler `$07F3` has
   never executed in any run. This is the top blocker, and it invalidates
   any conclusion of the form "device X's interrupt is delivered"
   whenever more than one source is active.
2. **The CTC channel wiring is probably wrong, and its timing is
   arbitrary** (`TODO.md` T18, T19). The `ch0 → ch1/2/3` chain is
   contradicted by channel 2 counting the opposite CLK/TRG edge; the real
   channels are identifiable from CBIOS's own handlers (`$F669` re-arms
   channel 1 as a bit-cell timer, `$F597` bit-samples port `$05`,
   `$F37F` is a clock tick). Separately, `ctc.c` ticks once per
   *instruction* with the prescaler folded in as ×4 instead of ×16, so no
   timing conclusion drawn from it is trustworthy — and `z->cyc` is
   already there to fix it properly.

Also newly known: **port `$05` is a read port**, not only the bank latch
(`IN A,($05) / BIT 6,A` at `$F5A4`) — `TODO.md` T20 settles T15's open
question.

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

## Phase 2 — CP/M actually running

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

Order for the rest of Phase 2: T17 (interrupt controller) → T18/T19 (CTC
timing and wiring) → T20 (port `$05` read) → re-measure ISSUE-5 → T22/T23
(DMA direction inversion, then WRITE/FORMAT/READ ID). T24 (`make test`)
should land early, not last: Phase 1 was expensive and is currently
guarded only by the SESAM banner diff.

Milestone: the CP/M `A>` prompt in a `--dump-vram` render.

## Phase 3 — console

Needed before the machine is interactive, not before it boots.

- **Video.** The CRTC model already computes the right geometry. What is
  missing is the video card's actual memory organisation: 12× MB8116E
  (16 Kbit×1) is very likely 16 K words × **12 bits** — an 8-bit character
  code plus a 4-bit attribute nibble — not a plain 16 KB byte bank. The
  current flat `$8000`–`$BFFF` model renders the boot banner correctly
  because the banner uses no attributes. Real CP/M console output will
  expose the difference.
- **Keyboard.** Bit-banged serial on the bottom DB25, timed off the
  Z80A-CTC at ports `$00`–`$03`. The CTC model exists (`TODO.md` T16) and
  CBIOS's receive path has been located: channel 1's ISR (`$F597`)
  bit-samples **port `$05` bit 6**, with channel 0's ISR re-arming
  channel 1 as a per-bit-cell TIMER. So the earlier "no bit-level timing
  needed, port `$06` delivers whole bytes" reading (`src/keyboard.h`) is
  at best only half the story. The T-state accuracy question answers
  itself: the vendored core already keeps `z->cyc`, so the CTC can be
  driven from real T-states without making the rest of the emulator
  cycle-accurate (`TODO.md` T18).
- **Printer.** Port `$04`, transmit-only, 9600-8N-2 out DB25 pin 3. Cheap
  to add once the CTC exists, and a nice early win: `PIP LST:=FILE.TXT`
  becomes observable.

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
- **A real display front-end.** Deliberately deferred — headless plus
  `--dump-vram` has been sufficient and keeps the build dependency-free.
  Worth doing only once Phase 3 makes interactivity meaningful.

---

## What hardware work would help, and what would not

The short answer for Phase 1 was **nothing is blocked on hardware** — the
firmware answered every question it needed. That is no longer quite true.
Phase 2 has produced the project's first genuinely un-derivable questions:
the interrupt daisy-chain order and what pulses the CTC's CLK/TRG pins are
board facts that firmware can only be consistent with, never reveal. They
are not blocking (pick an order, name it in one constant, say it is a
choice) but they are the first items on this list that measurement would
*settle* rather than merely confirm.

Ranked by value to this emulator, with the full checklist in
`../Tracing/P2500-predicted-wiring-from-firmware.md`:

1. **The IM2 daisy-chain order across the two cards.** The CTC is on the
   CPU card; the PIO and DMA are on the FDD card. Which is `IEI`-upstream
   decides whether a CTC tick can pre-empt a disk transfer's end-of-block
   — the one input `TODO.md` T17 cannot derive from firmware, because
   firmware never sees the chain, only its consequences. Tracing `IEI`/
   `IEO` between the Z8430, Z8420 and Z8410 is a continuity check.

2. **What drives CTC channels 1, 2 and 3's CLK/TRG pins**, and whether
   channel 0's `ZC/TO` leaves the chip at all. `TODO.md` T19 identifies
   what the three interrupt *handlers* do; this says what actually pulses
   them, and would retire the last guessed mechanism in the codebase.

3. **What port `$05`'s readable bits are** (`TODO.md` T20). Bit 6 is
   almost certainly RXD; the rest are unknown, and this is now an input
   the running machine polls, not a write-only latch.

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
