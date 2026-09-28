# Roadmap — P2500 emulator

Where this is going and in what order. `TODO.md` is the work queue with the
evidence behind each item and the decoded hardware model; this file is the
shape of the project and the reasoning about sequencing. Read `TODO.md`
first if you are here to do work.

Rewritten 2026-09-28. The historical "state before Phase 1" section and the
long Phase 1 / Phase 2 plans are gone now that both are complete —
`git log -p --follow ROADMAP.md` has them.

## Goal

Boot an unmodified Philips P2500 / P2000B into CP/M 2.2 from a real disk
image, with a working screen and keyboard, on a hardware model that is
*decoded from firmware* rather than approximated with tuning constants.

## Guiding principle

**Every mechanism in this emulator should trace to a specific byte in a real
dump.** The project's recurring failure mode has been introducing a
plausible-sounding mechanism (a free-running counter, a synthetic `$3019`
handoff, a step-count cooldown), tuning it until something moved, and then
building further work on top of it. Each later turned out to be covering for
a misread port. When a busy-wait will not clear, the answer has so far
*always* been in the ROM, not in a constant.

Five corollaries, each paid for:

- **Address-diversity is not evidence of progress.** Zeroed RAM decodes as
  `NOP` and produces an ever-growing distinct-address count indistinguishable
  from real execution. Landmarks must be validated against the opcode
  actually present at that address.
- **"It got further" is not evidence the mechanism is right.** The CTC
  channel-chaining model cleared a real stall and was still wrong. The tell
  was that it needed a second special case (`channel3_rx_ready`) to keep the
  first from misbehaving. **When a fix needs a qualifier, look for the
  mechanism it is standing in for before building on it.**
- **Check how far "derived from firmware" actually reaches before declaring
  something unmeasurable.** The IM2 daisy-chain order looked like pure wiring
  that only a multimeter could settle. It is not: the IPL's DMA and PIO
  handlers share one saved-SP word and one private stack, so only one of the
  two orders lets them coexist, and the wrong one kills the machine in an
  `RST 38` loop. Two of the three "hardware only" questions this project
  raised turned out to be answerable from bytes already in the repository.
- **A silent drop is worse than a stall.** The single-slot interrupt model
  discarded one device's entire interrupt stream with no diagnostic, and did
  so for the whole of Phase 1. Every model that can refuse to act should say
  so when it does — which is why the exit report now asserts 1:1
  request/acknowledge counts.
- **"Not derivable" needs an exhausted search, not a failed one.** The
  video attribute write path was twice declared underivable from firmware -
  once on the grounds that no surviving software sets an attribute, once on
  the grounds that every `OUT ($05)` in the corpus was accounted for. Both
  searches were real and both were in the wrong place: the answer was port
  `$0A` and it was sitting in a screen driver that had never been
  disassembled. Before calling something hardware-only, name the code you
  have *not* read.
- **Suspect the input before the emulator, but prove it.** Four disk images
  failed to boot for reasons that were not in this code at all: they are
  double-stepped dumps missing every other track. The proof was not a hunch
  but the `.IMD` cylinder maps, which split the nine images perfectly along
  the boot/no-boot line. The inverse also holds — `P25K_G` is clean, so its
  failure *is* ours.

---

## Phase 1 — a correct floppy path — COMPLETE

