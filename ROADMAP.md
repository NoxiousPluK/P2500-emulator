# Roadmap — P2500 emulator

Where this is going and in what order. `TODO.md` is the work queue with the
evidence behind each item and the decoded hardware model; this file is the
shape of the project and the reasoning about sequencing. Read `TODO.md`
first if you are here to do work.

Rewritten 2026-09-28 (previous: `5e73169`). Completed phases are summarised
rather than narrated; `git log -p --follow ROADMAP.md` has the long form.

## Goal

Boot an unmodified Philips P2500 / P2000B into CP/M 2.2 from a real disk
image, with a working screen and keyboard, on a hardware model that is
*decoded from firmware* rather than approximated with tuning constants.

## Guiding principle

**Every mechanism in this emulator should trace to a specific byte in a real
dump.** The project's recurring failure mode has been introducing a
plausible-sounding mechanism, tuning it until something moved, and then
building on it. Each later turned out to be covering for a misread port.
When a busy-wait will not clear, the answer has so far *always* been in the
ROM, not in a constant.

Corollaries, each paid for:

- **Address-diversity is not evidence of progress.** Zeroed RAM decodes as
  `NOP` and produces an ever-growing distinct-address count indistinguishable
  from real execution. Landmarks must be validated against the opcode
  actually present at that address.
- **"It got further" is not evidence the mechanism is right.** The CTC
  channel-chaining model cleared a real stall and was still wrong. The tell
  was that it needed a second special case to keep the first from
  misbehaving. **When a fix needs a qualifier, look for the mechanism it is
  standing in for.**
- **"Not derivable" needs an exhausted search, not a failed one.** The video
  attribute write path was twice declared underivable from firmware — once
  because no surviving software sets an attribute, once because every
  `OUT ($05)` in the corpus was accounted for. Both searches were real and
  both were in the wrong place: the answer was port `$0A`, sitting in a
  screen driver nobody had disassembled. Before calling something
  hardware-only, name the code you have *not* read.
- **A silent drop is worse than a stall.** The single-slot interrupt model
  discarded one device's entire interrupt stream with no diagnostic, for the
  whole of Phase 1. Every model that can refuse to act should say so — which
  is why the exit report asserts 1:1 request/acknowledge counts and why
  unmodelled ports are now counted rather than ignored.
- **Suspect the input before the emulator, but prove it.** Four disk images
  failed for reasons that were not in this code: they are double-stepped
  dumps. The proof was the `.IMD` cylinder maps, which split the nine images
  perfectly along the boot/no-boot line. The inverse also holds — `P25K_G`
  is clean, so its failure *is* ours.
- **A test that cannot fail is worse than no test.** Three times now: a
  stale GUI binary passed the whole front-end section for hours; a pixel
  probe for the screen offset passed with the offset deleted; and that same
  probe went on passing after a third menu was added moved the labels
  underneath the strip it was watching. Negative-test anything that guards a
  behaviour you care about — and where a probe depends on a layout, make it
  assert its own discriminating power, as that one now does.

---

## Phases 1–3 — complete

**Phase 1, a correct floppy path.** The boot sector's own `READ DATA` runs,
the DMA delivers, execution reaches `$4A00`. Getting there meant correcting
three misidentifications provable from the ROM bytes: ports `$10`–`$13` are
a PIO not a CTC, port `$16` is the DMA and was the missing data path, and
`CALL $3019` is a relocated RAM test rather than a disk handoff. The
expected side effect happened — every tuning constant disappeared.