The boot sector's own `LD DE,$1030 / CALL $0003` produces a real `READ
DATA`, the DMA delivers 32 sectors to `$1000`–`$2FFF`, and execution reaches
`$4A00` un-gated — a hard, falsifiable criterion, since `$4A00` is where the
real boot sector jumps after its own `LDIR` and nothing reaches it by
accident.

Getting there meant correcting three misidentifications, all provable from
the ROM bytes: ports `$10`–`$13` are a Z80A-PIO and not a CTC; port `$16` is
the Z80A-DMA and was the missing `READ DATA` data path; and `CALL $3019` is
the ROM's own relocated RAM test, not a disk handoff. Four further
compounding bugs sat between "wired up" and "actually runs", none of them in
the wiring itself. The expected side effect happened: every tuning constant
in the codebase disappeared.

## Phase 2 — CP/M actually running — COMPLETE (2026-09-27)

`make test` renders the `A>` prompt and `DIR` lists the disk's real
contents. Those six files are what the disk image holds, so the listing is
fixed by the media rather than by anything in this emulator.

What it took, over and above Phase 1:

- **A real IM2 daisy chain.** The vendored core's single pending-interrupt
  slot had been silently discarding one device's entire interrupt stream: the
  IPL's DMA end-of-block handler `$07F3` had never executed in any run of
  this emulator. It now runs 87 times, once per transfer, every request
  acknowledged 1:1. This alone cleared the deadlock that had looked like a
  clock problem — it was the *disk* completion that never arrived.
- **The chain order, derived rather than guessed** (guiding principle,
  corollary 3).
- **A T-state-driven CTC with a real prescaler.** The ×4 fudge is gone, and
  the payoff is a falsifiable check: CBIOS's table at `$F546` now decodes to
  75/110/150/300/600/1200/2400 baud, each within 0.3%, which it cannot do
  under a wrong prescaler or a per-instruction tick.
- **The CTC's actual per-channel wiring.** ch0 is the serial transmit bit
  clock (TIMER, no CLK/TRG at all); ch1's CLK/TRG is the serial RXD line;
  ch2's is an unidentified 50 Hz strobe feeding the real-time clock; ch3's is
  the keyboard's byte-ready strobe.
- **Port `$05` as an input** — bit 7 RXD, bit 6 transmit handshake.
- **`tools/disasm_ram.sh`**, which is why any CBIOS finding here is
  checkable at all. The `.phi` files are sector-interleaved on disk, so only
  a `--dump-ram` image yields usable addresses.

---

## Phase 3 — an interactive machine (current)

**Mostly landed 2026-09-28.** `p2500-gui` boots a disk to the `A>` prompt in
an SDL3 window and takes live keyboard input. Geometry, cursor position and
cursor shape all come from the MC6845's registers; the renderer lives in the
core (`p2500_video_render`) and both front-ends call it, so they cannot drift
apart. Pacing is one video field of emulation per presented frame, which
makes the frame loop and the guest's own 50 Hz clock strobe the same event by
construction. `libp2500.a` is still dependency-free and `make test` still
needs no display — the front-end is tested under `SDL_VIDEODRIVER=dummy`.

What is left in `TODO.md` P1: **T39, the debugger panels**, which are the
actual reason for wanting a GUI, and **T34** (a log callback and a real
reset) which the GUI works without today but a log panel and a reset button
will need. T38's live input works; what is deferred there is accented-key
decoding, which needs the `$E274` dead-key table read first rather than
guessed.

Note that T27 turned out not to gate this after all. The attribute plane is
modelled and composited, and since nothing that survives ever sets an
attribute, the renderer was unblocked by *answering* the layout question
rather than by resolving the write path.

**The GUI's primary purpose is a debugger, not a settings dialog.** Every
advance in this project came from instrumentation — `--peek`, `--count`,
`--watch`, `--dump-ram`, trace windows. A live view of the daisy chain and
the CTC channels would have turned the two hardest bugs in this project's
history from archaeology into inspection. Build the panels first and the file
dialogs last.

Three console questions are already settled and need no further research:

- **Video is settled, except for one wire.** The CRTC registers decode to
  80×24 at 12 scanlines/row, matching the official P2219 manual's 640×288
  mode, and the card's memory is 16 K words × **12 bits** — an 8-bit
  character code plus a 4-bit attribute nibble (underline, reverse, flash,
  low intensity). Four independent facts agree on that; see `TODO.md` T27.
  The glyph is the full 8×12 cell, not 8×8 (T27a). What is *not* known is
  how the CPU reaches the attribute nibble, and it is not derivable —
  **nothing in the entire disk corpus ever sets an attribute.** The emulator
  models the plane, declines to guess the selector, and now raises a
  diagnostic on any bank value it cannot account for, so the day something
  does set one it announces itself.
- **Keyboard**: there are **two** input paths, not one, which is what made
  this confusing for so long. The keyboard is port `$06`, byte-wide,
  announced by a strobe on CTC channel 3. The serial port is the bit-banged
  one, on channels 0 and 1 — transmit on port `$04` clocked by ch0, receive
  by sampling port `$05` bit 7 once per bit cell under ch1, whose CLK/TRG is
  the RXD line itself so the start bit triggers the sequence. Typing works
  end to end and `make test` asserts it.
- **Printer**: port `$04`, transmit-only, out DB25 pin 3, and the rate is no
  longer a guess — CBIOS's own baud table at `$F546`, selected by `$F727`,
  which is 1200 baud on this disk. Everything is modelled and nothing
  exercises it, so `PIP LST:=FILE.TXT` is both the remaining work and its own
  test.

## Phase 4 — writing, and the disks that still fail

Parallel to Phase 3 and independent of it.

- **Write and format** (`TODO.md` T30). Reading works; `WRITE DATA` and
  `FORMAT A TRACK` are decoded but unimplemented, and the DMA↔FDC data flow
  is still inverted, which is what blocks them. `PIP` copying a file onto a
  disk is the test.
- **Multi-track reads past `EOT`** (`TODO.md` T31). CP/M's directory reads
  stay inside one track, which is why `DIR` is correct without this; a large
  sequential file read is not.
- **`P25K_G`** (`TODO.md` T42) — the one complete, clean image that does not
  reach a prompt, so this failure belongs to this emulator rather than to the
  dump. Characterised down to a single unsignalled event slot; the next step
  is to identify which ISR is supposed to signal it.
- **The four double-stepped images are blocked on re-imaging**, not on code.
  They were read by a 96 TPI drive single-stepping across 48 TPI media, so
  every other track is simply absent. `tools/imd_tool.py verify` reports this
  for any `.IMD`.

## Phase 5 — everything else

Not on the critical path; listed so it is not forgotten.

- **UCSD p-System boot** (`P2k5_LOGIC`, `P2k5_TKS`) — a genuinely different
  bootstrap and therefore a real independent test of the disk path, and the
  first thing that would test the IM2 chain against software that was never
  considered while building it. Blocked until those disks are re-imaged.
- **SESAM dongle emulation for real software.** The protocol is modelled;
  what is missing is a real key's 3 bytes. Any P2219 disk that refuses to
  boot with `INIT ERROR` is testing this.
- **An Emscripten build** (`TODO.md` T40), which falls out nearly free if
  T36 uses SDL3's callback app model from the start. A browser-playable P2500
  is a disproportionately good outcome for a machine with this little
  surviving software.
- **The FXD/SASI card.** Was out of scope "until a Winchester-configured
  `PBIx.PHI` turns up" — and something that looks like one now has
  (`TODO.md` T45). Salvaged fragments of `p25k_prg`'s alternate BIOS
  profiles show drive-type tables where `SYSTEM` and `SYS09` list only
  `5s`/`5d` while **`SYS12` and `SYS13` list `hd`**. Not proof: those files
  are 61–67% recovered and the tables are undecoded. Re-imaging that disk
  is the cheap way to settle it.
- **The `$18`–`$1E` port cluster**, which appears in `SYS09.PHI` and is most
  likely the optional 8" drive interface. Ignore it unless a disk image needs
  it.

**A MAME driver** is a plausible eventual sibling but would reuse the
*findings* and `make test` as an oracle rather than the code — MAME's devices
are C++ classes deriving from `device_t`, driven by its own scheduler and
address maps, and it already ships `z80daisy`, `z80ctc`, `z80pio`, `z80dma`
and `upd765`. So the transferable assets are the decoded hardware model
(documentation, not code) and the test suite as an equivalence check: a MAME
driver that boots to `A>` and lists the same six files is demonstrably
equivalent. Worth keeping the core clean for; not worth contorting it for.

---

## What hardware work would help, and what would not

**Nothing has been blocked on hardware through either completed phase.** The
Phase 2 review's claim that it had found this project's first un-derivable
questions did not survive contact with the work: two of its three were
answerable from bytes already here — the daisy-chain order, from the IPL's
own stack discipline, and what the CTC's CLK/TRG pins are, from reading
CBIOS's four ISRs.

So the list below is shorter than expected. Measurement would still settle
these faster than firmware archaeology, and **refuting any of them would be
more valuable than confirming it.** Full checklist in
`../Tracing/P2500-predicted-wiring-from-firmware.md`.

1. **Where the CTC sits in the IM2 daisy chain**, relative to the FDD card's
   PIO and DMA. The DMA-before-PIO half is settled; this half is not, and it
   decides whether a 50 Hz clock tick can pre-empt a disk handler that
   released itself early with `EI`/`RETI`. Tracing `IEI`/`IEO` between the
   Z8430, Z8420 and Z8410 is a continuity check. The emulator currently puts
   the CTC last, which is the conservative reading.
2. **The video card's dot-clock crystal** — the can-shaped part at ref
   `5101`, never identified. Cheap to read and it settles `TODO.md` T29 by
   arithmetic: the CRTC is programmed for 311 scanlines of 98 character
   times, so the frame rate is the dot clock over 243,824. If that comes out
   at 50 Hz, CTC channel 2 is the video frame rate rather than mains, and
   the last non-derived frequency in the emulator becomes derived.

3. **What pulses CTC channel 2's CLK/TRG** — the 50 Hz real-time clock tick.
   Mains-derived or the video card's frame rate; both are 50 Hz so the
   emulator is right either way. See the item above for the cheaper route.
4. **Which nibble bit is which attribute.** The attribute *path* is solved
   (`TODO.md` T27): the CPU latches a 4-bit nibble in port `$0A` and the card
   stores it beside the next character. What is not pinned is which bit means
   underline, reverse, flash or low intensity. Firmware may still settle it -
   the `ESC S/T/U/V` handlers and `$F30C` are unread - so this is a
   *measurement of last resort*, not a first resort.

   Worth recording how the previous version of this item read: "unlike every
   other open question in this project it is *provably* not answerable from
   firmware". That was wrong, and wrong in the project's characteristic way -
   it concluded "not derivable" from a failed search rather than from an
   exhausted one. The answer was in a driver nobody had disassembled yet.
5. **FDD card: which µPD765 signals reach PIO port A bits 0 and 1.** The
   emulator has to guess this. A 10-minute continuity check settles it.
6. **FDD card: PIO port B direction** (`$A1` mask → PB0/PB5/PB7 inputs). One
   measurement that validates or kills the entire PIO identification.
7. **Port `$05`'s remaining readable bits.** Bit 7 is RXD and bit 6 is a
   transmit handshake, both confirmed from the code that reads them. Bits 0–5
   are unknown and currently read back as 1. This is a live polled input now,
   not a write-only latch.
8. **CPU card: the port `$05` latch and what its outputs gate.** Turns a
   reasoned guess into a fact, and is needed properly for the video-RAM
   window.
9. **CPU card: the four `515xx` decode PROMs' address inputs.** Their truth
   tables are already dumped but inert without the wiring. Would give the
   complete memory and I/O map in one go.
10. **The Philips P2500 System Reference Manual (`5103 992 30421`).**
   Supersedes all of the above. Still the single biggest documentation gap in
   the whole project.

Things that would *not* help much right now: the SASI/FXD card, the
backplane's glue logic, the DIP switch (all 8 read ON, so there is no bit
pattern to correlate), and the video card's DIN connectors. All matter
eventually; none unblocks anything.

---

## Cross-references

- `TODO.md` — the work queue, the decoded hardware model, address references
- `../ROM Dumps/CPU-Card-Boot-EPROM/disassembly/findings.md` — the IPL
- `../Disk Images/findings/BIOS-disassembly-findings.md` — the CP/M BIOS
- `../Disk Images/findings/IMD-integrity-manifest.txt` — per-image dump
  integrity, from `tools/imd_tool.py verify`
- `../Tracing/P2500-predicted-wiring-from-firmware.md` — what to measure
- `../P2500-System-Specifications.md` — the consolidated spec sheet
- MC6845 register semantics: <https://book.martypc.net/display-graphics/6845>
  (see `TODO.md`'s source assessment for what a 6845 reference can and
  cannot settle for this board)