**Phase 2, CP/M running.** `A>` and a correct `DIR`. What it took: a real
IM2 daisy chain (the vendored core's single pending-interrupt slot had been
silently discarding one device's entire stream), the chain order *derived*
from the IPL's own stack discipline, a T-state-driven CTC whose payoff is a
falsifiable check — CBIOS's baud table decodes to 75–2400 baud each within
0.3% — the CTC's real per-channel wiring, and port `$05` as an input.

**Phase 3, an interactive machine.** An SDL3 + ImGui front-end with a
CRTC-driven renderer, live keyboard, a styled menu bar and native file
dialogs. The renderer lives in the core so both front-ends call it and
cannot drift; pacing holds the guest to 50 fields a second against the wall
clock — not against the display's refresh rate, and not "by construction",
which is what an earlier version of this paragraph claimed while the guest
ran at whatever rate the host allowed. The speed setting (T54) scales the
emulation budget per iteration rather than that period. Along the way the video card was fully decoded: 16K × 12 bits,
the attribute nibble latched in port `$0A`, the 8×12 character cell, and the
complete escape-code set — which the P2219 manual then confirmed entry for
entry.

---

## Phase 4 — a *complete* machine (current)

Four things stood between "boots and runs software" and "emulates the
machine". In `TODO.md` order, the first is now finished:

**The debugger panels (P1).** *Done.* The GUI exists because
instrumentation is what has moved this project every single time. Device
state, memory, disassembly and the log are all in, and the machinery behind
them — `core/debug.{c,h}` and `core/log.{c,h}` — is shared with the harness,
so `p2500-emu --state` and `--disasm` print exactly what the panels draw and
the core's diagnostics reach both front-ends through one callback. That was
the deliberate part: a panel no headless run can contradict is a panel that
can quietly start lying.

**Graphics mode (P2).** The character path is complete and the graphics path
is entirely absent. It is fully specified now — 512 × 256 dots, one bit
each, selected by port `$0A` bit 6 — so this is implementation, not
research. It is also the last major part of the video hardware that exists
only on paper.

**Writing to disk (P3).** Reading works; the DMA↔FDC data flow is inverted
for writes and `WRITE DATA` / `FORMAT` are decoded but unimplemented. This
is what stands between the emulator and running `CONFIG`, copying files with
`PIP`, or saving from SuperCalc — and it is a prerequisite for modelling
SESAM properly, since disk initialization *writes*.

**The FDC's drive-ready model (P4).** Two hangs — a diskless boot, and
`P25K_G` after its last successful read — that look like one cause: a real
µPD765 polls each drive's READY and interrupts on changes, and this emulator
has two synthetic post-reset interrupts instead. Whether real hardware also
hangs with an empty drive is genuinely open; the honest move is to model the
line properly rather than invent an interrupt that makes a symptom go away.

## Phase 5 — the media, and what is locked inside it

The nine disk images are the entire surviving software corpus this project
has, and four of them are half dumps. That is now the biggest single
constraint on what can be learned, because those four hold the P2219 system
diskette itself, the `CONFIG` utility, the alternate BIOS profiles and the
national keyboard tables.

**Re-imaging them with single-stepping is the highest-leverage action
available to this project, and it needs hardware rather than code.** About
60% of each file can already be salvaged, which was enough to identify what
is there but not to run any of it.

Everything else here is downstream of that: the UCSD p-System disks (a
genuinely different bootstrap, and the best independent test of the disk
path that could exist), reading `.IMD` directly for bad-sector fidelity, and
`CFTABLES.PHI` for the keyboard tables.

## Phase 6 — research that no longer needs hardware

Two sources opened up recently and are far from exhausted.

**The `.PHI` system files are memory-mapped images**, so any disk's CBIOS and
PBIOS can be disassembled without booting it, and different builds diffed
against each other. The standing note that they were unusable for static
analysis was an artifact of a broken extractor. This is how `P25K_G`'s
failure was narrowed from "an event never signals" to "the disk device's
request never completes", and it is the route to the drive-type tables, the
national keyboard tables and the `$E274` dead-key decode.

**The P2219 manual is OCR'd** and checked in. It has already settled the
attribute encoding, the screen control codes, capitals lock, the drive map
and SESAM initialization — several of which confirmed firmware decodes
independently, and two of which corrected them. The printer interface, the
disk formats, the utility descriptions and the sideways 8-bit code table on
page 27 are still unread.

## Phase 7 — everything else

- **SESAM for real software.** The protocol is modelled; a real key's three
  bytes are missing. The manual reveals more than expected: a system disk is
  *initialized* against a key on first boot, writing to itself, after which
  the wrong key gives `INIT ERROR`. So a genuine key may not boot these
  particular images.
- **An Emscripten build.** Nearly free given SDL3's callback model, and a
  browser-playable P2500 is a disproportionately good outcome for a machine
  with this little surviving software.
- **The FXD/SASI card.** Was out of scope "until a Winchester-configured
  `PBIx.PHI` turns up" — and something that looks like one now has. Salvaged
  fragments of the alternate BIOS profiles show drive-type tables where
  `SYSTEM` and `SYS09` list only `5s`/`5d` while **`SYS12` and `SYS13` list
  `hd`**. Not proof: those files are 61–67% recovered and the tables are
  undecoded. Re-imaging that disk settles it.

**A MAME driver** remains a plausible sibling, but it would reuse the
*findings* and `make test` as an oracle rather than the code — MAME's
devices are C++ classes with their own scheduler, and it already ships
`z80daisy`, `z80ctc`, `z80pio`, `z80dma` and `upd765`. Worth keeping the
core clean for; not worth contorting it for.

---

## What hardware work would help

**Nothing has been blocked on hardware through four phases.** The one
genuine exception is the media: re-imaging the four double-stepped disks
needs a drive and cannot be done from here.

Beyond that, measurement would settle these faster than firmware
archaeology, and **refuting any of them would be more valuable than
confirming it**. Full checklist in
`../Tracing/P2500-predicted-wiring-from-firmware.md`.

1. **Re-image the double-stepped disks.** See Phase 5. Not a measurement,
   but the highest-value physical task by a wide margin.
2. **The video card's dot-clock crystal** — the can at ref `5101`, reported
   to start with "12", never identified. Cheap, and it settles `TODO.md` T29
   by arithmetic: the CRTC is programmed for 311 scanlines of 98 character
   times, so the frame rate is the dot clock over 243,824. If that is 50 Hz,
   CTC channel 2 is the video frame rate rather than mains and the last
   non-derived frequency in the emulator becomes derived.
3. **Where the CTC sits in the IM2 daisy chain**, relative to the FDD card's
   PIO and DMA. The DMA-before-PIO half is settled; this half is not, and it
   decides whether a clock tick can pre-empt a disk handler that released
   itself early with `EI`/`RETI`. Tracing `IEI`/`IEO` between the Z8430,
   Z8420 and Z8410 is a continuity check.
4. **The drive READY line**, which decides whether a real machine also hangs
   with an empty drive (`TODO.md` T43).
5. **FDD card: which µPD765 signals reach PIO port A bits 0 and 1.** The
   emulator has to guess this. A 10-minute continuity check settles it.
6. **FDD card: PIO port B direction** (`$A1` mask → PB0/PB5/PB7 inputs). One
   measurement that validates or kills the entire PIO identification.
7. **Port `$05`'s remaining readable bits.** Bit 7 is RXD and bit 6 a
   transmit handshake, both confirmed from the code that reads them. Bits
   0–5 are unknown and read back as 1.
8. **The four `515xx` decode PROMs' address inputs.** Truth tables already
   dumped but inert without the wiring. Would give the complete memory and
   I/O map in one go.
9. **The Philips P2500 System Reference Manual (`5103 992 30421`).**
   Supersedes most of the above, and the P2219 manual explicitly defers to it
   for high-resolution graphics detail. Still the single biggest
   documentation gap.

Things that would *not* help much: the backplane glue logic, the DIP switch
(all 8 read ON, so there is no bit pattern to correlate), and the video
card's DIN connectors.

---

## Cross-references

- `TODO.md` — the work queue, the decoded hardware model, address references
- `../Information from the internet/P2219-manual-OCR/` — the CP/M manual,
  OCR'd, with `findings.md` for what it settled
- `../ROM Dumps/CPU-Card-Boot-EPROM/disassembly/findings.md` — the IPL
- `../Disk Images/findings/` — image integrity, BIOS disassembly, salvage
- `../UCSD p-System Repair/README.md` — p-System viability
- `../Tracing/P2500-predicted-wiring-from-firmware.md` — what to measure
- `../P2500-System-Specifications.md` — the consolidated spec sheet
