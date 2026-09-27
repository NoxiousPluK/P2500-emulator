# TODO — P2500 emulator

Written for a fresh session with no prior context. Read this **before**
`README.md`: large parts of that file describe a hardware model that has
since been shown to be wrong, and following its "next steps" will waste
time. `ROADMAP.md` has the longer-term shape; this file is the work queue.

Everything below is derived from evidence already in this repository — the
boot IPL EPROM dump, the CP/M system files, the disk images, and live
traces of this emulator. Nothing in the queue is *blocked* on new hardware
measurements.

**Status: CP/M 2.2 boots to the `A>` prompt and accepts typed commands.**
`make test` proves it: the SESAM banner is still byte-exact, the IPL still
reaches `$4A00`, CP/M's page zero and `$E200` jump table are real, the
screen reads `58K CP/M Ver. 2.2`, and typing `DIR` lists exactly the six
files the disk image contains. Reproduce the last one with:

```
./p2500-emu --disk "../Disk Images/extracted/P25K_B/P25K_B.raw" \
            --max-steps 20000000 --type 'dir\r' --dump-vram /tmp/vram.bin
```

**If you are picking up work now, start at the P4 section.** T1–T12 (P0/P1)
are complete; T13–T16 (P2) are IPL-era cleanup; T17–T21, T24 and T26 (P3)
are done and their evidence is kept below because it is the reasoning the
current model rests on. P4 is what is left. The "Background" section
immediately below is the original IPL decode and is still accurate.

---

## Background: three misidentifications that block everything else

The emulator's I/O model is wrong in three load-bearing ways. Each is
provable from the ROM bytes. Fixing them replaces every tuning knob in the
codebase with decoded protocol.

### 1. Ports `$10`–`$13` are a Z80A-PIO, not a Z80-CTC

`src/ctc.c` models `$12`/`$13` as Z80-CTC channels 0 and 1. They are the
control registers of the FDD card's **Z80A-PIO** (Z8420A), and `$10`/`$11`
are its data registers.

Proof — the init sequence at ROM `$045A`–`$0473`:

| ROM | Write | Z80-PIO meaning |
|---|---|---|
| `$045A` | `OUT ($12),$FF` | Port A **mode 3** (bit control) → I/O-select mask follows |
| `$045E` | `OUT ($12),$03` | I/O mask: **bits 0,1 inputs**, bits 2–7 outputs |
| `$0463` | `OUT ($12),($FEA0)` | **Interrupt vector** (D0 must be 0; `($FEA0)` is `$00` at runtime) |
| `$0467` | `OUT ($12),$37` | Interrupt control word (low nibble 7): active **high**, **OR** logic, mask follows |
| `$046B` | `OUT ($12),$FC` | Mask: **monitor bits 0 and 1 only** |
| `$046F` | `OUT ($13),$FF` | Port B mode 3 |
| `$0473` | `OUT ($13),$A1` | Port B I/O mask: **PB0, PB5, PB7 inputs**, PB1–PB4, PB6 outputs |

Corroboration, all independent:

- `LD A,$F3 / DI / OUT ($12),A … EI` and `LD A,$73 / DI / OUT ($12),A /
  EI` occur ~10× through the driver (`$074B`, `$076D`, `$078F`, `$07A3`,
  `$07C3`, `$07E2`, `$0829`, `$087C`, `$089D`, `$08CD`, `$08E3`). Low
  nibble 3 is the PIO **interrupt disable/enable word**: `$F3` = enable,
  `$73` = disable. This is the driver masking its own interrupt around
  critical sections. As CTC control words these would be meaningless
  "reset, no time constant" writes.
- `$10` and `$11` are both written *and read* (`IN A,($10) / RRA` at
  `$0832`; `IN A,($11) / AND $80` at `$054C`). CTC channels are
  write-only for control.
- `$11` receives `$42`/`$46`/`$52`/`$56` — data bits (drive/head/mode
  select), not control words. As CTC they would all be D0-clear "vector"
  writes on a non-zero channel, which real silicon ignores.
- The community diagnostic tool **HWTEST V100** puts the CTC at ports
  `$00`–`$03`, and the IPL never touches those ports. Both CTC claims in
  this project's docs reconcile cleanly: the CTC exists, lives at
  `$00`–`$03`, and is **unused by the disk boot path**.
- `README.md`'s unexplained "channel 1 inherits vector `$02` and nothing
  ever patches its handler" is an artifact. PIOs do not inherit vectors.
  Vector `$02` is the **Z80-DMA's** (see below).

**Consequence:** `P2500_CTC_TSTATES_PER_PULSE`, the free-running counter,
the deliberate deviation from software-reset semantics, and the
`resolve_im2_target` no-op suppression are all scaffolding around a chip
that is not on this bus. Under a PIO model the interrupt source is the
µPD765's `INT` line wired into PIO port A bit 0 or 1, and the ROM tells
you exactly when it is allowed to fire (`$F3`) and when it is not (`$73`).

### 2. Port `$16` is the Z80A-DMA — and it is the missing `READ DATA` data path

`src/fdc.h` says `dest_ram` is "set by machine.c right before issuing a
read". It is never set anywhere in the tree. `READ DATA` therefore cannot
deliver a byte, which is why no run has ever produced a useful one.

The project docs call `$16` a stepper-motor pulse table / drive latch. It
is the DMA. `sub_0b60h` copies a 22-byte template from ROM `$0B74` to RAM
`$FEB3`; `sub_0bb4h` patches three fields; `sub_0b89h`
(`LD B,(HL) / INC HL / LD C,$16 / OTIR`) streams the 20-byte block to port
`$16`:

```
14 | 0D 15 CF 69 15 [LL LL] 2C A3 10 A3 9D [AA AA] 12 [VV] 8A CF AB 87
^count            ^blocklen                  ^PortB addr  ^vector
```

| Byte | Register | Meaning |
|---|---|---|
| `$0D`, `$69` | WR0 | Transfer; Port A starting address low = **`$15`** (the µPD765 data register); block length follows |
| `$2C` | WR1 | **Port A is I/O, fixed address** |
| `$10` | WR2 | **Port B is memory, incrementing** |
| `$9D` | WR4 | Byte mode; Port B address + interrupt control byte follow |
| `$12` | — | Interrupt control byte (interrupt at end of block) |
| `$8A` | WR5 | `/CE`//`WAIT` multiplexed, `/RDY` active low |
| `$CF $AB $87` | WR6 | **Load / Enable Interrupts / Enable DMA** — the canonical Z80-DMA startup trailer |

Elsewhere: `sub_0b4fh` sends `$83`×6 (**Disable DMA**); CP/M's
`SYSPBI.PHI` sends `$C3`×6 (**Reset**) and `$AF` (**Disable DMA
interrupts**) to the same port. All valid Z80-DMA WR6 commands.

Runtime patches (`sub_0bb4h`):

| Target | Source | Meaning |
|---|---|---|
| `$FEB9`/`$FEBA` (stream offset 5–6) | computed count, **`DEC HL`** first | Block length — Z80-DMA programs it as **count − 1** |
| `$FEC0`/`$FEC1` (stream offset 12–13) | `($FE9C)` | **Port B address = the destination/source RAM buffer** |
| `$FEC3` (stream offset 15) | `($FEA1)` | DMA interrupt vector, set at ROM `$0455` to `($FEA0) + 2` |

Byte count (`sub_0b96h`): `($FE9A) << (7 + (($FE89) & 7))`, i.e.
**sectors × (128 << N)**.

Three call-site variants, dispatched from ROM `$05C8`–`$05E0` on the
µPD765 opcode stored in `$FEDD`:

| Opcode in `$FEDD` | µPD765 command | DMA setup routine | Notes |
|---|---|---|---|
| `$06` | READ DATA | `sub_0b32h` (`$0B32`) | Patches stream offsets 2,3 from `$CF,$69` to `$11,$6D` — the direction change |
| `$05` | WRITE DATA | `sub_0b43h` (`$0B43`) | Template used unmodified |
| `$0D` | FORMAT A TRACK | `$0B19` | Block length = `($FE88) × 4` — 4 bytes (C,H,R,N) per sector, exactly as the µPD765 FORMAT command expects. Confirms `$FE88` = sectors per track |

**Consequence:** `dest_ram` should be `($FE9C)`; the length is the
computed byte count; and `$FE88`/`$FE89`/`$FE9A`/`$FE9C` are all readable
from emulated RAM at the moment the DMA is programmed.

### 3. `CALL $3019` is a RAM test, not a disk handoff

This invalidates the harness's central mechanism.

The 37-byte block at ROM `$014F` is copied to `$3000` (`$011D`–`$0126`).
`$3000 + $19` maps back to source address `$0168` — the RAM
write-then-verify primitive (`LD A,D / OR E / RET Z / XOR A / LD (HL),A /
LD A,(HL) / OR A / RET NZ / INC HL / DEC DE / JR $0168`). With the boot
EPROM banked out by `OUT ($05),$0F`, the call `HL=$0000 / DE=$1100 /
CALL $3019` tests **`$0000`–`$10FF`** — the 4.25 KB hidden behind the
EPROM. The earlier `CALL $0168` at `$0116` (`HL=$1100`, `DE=$EF00`) tests
`$1100`–`$FFFF`. Together: exactly 64 KB, no gap, no overlap.

So `main.c`'s `$3019` intercept invents a disk read the ROM never
performs, and the `$1100` sector refresh compounds it. Reproduce the
damage in the current build:

```
$ ./p2500-emu --rom roms/ipl.bin        # no disk, so the intercept is inactive
RAM $0000-$000F: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

The ROM is gone — the RAM test erased it, because port `$05` bank
switching is not modeled. The same run still reports `$1100`/`$1103`/
`$1106` landmark hits with **no disk attached**: that is the NOP-sled
through zeroed RAM the README warns about, appearing inside the claimed
landmark evidence.

**Where the boot sector actually goes: `$1000`, not `$1100`.**
`sub_0333h` does `JP $1000` when the `$FE45` request returns status `$00`.
Sector 0 of `Disk Images/extracted/P25K_B/P25K_B.raw` begins:

```
00 00 FB 11 30 10 CD 03 00 21 80 10 11 00 34 01 80 1F ED B0 C3 00 4A
```
= `NOP / NOP / EI / LD DE,$1030 / CALL $0003 / LD HL,$1080 / LD DE,$3400 /
LD BC,$1F80 / LDIR / JP $4A00`.

`JP $1000` lands on two harmless NOPs, and `$1030` is **offset `$30`
inside the sector itself**, holding `04 00 00 00 01 00 10 00 00 20` — a
real request descriptor. At `$1100` none of that lines up, which is the
only reason `--poke` had to synthesize a descriptor at `$1030`.

**The descriptor's command ID is `$04`.** Per the `$0422` dispatch table
(9 entries, 3-byte records at `$0423`), ID 4 → **`$059C`**, which sets
`$FEDD = $06` = µPD765 READ DATA. So the real read handler is `$059C`,
not `$04D5` as `main.c`'s landmark list assumes. Full table:

| ID | Handler | ID | Handler | ID | Handler |
|---|---|---|---|---|---|
| 0 | `$043E` | 3 | `$0539` | 6 | `$056B` |
| 1 | `$04D5` | 4 | **`$059C`** (READ DATA) | 7 | `$057D` |
| 2 | `$04F9` | 5 | `$05A1` (WRITE DATA) | 8 | `$05A6` (FORMAT) |

---

## Work queue

### P0 — required before anything else means anything

- [x] **T1. Model port `$05` bank switching.** Three values are used:
  `$07` = normal (EPROM at `$0000`–`$0FFF`); `$0F` = EPROM out, RAM at
  `$0000`–`$0FFF`; `$00` = video RAM mapped at `$8000`–`$BFFF`. Keep the
  EPROM image in a separate 4 KB array from the 64 KB RAM and select on
  read; writes to `$0000`–`$0FFF` always land in RAM. Without this, either
  the RAM test wipes the ROM (current behaviour) or the boot path is
  falsified by the `$3019` intercept.
  *Extra:* the `$00` case only matters once you care about the video
  card's separate DRAM bank; a flat `$8000`–`$BFFF` is fine for now, but
  leave the hook in. See `../Tracing/P2500-predicted-wiring-from-firmware.md`
  §C1–C4 for what the real latch is believed to be.

- [x] **T2. Delete the `$3019` intercept and the `$1100` refresh** from
  `src/main.c`. With T1 done, the ROM's own RAM test runs correctly and
  returns `Z` on success, which is what `$015C`–`$0165` expects.
  *Extra:* expect the step count to the same point to rise — the test now
  actually walks `$0000`–`$10FF`. That is correct behaviour, not a
  regression.

- [x] **T3. Replace `src/ctc.{c,h}` with `src/pio.{c,h}`** (ports
  `$10`–`$13`). Needs: per-port mode register, I/O-select mask, interrupt
  vector, interrupt control word (enable / AND-OR / active level / mask),
  the mask byte, input and output data latches, and the "interrupt
  disable/enable word" short form (low nibble 3) that the driver uses
  constantly. Raise an IM2 interrupt with the port's own vector when a
  monitored input bit matches the configured condition **and** the port's
  interrupt enable is set.
  *Extra:* the state machine after a mode-3 word is "next byte is the I/O
  mask"; after an interrupt control word with D4 set it is "next byte is
  the monitor mask". Getting those two follow-byte states right is most of
  the work. Delete `P2500_CTC_TSTATES_PER_PULSE` and every comment
  justifying it.

- [x] **T4. Route the µPD765 `INT` into PIO port A bit 0.** Replace
  `fdc_interrupt_trampoline`'s direct `z80_gen_int()` with "assert PA0";
  the PIO decides whether that becomes a CPU interrupt.
  *Extra:* PA0 vs PA1 is not yet confirmed by hardware — see
  `../Tracing/P2500-predicted-wiring-from-firmware.md` §P1/P2. Make it a
  single named constant so it can be flipped in one place when the FDD
  card gets traced. If PA0 fails to produce sane behaviour, try PA1 before
  concluding the model is wrong.

- [x] **T5. Add `src/dma.{c,h}` at port `$16`.** Parse the register-write
  stream (WR0–WR6 with their follow-byte rules) and the WR6 command bytes
  (`$C3` reset, `$CF` load, `$AB` enable interrupts, `$87` enable DMA,
  `$83` disable DMA, `$AF` disable DMA interrupts). On "enable DMA" with
  Port A = I/O fixed at `$15` and Port B = memory incrementing, perform
  the block transfer between the FDC's byte stream and RAM, then raise the
  end-of-block interrupt on the DMA's own vector.
  *Extra:* decode the WR0 bit layout against the Zilog Z8410 datasheet
  rather than trusting the table above — the read/write direction bit is
  the one that differs between `$69` (WRITE) and `$6D` (READ), i.e. bit 2,
  but the rest of WR0's "which bytes follow" bits should be read from the
  datasheet properly. This is the one place in this document where the
  decode is partly inferred rather than fully pinned.

- [x] **T6. Wire `READ DATA` to the DMA.** Done: `fdc.c`'s `do_read_data`
  now calls `p2500_dma_deliver`, which writes into RAM at the DMA's own
  programmed Port B address for its own programmed block length; `dest_ram`
  is gone. **Confirmed reached in practice** — see ISSUE-1 through
  ISSUE-4 below for the full chase (several compounding bugs sat between
  this being wired up and it actually running): a live `--disk` run now
  issues real `RECALIBRATE`/`READ DATA` commands, the DMA delivers a real
  4096-byte transfer, and execution reaches **`$4A00`** — the phase's own
  success criterion — for real, un-gated. Phase 1 is done; the boot goes
  on to load and jump into CP/M's own CBIOS.

- [x] **T7. Load the boot sector at `$1000`, not `$1100`,** and remove the
  `--poke` workaround for the `$1030` descriptor (keep the flag itself —
  it is generally useful). Update `main.c`'s landmark table: `$1000`
  (sector entry), `$1002` (`EI`), `$1003` (`LD DE,$1030`), `$1006`
  (`CALL $0003`), `$059C` (real READ DATA handler), `$4A00` (final
  handoff). Drop `$1100`/`$1103`/`$1106` and demote `$04D5`.

### P1 — correctness of the harness itself

- [x] **T8. Gate landmark hits on the opcode at PC.** Done: each landmark
  in `main.c` now carries an expected byte — read straight out of the
  loaded EPROM at startup for ROM-resident addresses, hardcoded from the
  known sector-0 bytes (`00 00 FB 11 30 10 CD 03 00 ...`) for `$1000`-
  `$1006`, and falling back to "must be nonzero" for addresses whose real
  content isn't known yet (`$4A00`, `$FEC9`). A hit whose byte doesn't
  match is counted separately (`gated_count`) and reported at exit instead
  of silently inflating `hit_count`. Verified against a real `--disk` run:
  the previously-firing landmarks (`$0000`, `$0100`, `$03E3`, `$043E`,
  `$06C6`) still fire and no gated (filtered) hits occurred — and this run
  also surfaced a new, useful data point: `$1000` (sector-0 entry) is
  never actually reached as PC at all in this run, refining ISSUE-1 below
  (drive-prep code at `$043E` is reached some other way, not via the
  documented `JP $1000` path — worth tracing next).

- [x] **T9. Model ports `$10`/`$11` reads properly.** Done two ways: data
  register reads already combined `output_latch`/`input_latch` by
  `io_mask` (`pio.c`); what was still missing is that the FDC's `INT` line
  was only pulsed true-then-false within a single callback, so polling
  code (e.g. `IN A,($10)/RRA` at `$0832`) could never observe it held.
  `P2500Fdc` now has a real `int_line` level, set on command completion
  and cleared once the host reads through a result phase; `p2500_step()`
  samples it into the PIO input bit every instruction, so both
  edge-triggered PIO delivery and direct polling see the same level a
  real driver would.

- [x] **T10. Give the µPD765 a real execution phase.** Partial: `CB`
  (bit 4) now reads 1 for the whole command+execution+result phase
  (`fdc->phase != IDLE`), visible to a driver polling `$14` mid-command;
  `DIO` and the exact `$80` idle value were already correct. `EXM`
  (bit 5) is **not modeled** — `do_read_data` hands its whole block to the
  DMA in one `memcpy` (see T6/`dma.c`) rather than byte-at-a-time through
  port `$15`, so there is no CPU-visible window where `EXM` would ever
  read 1 in this model. Modeling that for real would mean spreading
  command execution across multiple `p2500_step()` calls — deferred as a
  P2-level realism improvement, not needed for `$4A00`.

- [x] **T11. Remove `resolve_im2_target`'s heuristics.** Done: the
  `$03DB`/`0xFF`/`0x00` suppression is gone now that T3/T4 replaced the
  phantom CTC with real, edge-triggered PIO/DMA interrupt sources. Only
  the `I != $FE` table-page guard remains.

- [x] **T12. Re-validate the SESAM regression baseline.** Still green
  byte-for-byte after T8-T11 (`--sesam roms/sesam_banner_test.bin
  --dump-vram`, diffed against
  `../ROM Dumps/CPU-Card-Boot-EPROM/emulation/vram_after_banner.bin`).

### P2 — after READ DATA works

- [ ] **T13. Rewrite `README.md`.** It currently narrates a long
  investigation into a chip that is not on the bus, and a fresh reader
  will follow its conclusions. Keep the build/run instructions, the SESAM
  validation section, and the "why this exists" credits; replace the
  hardware-model sections with the decode in this file.

- [ ] **T14. Read `$0422` handlers 2, 3, 6, 7** (`$04F9`, `$0539`,
  `$056B`, `$057D`) — the only dispatch IDs still unidentified.

- [ ] **T15. Model `$0A`** (diagnostic/POST latch — written at `$013C` and
  `$01C3`, never read) and confirm whether anything reads port `$05`
  (`SYSPBI.PHI` does, the IPL does not).

- [x] **T16. Add a CTC at `$00`–`$03`.** Done: `src/ctc.{c,h}`, a real
  Z80A-CTC model (4 independently-programmed channels, one per port -
  `channel = port & 3`, the standard Z80 CS0/CS1-from-A0/A1 wiring, not
  separately confirmed by continuity on this board but the only sane
  option). Control-word bits, the shared interrupt-vector register
  (channel-identifier bits auto-inserted per real hardware), and TIMER
  mode's down-counter/auto-reload/ZC-TO-interrupt, and (see below)
  channel-to-channel ZC/TO->CLK/TRG chaining, are modeled directly off
  "Zilog Z80 Family CPU Peripherals User Manual" (the same manual that
  resolved the DMA's real protocol - see ISSUE-2), not inferred blind
  from ROM behavior. Verified live: the repeated unhandled `OUT ($00)`-
  `OUT ($03)` that was blocking CBIOS are completely gone.

  **Investigated the resulting `$E46C` stall thoroughly before concluding
  anything.** Using a new `--peek ADDR:LEN` flag (main.c) to inspect
  memory this project had no static disassembly for:
  - `$E200` is CP/M's standard BIOS jump table, byte-for-byte: index 2
    (`$E206`) = `JP $E46C` = `CONST` (console status), index 3 (`$E209`)
    = `JP $E48C` = `CONIN` (console input) - the whole table's layout
    (BOOT/WBOOT/CONST/CONIN/CONOUT/LIST/PUNCH/READER) matches the
    standard CP/M 2.2 vector exactly. `$CC00`+ holds live CCP data
    including its embedded `"COPYRIGHT (C) 1979, DIGITAL RESEARCH"`
    sign-on string - this is genuinely the CCP, loaded and running.
  - Checked (and ruled out) two specific alternative explanations before
    settling on "needs real input": (1) `CONOUT` (`$E4C3`) is never
    called anywhere in the run (checked with a full, unsampled
    instruction trace over all 1.3M steps) - so this isn't a stalled
    banner-print waiting behind a rendering gap, it's a genuine
    *pre*-banner gate. (2) CBIOS does perform a real SESAM check right
    beforehand (`$E448`: `OUTI` sends `$03` to port `$0F`, `INIR` reads 3
    bytes back, matching the IPL's own dongle protocol exactly) - but it
    runs to completion and returns normally ("not present", a flag set
    to 1), it doesn't gate whether `CONST`/`CONIN` get called.
  - The `CONST`/`CONIN` caller (`$D4FB`) is a small primitive:
    read-and-clear a flag at `$D70E`, return immediately if it was
    already nonzero, otherwise block on `CONIN`.

  > **Superseded in part — read T19 before acting on the rest of this
  > entry.** The CTC channel chaining described below does not exist on the
  > hardware; T19 has the real per-channel wiring and the evidence for it. The `channel 0 ZC/TO -> channels 1-3 CLK/TRG` chain below,
  > and the `channel3_rx_ready` patch that follows it, were arrived at by
  > trying a mechanism and keeping what moved - the exact pattern
  > `ROADMAP.md`'s guiding principle warns against. A later review found
  > direct evidence against the chain (channel 2 counts the opposite
  > CLK/TRG edge from channels 1 and 3) and, more usefully, found the
  > CBIOS handlers that identify all three channels. The narrative below
  > is kept because the *symptoms* it records are real and reproducible;
  > its hardware conclusion is not.

  **Root cause, confirmed by fixing it**: that flag is set by a
  console-receive interrupt that depends on CTC channels 1-3 (all
  COUNTER mode, `tc=1`, interrupts enabled) ticking - and they never did,
  because `p2500_ctc_tick()` only advanced TIMER-mode channels. The
  control words CBIOS actually programs are the textbook Z80-CTC
  baud-generator pattern: channel 0 is always TIMER mode with interrupts
  *disabled* (`tc=$D0` - it only needs to produce pulses, not interrupt
  the CPU), while channels 1-3 are COUNTER mode expecting to fire on
  every incoming pulse. Real hardware wires channel 0's ZC/TO output pin
  into channels 1-3's CLK/TRG input pins; `p2500_ctc_tick()` now does the
  same (not confirmed by board continuity, but the only source of pulses
  this emulator has, and the control-word shapes fit it exactly).

  **Verified live**: with the chain wired up, the `$E46C` tight loop is
  completely gone - CTC channels 1/2/3 fire real interrupts
  (vectors `$92`/`$94`/`$96`), and the run moved into a *different*
  pattern: 19,014 reads of the documented RX port `$06` (TODO.md's own
  port map: `$04` TX / `$06` RX, 9600-8N-2), all then unhandled. This
  confirmed it was no longer a hardware-timing bug but CBIOS correctly,
  actively polling for real serial/keyboard data - see the follow-up
  below for what happened once that input path was actually built.

  ---

  **Follow-up: `src/keyboard.{c,h}` (console RX/TX) + a second CTC fix.**
  Live disassembly of CBIOS's own port-`$06` ISR (traced via `--peek`,
  not guessed) showed no bit-shift/accumulate logic anywhere in it: it
  reads one complete byte from port `$06` and pushes it straight into a
  32-byte circular buffer (count at `$ED8F`, data from `$ED92`).
  *(Correction, see T19/T20: the conclusion drawn from this - that the
  console receive path is not bit-banged - was drawn from the wrong
  handler. The **channel 1** ISR at `$F597` samples port `$05` bit 6
  bit-by-bit, and channel 0's ISR re-arms channel 1 as a per-bit-cell
  TIMER. Port `$06` and this byte-level model may well still be right for
  whatever feeds that buffer, but they are not the whole receive path.)* Modeled
  port `$06`/`$04` at the byte level to match (`p2500_keyboard_in`
  delivers one queued byte per read, `$FF` idle; `p2500_serial_out`
  captures TX bytes - this project's only window into program output,
  since there's still no live video and `CONOUT` has never been called
  in any traced run). New `--type STRING` flag (`\r`/`\n`/`\t`/`\xHH`
  supported) queues keystrokes.

  Chaining *all three* CTC channels unconditionally (the fix above)
  turned out to be too broad: vector `$96`'s handler (channel 3) is
  specifically this RX-buffer push, with no filtering of the value it
  reads - chaining it on every single channel-0 pulse floods the buffer
  with the idle `$FF` byte within the first few thousand instructions,
  and it never drains (confirmed live: `$ED8F` stays at `$1F`, the "full"
  threshold, even after 20,000,000 steps), so every real keystroke queued
  afterward was silently dropped by the buffer-full check. Channels 1 and
  2 (vectors `$92`/`$94`, different handler tables entirely - confirmed
  via `--peek`, not assumed) don't have this problem and are still needed
  unconditionally (removing them regresses back to the original `$E46C`
  hang). Fix: `p2500_ctc_tick()` now takes a `channel3_rx_ready` flag
  (true exactly when `machine.c` has a queued keystroke waiting) and only
  pulses channel 3 when there's a real byte to deliver - standing in for
  a real keyboard controller's own "byte ready" strobe, which is the only
  sane real-hardware explanation for why channel 3 is wired differently
  from 1/2 in the first place.

  **Verified live**: with channel 3 no longer flooding, the RX buffer
  stays empty on its own (no keystrokes needed) and the run makes real
  further progress past a semaphore wait (`$EB7C`, part of a generic
  "wait for event ID" dispatcher CBIOS reuses at several boot-sequence
  synchronization points - not yet catalogued) that the flooded-buffer
  version never reached. **Not yet confirmed**: whether an actual
  delivered keystroke reaches CBIOS's buffer correctly end-to-end. Firing
  a real `--type '\r'` test found channels 1, 2, and 3 all requesting
  interrupts on the *same* tick, and the vendored z80 core
  (`z80_gen_int()`, `src/vendor/superzazu_z80/z80.c`) has only a single
  pending-interrupt slot (`int_pending`/`int_data`) - each call
  overwrites the last, with no queueing or priority ordering the way a
  real daisy-chained Z80 IM2 system would provide (Channel 0 highest
  priority, Channel 3 lowest, per the CTC datasheet). The keystroke's own
  vector (`$96`) was requested but the CPU trace shows execution never
  actually jumped to its handler afterward - almost certainly lost to
  this same-tick collision, or to a DI/EI window in CBIOS's own init
  code. This is a real, previously-unknown limitation of the interrupt
  delivery model, exposed by chaining multiple CTC channels together
  (nothing before this needed more than one pending source at a time).
  Status: `src/keyboard.{c,h}` and the channel-3 fix are solid,
  measurable progress (committed); reliable keystroke delivery needs
  either a priority-queued interrupt model or a narrower fix for this
  specific collision - not yet done.

### P3 — Phase 2: CP/M / CBIOS — COMPLETE (2026-09-27)

Written 2026-09-23 after an evidence review of the post-T16 tree; closed
2026-09-27. T17–T21, T24 and T26 are implemented, ISSUE-5 is resolved, and
CP/M reaches its prompt and runs commands. The entries are kept in full
because they are the evidence the current hardware model rests on; each
carries a **Done** note with what actually turned out to be true, which in
two cases (T19's channel assignment, T20's bit number) was not what the
review's first reading said.

The single most useful thing the close-out produced is `tools/disasm_ram.sh`:
the CP/M system files exist on disk only as sector-interleaved `.phi`
images, so `../Disk Images/disassembly/*.asm` are all based at org 0 and
their addresses mean nothing. A `--dump-ram` image taken at a breakpoint is
the ground truth, and z80dasm can be pointed straight at it:

```
./p2500-emu --disk "..." --max-steps 3000000 --dump-ram /tmp/ram.bin
tools/disasm_ram.sh /tmp/ram.bin E200 FFFF > /tmp/cbios.asm
```

Every CBIOS address quoted below was read that way, not guessed.

The boot now gets much further than `$4A00`: CP/M's page zero is real
(`$0000` = `C3 03 E2` = `JP $E203`, the CBIOS warm-boot vector), CBIOS's
own `$E200` jump table is live, and the CCP is loaded at `$CC00`. It then
**deadlocks** at `$EB02` (`LD A,$01 / CP (HL) / JR NZ,$EB02` with
`HL=$EB70`) — a semaphore wait that never clears. See ISSUE-5.

Reproduce the whole picture with:

```
./p2500-emu --disk "../Disk Images/extracted/P25K_B/P25K_B.raw" \
            --max-steps 5000000 --peek FF90:16 --peek F37F:32 \
            --peek F597:32 --peek F669:32 --peek FF33:32
```

- [x] **T17. Replace `z80_gen_int()` with a real IM2 daisy-chain
  interrupt controller. — DONE.** *This is the top blocker; several items below
  are unresolvable without it, and it is currently silently discarding an
  entire device's interrupt stream.*

  The vendored core (`src/vendor/superzazu_z80/z80.c`) has a **single**
  pending-interrupt slot — `int_pending` / `int_data`. Every
  `z80_gen_int()` call overwrites the previous one, with no queue, no
  priority, and no concept of a device holding `/INT` asserted until its
  own INTA cycle. Two independently measured consequences in the current
  build:

  1. **CTC channel 1's interrupt never reaches its handler — 0 of
     19,106 requests.** `p2500_ctc_tick()` pulses channels 1 and 2 in the
     same call, so channel 1's `z80_gen_int($92)` is always immediately
     overwritten by channel 2's `z80_gen_int($94)`. Measured over a
     5,000,000-step run: 38,213 CTC interrupt requests (19,106 × `$92`,
     19,107 × `$94`); `$94`'s handler body (`$F382`) executes 18,925
     times; `$92`'s handler body (`$F59A`) executes **zero** times.
  2. **The IPL's DMA end-of-block interrupt is lost too**, and always
     has been. Trace steps 791,658–791,675 (`P2500_TRACE_FROM=791100
     P2500_TRACE_TO=792000`): `OUT ($15)` at `$0A98` raises DMA vector
     `$02` (handler `$07F3`) with `IFF1=0`; seven instructions later
     `OUT ($12),$F3` at `$07E4` raises PIO vector `$00`, overwriting it;
     the `EI` at `$06B9` then dispatches `$00` only. `$07F3` is never
     executed in any run. Phase 1 reaching `$4A00` was not affected, so
     this went unnoticed — but it means *every* claim in this document of
     the form "interrupt X is delivered" is only true when nothing else
     asked at the same time.

  What to build, in `machine.c`, from the Z80 peripheral manual's
  interrupt chapter rather than by trial:
  - Each peripheral (PIO port A, PIO port B, DMA, CTC ×4) gets an
    explicit `int_requested` and `int_under_service` state, plus its
    vector. Peripherals set `int_requested` and **hold it** until
    acknowledged; they must not call into the CPU core directly.
  - `p2500_step()` acknowledges at most one interrupt per instruction
    boundary: the highest-priority device with `int_requested` and no
    higher-priority device `int_under_service`, and only when `IFF1` is
    set. That is the daisy chain's whole behaviour.
  - `int_under_service` clears on `RETI`. The vendored core does not
    expose `RETI` — either snoop `ED 4D` at the fetch (the same trick
    real peripherals use, since they watch the bus for the opcode) or
    patch the core to call a callback. **Note the IPL does not use
    `RETI`** (its handlers exit via `LD SP,($FF26)` + `JP`, see the RAM
    address table below), so IPL-side devices need a fallback release
    condition; CBIOS's own handlers should be checked for `RETI` before
    assuming either way.
  - Delete `resolve_im2_target`'s logging-only decode of the trampoline
    chain from the acknowledge path, or keep it strictly as a log.

  **Done.** `src/intctl.{c,h}` is the chain: per-source `requested` /
  `under_service` / vector, one acknowledge per instruction boundary in
  `p2500_step()`, and release on `RETI`. The vendored core gained one
  optional `on_reti` callback (marked LOCAL ADDITION in `z80.{c,h}`) so the
  chain can see `ED 4D` off the bus the way real peripherals do.

  The "IPL never uses RETI" problem turned out not to need a fallback
  heuristic. Two separate findings closed it:
  - The IPL *does* execute `RETI`, from a two-byte subroutine at `$0853`
    (`EI / RETI`) that its handlers `CALL` to release themselves *early*
    and then keep running. CBIOS does the same thing from `$EBE2`.
  - Independently, a device is also released whenever it is reset or has
    its interrupts disabled by a command written to it — which the floppy
    driver does constantly (`$73`/`$F3` to the PIO, WR6 `$A3` to the DMA).
    Both are datasheet behaviour, both are now modelled
    (`pio.on_int_reset`, `dma.on_int_reset`, CTC control-word reset).

  **The chain order was not a free choice after all.** The review assumed
  only hardware could settle it. The IPL settles half of it: **the DMA is
  upstream of the PIO.** Both `$07F3` (DMA end-of-block) and `$080B` (PIO)
  open with `LD ($FF26),SP / LD SP,$FF26` — same saved-SP word, same tiny
  private stack below it — so they cannot nest, and being entered while SP
  is already `$FF26` is fatal on its own, because the handler's first
  `PUSH` lands on `$FF24` where the interrupt's own return address was just
  pushed. With the PIO ahead of the DMA that is exactly what happens: the
  PIO handler releases itself at `$0853`, the still-pending DMA interrupt is
  taken at `$081C`, `$07F3`'s `PUSH AF` overwrites its own return address,
  and the machine returns to `$0042` and dies in an `RST 38` loop. Verified
  both ways with `--break 0038`. With the DMA first, both are serviced in
  turn from the normal stack and neither ever nests.

  Where the **CTC** sits relative to the FDD card is still open — the IPL
  never programs the CTC, so nothing from that era constrains it. It is
  placed last (the conservative reading: a clock tick cannot pre-empt a disk
  handler that released itself early) and stays on `ROADMAP.md`'s list.

  **Result, measured over the same run that used to lose them:** every
  request is now acknowledged 1:1 — `DMA 97/97, PIO port A 113/113,
  CTC ch2 2704/2704, CTC ch3 4/4`. `$07F3` executes 87 times, once per disk
  transfer, having never executed once before. The exit report prints the
  request/acknowledge counts so a future drop cannot be silent.

- [x] **T18. Drive the CTC from T-states, not instruction counts, and
  model the prescaler honestly. — DONE.** `ctc.c`'s `scaled_time_constant()`
  folds the prescaler in as a **×4** tick multiplier for `/256` and ×1
  for `/16` — the real ratio is 16:1, and the unit is "one
  `p2500_ctc_tick()` per instruction" rather than a clock. The file's own
  comment calls this "close enough", which makes every timing conclusion
  drawn from this emulator's CTC unreliable, and is precisely the class
  of tuning constant `ROADMAP.md`'s guiding principle says should not
  exist.

  The fix is cheap and removes the fudge entirely: the vendored core
  already maintains `z->cyc`, a monotonic T-state counter that
  `z80_step()` never resets. Pass the elapsed T-state delta into
  `p2500_ctc_tick()` and decrement by `delta / prescaler` with a carried
  remainder. Channel 0 as CBIOS programs it (`tc=$D0`, `/16`) then
  becomes a real 3,328-T-state period (≈1.2 kHz at 4 MHz) instead of
  "208 instructions", which is also what makes T19 answerable at all.

  **Done**, and it paid off immediately as a *check* rather than just a
  cleanup. CBIOS's table at `$F546` is seven `{control, time constant}`
  pairs selected by a baud index at `$F727`; read through the real
  prescaler at the confirmed 4 MHz they are
  `/256×208 = 75.1`, `/256×142 = 110.0`, `/256×104 = 150.2`,
  `/256×52 = 300.5`, `/256×26 = 601.0`, `/16×208 = 1201.9`,
  `/16×104 = 2403.8` — the standard rates 75/110/150/300/600/1200/2400,
  each within 0.3%, with `$F727` = 5 (1200 baud). A wrong prescaler or a
  per-instruction tick decodes that table to nothing, so this is a real
  falsifiable confirmation of both the CTC model and the 4 MHz clock. The
  `--count`-able consequence: channel 2's 50 Hz strobe produces 2,704 ticks
  in 56.068 s of emulated time, which is 50.0 Hz once the ~2 s before CBIOS
  programs the channel is subtracted. `p2500_ctc_read` no longer rescales.

- [x] **T19. Re-derive the CTC channel wiring from CBIOS's own ISRs. — DONE;
  the chain was indeed fictional, and the review's own channel assignment
  turned out to be half wrong.** T16's write-up is
  honest that the chain is unconfirmed; the review found direct evidence
  against it, and — more importantly — found the handlers that settle it.

  The control words CBIOS actually programs (from `--verbose-io`):

  | Ch | Control | Decode | TC |
  |---|---|---|---|
  | 0 | `$07` | int **off**, TIMER, `/16`, reset | `$D0` |
  | 1 | `$C7` | int on, COUNTER, **falling** edge | `$01` |
  | 2 | `$D5` | int on, COUNTER, **rising** edge | `$01` |
  | 3 | `$C5` | int on, COUNTER, **falling** edge | `$01` |

  Evidence against the chain:
  - **Channel 2 counts the opposite edge from channels 1 and 3.** If all
    three were wired to the same channel-0 `ZC/TO` pin, that would buy
    nothing but a half-period phase shift. Different edges is what you
    get when three CLK/TRG pins are wired to three *different* external
    signals — which is also the canonical Z80-CTC idiom for "turn an
    external edge into an IM2 interrupt" (COUNTER, `tc=1`, int enabled).
  - Three ISRs firing in permanent lockstep on one event is not a design
    a real board would have; it is an artifact of the chain.
  - `ctc.c` parses `rising_edge` and then never uses it, which is the
    code admitting the same thing.

  Evidence for what the channels really are — CBIOS's own IM2 table at
  `I=$FF` (`--peek FF90:16`) is `0C FF 15 FF 03 FF FA FE …`, i.e.
  vector `$90`→`$FF0C`→`$F669`, `$92`→`$FF15`→`$F597`,
  `$94`→`$FF03`→`$F37F`, `$96`→`$FEFA`:
  - **`$F669` (channel 0)** does `LD A,($F731) / OR $87 / OUT ($01),A /
    LD A,($F732) / RRA / OUT ($01),A` — it **reprograms channel 1** with
    `$87` (int on, **TIMER**, `/16`, tc follows, reset) plus a computed
    time constant. That is the second half of a textbook bit-banged async
    receive: COUNTER mode to catch the start-bit edge, then TIMER mode
    re-armed per bit cell. A channel that is *reprogrammed* by another
    channel's ISR is not a channel being clocked by it.
  - **`$F597` (channel 1)** does `LD HL,$F733 / BIT 6,(HL) / JR Z,… /
    IN A,($05) / BIT 6,A / JR Z,…` — it samples **port `$05` bit 6**.
    See T20: that is almost certainly RXD, and it means the console
    receive path really is bit-banged, contradicting `keyboard.h`'s
    current "no bit-shift logic anywhere in it" conclusion (which was
    drawn from the *channel 3* handler, a different routine).
  - **`$F37F` (channel 2)** does `CALL $EBE2 / LD B,3 / LD HL,$F436 /
    INC (HL) / …` — an incrementing multi-byte counter, i.e. a real-time
    clock tick.
  - Note channel 0 has a **registered handler** (`$F669`) but is
    programmed with interrupts **disabled** (`$07`). Either something
    else vectors to `$90`, or a control word this project has not traced
    re-enables it. Worth resolving — it is the one loose end in the
    reading above.

  Deliverable: identify each of the three CLK/TRG sources, model them as
  explicit device strobes, and delete both the unconditional `ch0 → ch1/
  ch2` chaining and the `channel3_rx_ready` parameter. If a channel turns
  out to have no source this emulator can supply yet, it should simply
  never tick — not be fed a substitute pulse.

  **Done — and the vector→handler mapping above was read backwards.** Each
  word in the `$FF90` table points at a *bank-switching* stub that loads the
  real handler address and self-patches the `JP` at `$FF51`. Following them
  through:

  | Vector | Stub | Handler | What it is |
  |---|---|---|---|
  | `$90` (ch0) | `$FF0C` | **`$F597`** | serial **transmit** bit clock |
  | `$92` (ch1) | `$FF15` | **`$F669`** | serial **receive** bit sampler |
  | `$94` (ch2) | `$FF03` | **`$F37F`** | real-time clock tick |
  | `$96` (ch3) | `$FEFA` | **`$ED2A`** | **keyboard** byte ready |

  So `$F669` is channel **1**'s handler, not channel 0's — consistent with
  `$F52E` patching `$F669+1` in the same breath as programming channel 1
  (`$F531`: `OUT ($01),$C7`). That also closes the "loose end" above:
  channel 0 has a registered handler *and* interrupts disabled at init
  because its interrupt is armed only while a byte is being transmitted
  (`$F57F`: `($F731) | $87` → `OUT ($00)`), which is exactly what a
  bit-banged transmitter should do.

  The four channels as modelled now (`src/ctc.h` carries the same table):
  - **ch0** TIMER, no CLK/TRG input at all. Transmit bit clock.
  - **ch1** CLK/TRG = the serial **RXD** line. COUNTER/falling/tc=1 catches
    the start bit; `$F669` then reprograms channel 1 *itself* as a TIMER at
    half a bit time and thereafter whole bit times, sampling port `$05`
    bit 7 into a shift register at `$F73D`, and finally restores `$C7`/tc=1
    to wait for the next start bit.
  - **ch2** CLK/TRG = an unidentified 50 Hz strobe → the 24-bit tick counter
    at `$F436`.
  - **ch3** CLK/TRG = the keyboard controller's "byte ready" strobe. `$ED2D`
    does one `IN A,($06)` and pushes the whole byte into a 32-byte ring at
    `$ED8F`. So `keyboard.h`'s byte-level model was right — it just belongs
    to the keyboard, while the bit-banging belongs to the serial port.

  `channel3_rx_ready` is gone. `p2500_ctc_tick()` takes T-states and touches
  only TIMER channels; `p2500_ctc_set_clk_trg()` drives the three real pins;
  `rising_edge` is finally used. A channel whose real source this emulator
  cannot supply (ch1's RXD, nothing plugged in) simply never ticks, and
  ch0/ch1 duly report `0/0` requests in a CP/M run — correct, because CP/M's
  console output goes to video, not the serial line.

- [x] **T20. Make port `$05` readable. It is a live, polled input, not
  just the bank latch. — DONE.** Supersedes T15's open question ("confirm whether
  anything reads port `$05`"): `$F5A4` executes `IN A,($05)` and tests
  **bit 6**, inside the CTC channel-1 (serial receive) ISR. Today
  `port_in` has no `$05` case, so it falls through to the `0xFF` default
  — bit 6 reads as a permanent 1, i.e. a permanently-idle (or
  permanently-asserted, depending on polarity) receive line.

  This is currently masked by T17: the ISR that reads it never executes,
  which is why a `--verbose-io` run reports **no** unhandled `IN` at all.
  Expect it to start appearing the moment T17 lands. Model bit 6 as RXD
  driven by `keyboard.c`'s queue, and check the remaining bits against
  `../Tracing/P2500-predicted-wiring-from-firmware.md` §C1–C4 before
  guessing at them.

  **Done, with the bit numbers corrected.** There are *two* readable bits
  and they do different jobs:
  - **bit 7 is RXD.** `$F6A9` does `IN A,($05) / RLA` — it wants the bit in
    carry, so it is sampling bit 7, once per bit cell, inside the channel-1
    receive state machine.
  - **bit 6 is a transmit handshake/ready input.** `$F5AD` does
    `IN A,($05) / BIT 6,A` inside the channel-0 *transmit* ISR, gated on a
    flag `$F50A` sets from a configuration byte at `$F72A`, and backed by a
    timeout counter at `$F72D`. That is the bit the review saw; it is not
    RXD.

  Both idle high (marking RXD, asserted handshake) — what an unplugged line
  with a pull-up looks like. `../Tracing/
  P2500-predicted-wiring-from-firmware.md` §C4 predicted this port was
  readable at "Likely" confidence before any code that reads it had been
  found, which is a clean independent hit for that document's method. The
  remaining bits still read back as 1; nothing traced reads them.

- [x] **T21. Fix `dma.c`'s WR3 base-register pattern. — DONE.** `dma.c` decodes
  WR3 as `D7,D1,D0 = 0,0,0`, which makes the branch **dead code**: every
  such byte is claimed by the WR1 (`D2=1`) or WR2 (`D2=0`) test above it.
  `dma.h`'s comment treats the overlap as an unresolvable ambiguity in
  UM008101 — it is resolvable: the Zilog **Component Data Book (1985)**
  DMA write-register bit map (`Datasheets and manuals/Zilog Component
  Data Book (1985) (OCR).pdf`, the WR3 figure carrying "DMA ENABLE /
  INTERRUPT ENABLE / STOP ON MATCH") shows WR3 as **`D7=1`, `D1=D0=0`**,
  which is unambiguous against WR1/WR2 and matches every other Z80-DMA
  implementation. UM008101's Figure 43 printing `D7=0` is the erratum.

  Consequence worth fixing before Phase 2 exercises it: WR3 bit 6 is
  **DMA Enable** and bit 5 is **Interrupt Enable** — the documented
  "fast one-byte enabling" alternative to WR6 `$87`/`$AB`. A driver that
  uses it would today be logged as `unrecognized base register byte`, and
  the transfer would be silently dropped by `p2500_dma_deliver`'s
  `dma_enabled` guard. Nothing traced so far sends WR3, so this is
  latent, not active.

  **Done:** the test is now `(value & 0x83) == 0x80`, placed where it is
  unambiguous against WR1/WR2/WR4/WR5/WR6, and `dma.h`'s comment records the
  erratum rather than an unresolvable ambiguity. Still latent — nothing in
  any traced run sends WR3 — but no longer dead code.

- [ ] **T22. Invert the DMA↔FDC data path, and give the DMA real
  end-of-block semantics.** Today `fdc.c`'s `do_read_data()` *calls*
  `p2500_dma_deliver()` and hands it a raw pointer into the disk image;
  the DMA never checks that its Port A address is `$15`, that Port A is
  programmed as I/O-fixed, or that Port B is memory-incrementing (`$15`
  is parsed and logged as "not modeled", 88 times per run). This
  inversion cannot express `WRITE DATA` or `FORMAT A TRACK`, where the
  DMA must **push** bytes from RAM into the FDC. Restructure so the DMA
  owns the transfer and pulls from / pushes to whichever device its
  programmed Port A address selects.

  While there: on end-of-block the model neither advances `port_b_addr`
  nor clears `dma_enabled`. A real Z80-DMA without auto-restart stops and
  must be re-enabled, and a driver that relies on that (or that reads
  back RR0's status byte / the address counters — `read` registers are
  not modeled at all) will be misled.

- [ ] **T23. Finish the µPD765 command set for Phase 2.** `fdc.c`
  implements SPECIFY, SENSE DRIVE STATUS, READ DATA, RECALIBRATE, SENSE
  INTERRUPT STATUS and SEEK. `ROADMAP.md`'s Phase 2 needs **WRITE DATA**
  (`$05`, handler `$05A1`) and **FORMAT A TRACK** (`$0D`, handler
  `$0B19`), and `READ ID` is the usual next thing a CP/M BIOS reaches
  for. Both decode paths are already documented above; only the FDC side
  is missing. Depends on T22.

  Also in `do_read_data()`, all three currently-ignored parameters become
  live once CBIOS starts seeking:
  - **`H` (head) is ignored.** Correct for stock single-sided P2500 media
    and therefore fine today, but it should at minimum log and fail a
    non-zero `H` rather than silently aliasing to head 0.
  - **`N` (sector-size code) is ignored** in favour of the hardcoded
    `P2500_FDC_SECTOR_SIZE` 256. The DMA already computes its byte count
    as `sectors × (128 << N)`, so the two can disagree.
  - **Multi-sector transfers do not wrap at the track boundary.** The
    whole remaining disk image is handed to the DMA as one flat span, so
    a read that runs past `EOT` walks straight into the next track
    instead of terminating. The IPL only ever reads track 0, so this has
    never mattered; a seeking BIOS will hit it.
  - *Confirmed correct, do not "fix":* sector skew. The real media is
    interleaved `1,3,5,…,2,4,6,…`, but `../Disk Images/findings/
    EXTRACTION-NOTES.md` records that the `.raw` dumps are already in
    logical sector-number order (built from each IMD track's own sector
    map), which is why the `diskdefs` all use `skew 1`. `lba =
    physical_track × 16 + (R − 1)` is right.

- [x] **T24. Add `make test` — a regression script, not just the one
  SESAM diff. — DONE.** Phase 1 was expensive to win and nothing currently
  guards it. Three cheap, falsifiable checks, all already reproducible
  from the CLI:
  1. SESAM banner VRAM byte-exact vs
     `../ROM Dumps/CPU-Card-Boot-EPROM/emulation/vram_after_banner.bin`
     (today's only test — keep it).
  2. `--disk P25K_B.raw` reaches landmark `$4A00` un-gated, and the
     `$1000`/`$1002`/`$1003`/`$1006` sector-entry landmarks all hit.
  3. After boot, `$0000`–`$0002` reads `C3 03 E2` (CBIOS warm-boot
     vector) and `$0005`–`$0007` reads `C3 06 D4` — a one-line proof that
     CP/M page zero was really built, which no NOP-sled can fake.

  Make each of these exit non-zero on failure so they can gate commits.

  **Done:** `tools/run_tests.sh`, wired to `make test`, 17 checks in ~8 s,
  exit 1 on any failure. It got a fourth group beyond the three planned,
  which is now the most valuable one: **typing `DIR` at the prompt must list
  exactly the six files the disk image contains** (PIP, SYSGEN, SYSCBI,
  SYSLOAD, SYSPBI, SYSCPM). That single check exercises the keyboard strobe,
  port `$06`, the interrupt daisy chain, the FDC/DMA read path, the
  directory decode and CONOUT at once, and its expected output is fixed by
  the disk image rather than by this emulator — nothing internal can fake
  it.

- [x] **T25. Harness gaps that this review had to work around. — DONE.** All
  three cost real time during it:
  - **`--watch ADDR[:LEN]`.** `main.c` hardcodes watches on `$0003` and
    `$0039`. Finding who writes the `$EB70` semaphore (ISSUE-5) needed a
    throwaway patched build; it should be a flag.
  - **`--break ADDR` / `--count ADDR`.** Answering "does `$F59A` ever
    execute?" also needed a patched build. It is the single most useful
    question this project keeps asking.
  - **The stuck detector does not catch cycles.** `STUCK_DISTINCT_
    THRESHOLD` is 8 distinct addresses in a 200,000-step window. The
    ISSUE-5 deadlock cycles through **64** addresses and therefore ran
    the full 5,000,000 steps reporting nothing but "hit max-steps".
    Detect a repeating PC *sequence* (e.g. hash the window and compare
    against the previous window) instead of, or in addition to, counting
    distinct addresses.
  - Minor: T8's landmark gate compares against a *static* expected byte,
    so once CP/M legitimately reuses `$1000` the gate reports "didn't
    match" for a real revisit (`$1000` and `$1003`, 1 each, in every
    current run). Either snapshot the expected bytes at first hit or
    scope the gate to addresses whose content is genuinely invariant.

  **Done.** `--watch ADDR[:LEN]` (defaults to the two page-zero canaries
  when none is given), `--count ADDR` (reporting the first step it hit),
  `--break ADDR`, `--dump-ram PATH`, `--no-stuck-detect`, and
  `--type-after MS`. The exit report now also prints emulated time in
  seconds and the per-device interrupt request/acknowledge counts, so a
  future silent drop cannot hide. `read_whole_file()` checks `malloc` and
  rejects a non-positive size.

  The cycle detector took two attempts and the second one is the
  interesting part. Comparing repeated *PC* sequences fires immediately on
  the IPL's own 64 KB RAM test (`$016B`), an 11-instruction loop whose PC
  sequence repeats perfectly for hundreds of thousands of steps while making
  real progress. It has to be **state**: PC plus every visible register,
  unbroken for 20,000 steps. A loop that repeats all of that cannot be
  making progress, because nothing it could be waiting on is reaching a
  register — while a polling wait on a location an ISR writes (ISSUE-5's
  `$EB02`) does repeat state, and is correctly caught. It found ISSUE-5's
  deadlock at step 1,150,000 with a reported period of 208 instructions.

  The T8 gate now only reports a mismatch while the landmark has never
  legitimately hit; once it has, a mismatch just means CP/M reused the
  address, which is correct behaviour and was being reported as a failure
  every run.

- [x] **T26. Small correctness items, individually cheap. — DONE.**
  - `machine.h`'s `crtc_regs[16]` with `crtc_index & 0x0F`: the MC6845
    has **18** registers (R0–R17). R16/R17 (light pen) currently alias
    onto R0/R1 (horizontal total / displayed), so any access to them
    corrupts the geometry the CRTC model is trusted for.
  - `ctc.c` accepts an interrupt-vector byte written to any channel; a
    real Z80-CTC loads it only from **channel 0**. Verified harmless
    today (CBIOS writes `$90` to port `$00`, 6×, always channel 0), but
    the permissiveness would hide a future misread. Log the channel while
    you are there — the current message does not say which port it came
    from.
  - `ctc.c` applies the prescaler in COUNTER mode; on real hardware the
    prescaler is TIMER-mode only. Folded into T18.
  - `machine.c`'s `resolve_im2_target` reads `m->ram[]` directly instead
    of `p2500_peek()`. Harmless while IM2 tables live at `$FE00`/`$FF00`,
    wrong in principle.
  - `main.c`'s `read_whole_file()` does not check `malloc` or reject a
    zero/negative `ftell`.

  **Done**, all five. `crtc_regs` is 18 entries with an explicit range check
  (a write to R18+ is logged and dropped rather than aliased); the CTC
  accepts a vector byte on channel 0 only and logs the channel when it
  refuses one elsewhere; the prescaler now only exists in TIMER mode by
  construction (T18); `resolve_im2_target` uses `p2500_peek`;
  `read_whole_file` is hardened.


### P4 — Phase 3: a usable machine

Written 2026-09-27, with CP/M at its prompt and `make test` green. These are
the items P3 deliberately deferred plus what running CP/M exposed. Nothing
here is blocked on hardware.

- [ ] **T27. Video attributes.** The screen renders correctly today because
  nothing it prints uses attributes. The video card carries **12** MB8116E
  (16 Kbit x 1) parts, which is 16 K words x 12 bits — an 8-bit character
  code plus a 4-bit attribute nibble — not the flat 16 KB byte bank
  `machine.c` models at `$8000`-`$BFFF`. Anything that reverses, dims or
  underlines text will expose the difference. Start from
  `../Actual P2500 hardware/P2500 Video Card/P2500-video-card-findings.md`,
  and look for CBIOS writing a second plane: `$E4C3` is CONOUT, and the
  escape-sequence handling around it is the place attributes would appear.
  `tools/render_vram.py` will need the same treatment.

- [ ] **T28. Serial transmit, end to end.** Everything needed is in place
  and nothing exercises it: CTC channel 0 is the transmit bit clock, its ISR
  is `$F597`, port `$04` is the data bit, port `$05` bit 6 is the handshake
  input the ISR waits on (T20). `PIP LST:=FILE.TXT` at the prompt should
  make the whole path observable through the existing `[tx]` logging — and
  it is a genuine test of the T18 timing model, because a wrong bit rate
  produces recognisably mangled characters rather than nothing. Note the
  handshake: with `$F72A` bit 0 set, `$F5A6` will not transmit until bit 6
  reads high, and the timeout at `$F72D` gives up if it never does.

- [ ] **T29. Identify what really drives CTC channel 2's CLK/TRG.** The
  model assumes 50 Hz (`P2500_CLOCK_TICK_HZ` in `machine.h`), which is
  right for both candidates — mains and the video frame rate — so CP/M
  keeps good time either way. It is still an assumption, and it is the only
  frequency in the emulator that is not derived from the 4 MHz crystal.
  Firmware may be able to settle it after all: find what reads the 24-bit
  counter at `$F436` and what it divides by. If something converts it to
  seconds with a constant, that constant *is* the tick rate.

- [ ] **T30. Invert the DMA<->FDC data path (was T22), then finish the
  uPD765 command set (was T23).** Both are unchanged and still needed for
  writing to disk; see T22 and T23 above for the full write-ups. Nothing in
  CP/M's read-only path needed them, which is why the prompt was reachable
  without them. The first thing that will need them is `PIP` copying a file
  onto the disk, which is also the natural test.

- [ ] **T31. Multi-track reads past `EOT`.** Folded out of T23 because it is
  independent of the DMA inversion and now reachable: `do_read_data()` hands
  the DMA the whole remaining disk image as one flat span, so a multi-sector
  read that runs past the last sector of a track walks into the next track
  instead of terminating. CP/M's directory reads stay inside one track,
  which is why `DIR` is correct; a large sequential file read will not.

- [ ] **T32. UCSD p-System boot.** `P2k5_LOGIC` and `P2k5_TKS` in
  `../Disk Images/extracted/` use a genuinely different bootstrap, so they
  are the best independent check on the disk path that exists — and the
  first thing that will test the IM2 chain against software that was never
  considered while building it. Worth trying as-is before anything else in
  P4: it costs one command.

---

## Known issues (tracked, not yet root-caused)

- **ISSUE-1: boot stalls in the `$06C6`/`$FED5` busy-wait — RESOLVED.**
  Root cause: `$043E` (the `$0422` table's ID=0 "drive prep" handler) does
  `SPECIFY`, one defensive `SENSE INTERRUPT STATUS`, enables PIO port A
  interrupts, then blocks (`CALL $06C6`) waiting for `$FED5==1`, which only
  the vector-0 ISR at `$0883` sets — reached via a real FDC `INT` on PA0.
  Nothing had generated that interrupt yet: `SPECIFY`/`SENSE INTERRUPT
  STATUS` correctly don't fire one, and no `RECALIBRATE`/`SEEK`/`READ
  DATA` had been issued. Full trace evidence (bracketing `$043E`'s hit
  through `$06C6`) is preserved in git history; see the two fixes applied:

  1. **Fixed**: `fdc.c`'s `SENSE INTERRUPT STATUS` now returns Invalid
     Command (`$80`, 1 byte) instead of a fabricated "Seek End" whenever
     nothing is genuinely pending (new `seek_int_pending` flag, set only
     by real RECALIBRATE/SEEK completion, cleared by the SIS that reports
     it). Verified to have *no* effect on the stall by itself, as
     expected — confirmed the deadlock wasn't there.
  2. **Fixed**: real uPD765s generate one unsolicited interrupt per
     configured drive shortly after reset (the standard reason PC BIOS
     floppy drivers issue several defensive `SENSE INTERRUPT STATUS`
     calls after reset) — this ROM's own structure confirms it needs
     *two* such interrupts in a row before ever issuing a real command
     (`$0883` handles the first, a second lighter ISR at `$0752` handles
     the second). `p2500_fdc_raise_startup_interrupt()` (`fdc.c`) now
     fires one each time PIO port A interrupts are (re-)armed, budgeted at
     4 (the real uPD765 max-drive count, since the exact count this board
     uses isn't known) and permanently disabled once a real
     Recalibrate/Seek/Read Data is issued (`real_operation_started`), so
     it can only help early boot sequencing, never mask a later real bug.
     Triggered from `machine.c`'s port `$12` (PIO port A control) handler,
     which now diffs `int_enabled` before/after each write.

  **Result**: the boot now runs all the way through `$06C6` (twice), the
  real ISR chain (`$03E3` → `$0422` table dispatch), and reaches **`$059C`
  — the `$0422` table's ID=4 handler, the actual READ DATA dispatch** —
  for the first time ever. `$4A00` is still not reached; see ISSUE-2,
  encountered immediately after this fix while chasing the next stall.

- **ISSUE-2: the READ DATA DMA-program stream "starts 3 bytes late" —
  RESOLVED, and it turned out not to be a bug at all.** Originally found
  by tracing `sub_0b32h` (the `$06` READ DATA DMA-setup routine) sending
  what looked like a 17-byte stream instead of the "full" 20-byte
  template, starting 3 bytes in. The fix-1-then-2 write-up first read
  this as a ROM control-flow bug (`sub_0b32h` leaving `HL=$FEB6` instead
  of resetting it to `$FEB3` before the shared streaming routine).

  That reading was wrong. Reading the real datasheet the project has on
  hand for this exact part (`Datasheets and manuals/Zilog Z80 Family CPU
  Peripherals User Manual.pdf`, chapter "Direct Memory Access", "Write
  Registers", Figures 39-46) shows the Z80-DMA's register-load protocol
  was never positional to begin with: every byte the chip receives at the
  top level is **self-describing** — specific bits say which of WR0-WR6
  it is, other bits say how many follow-up bytes come next. The chip
  explicitly supports short, incremental "only reprogram what changed"
  loads (the Data Book calls this out by name: "Next-operation loading
  without disturbing current operations"), and the manual states plainly
  that WR0 should specifically **not** be the first byte sent when only
  changing direction. `sub_0b32h`'s 17-byte stream is exactly that: a
  valid, complete, self-contained short reprogram (new direction + new
  block length via one WR0 byte, `$6D`) that correctly omits re-sending
  Port A's address, since Port A (the FDC, `$15`) never changes between
  reads. Decoding the full stream byte-by-byte against the manual's bit
  rules confirms it exactly, including two standalone WR6 `$A3` ("RESET
  AND DISABLE INTERRUPTS") commands sent *mid-stream* — something a fixed
  20-byte template could never explain, but which falls straight out of
  the self-describing reading.

  **Fix**: `dma.c` was rewritten from a fixed-position template decoder
  into a real self-describing parser (WR0/WR1/WR2/WR4/WR5/WR6 fully
  implemented and cross-checked against two independent live-captured
  streams; WR3 implemented defensively — see the file's own doc comment
  for the one open bit-pattern ambiguity between WR2 and WR3 that the
  manual doesn't resolve, and which nothing in this ROM's traced behavior
  exercises anyway). Verified live: block length now decodes as the
  correct, clean `4096` bytes (was `4260`), Port B address as `$1000`
  (was `$8A02`, and `$1000` is exactly the boot sector's own load
  address — sensible where the old value wasn't), and the interrupt
  vector as `$02` (was `$AB`, actually a WR6 command byte misread as
  data). `--verbose-io` now logs every decoded WR group and field.
  Status: resolved and committed.

- **ISSUE-3: a live run deadlocked before `READ DATA` was ever issued —
  RESOLVED.** `Datasheets and manuals/NEC UPD765.PDF` (the *real* µPD765
  datasheet — `NEC UPD765C.PDF` is mislabeled and is actually a
  µPD780/Z80-clone CPU datasheet with no FDC content at all, worth
  renaming/flagging) settled the first question directly:

  > "It is important to note that during the Result Phase all bytes
  > shown in the Command Table must be read... The µPD765 will not
  > accept a new command until all seven bytes have been read."

  So `fdc.c` refusing a new command while a result byte sits unread is
  correct, real-hardware-accurate behavior — the two candidate `fdc.c`
  fixes floated in an earlier version of this entry were both wrong. The
  real question was why the ROM leaves a byte unread when a real board
  evidently boots fine, and tracing the exact instruction that skips the
  read answered it precisely:

  `$0786` arms PIO port A interrupts (`$0791: OUT ($12),A`) and then, with
  **no `EI` in between**, immediately streams whatever command is sitting
  in `$FEA2` (`$0794: CALL $0A87`) — the `RECALIBRATE` command `$056B`'s
  handler had just built. That's correct on real hardware: a `RECALIBRATE`
  completion interrupt physically can't arrive before the command bytes
  (and the seek behind them) are sent. But TODO.md ISSUE-1 fix 2's
  synthetic "post-reset unsolicited interrupt" fires *synchronously* the
  instant PIO is armed — hijacking control before `$0794` can run, into
  ISR `$0798`, whose own internal `SENSE INTERRUPT STATUS` (`$0A78`,
  overwrites `$FEA2`) clobbers the command buffer. Control then returns to
  `$0794`, which streams the now-stale `SENSE INTERRUPT STATUS` instead of
  `RECALIBRATE` — via the bare streamer (`$0A87`/`sub_0a87h`, confirmed by
  full disassembly to never touch port `$15` for input), so that result
  goes permanently unread. `RECALIBRATE` never gets sent at all.

  **Fix, two parts** (`fdc.c`/`fdc.h`/`machine.c`):
  1. Lowered the fix-2 budget from 4 to 2 — pinned down by tracing, not
     guessed: the ROM's own structure only performs two PIO-arm events
     before a real command is built and ready to stream (`$0786`'s is the
     third arm event, and it's structurally different from the first two).
  2. A second, compounding bug: the synthetic interrupt was delivered
     through `fdc->int_line`'s held-level path (T9), which assumes the ISR
     eventually reads a SIS result to clear it. True for the first
     synthetic interrupt's handler (`$0883`), **not** the second's
     (`$0752` — confirmed by full disassembly to never touch the FDC at
     all). Holding the level left it stuck asserted after `$0752` ran,
     spuriously re-firing the next time PIO interrupts happened to be
     re-armed for something unrelated — which is what was actually
     reaching `$0798`, not a third budgeted firing. Fixed by delivering
     the synthetic interrupt as a one-shot pulse (mirroring the pre-T9
     model) instead, decoupled from the held-level path real FDC
     completions still correctly use.

  **Verified live**: `RECALIBRATE` and `READ DATA` are issued for the
  first time ever, the DMA delivers a real 4096-byte transfer, and PC
  reaches **`$4A00`** — sector-0's own `JP` target, the actual Phase 1
  success criterion — for the first time. See ISSUE-4 for what's next.
  Status: resolved and committed.

- **ISSUE-4: the first real `READ DATA` reads the wrong disk location —
  RESOLVED. PHASE 1 COMPLETE.** The `READ DATA` command ISSUE-3 unblocked
  read C=1, H=0, R=1 into RAM at `$1000` - and the disk's known boot
  sector (`00 00 FB 11 30 10 CD 03 00 ...`) sits at raw-file byte offset
  0, which `do_read_data`'s naive `lba = C * 16 + (R-1)` mapped to C=0,
  not C=1. Guessing at the geometry wasn't necessary: the user pointed at
  `Information from the internet/...Diskettenhandling für PHILIPS
  Computer P2000M, P2500...VzEkC...`, a P2500 owner's own writeup of
  debugging this *exact* mismatch against stock 22DISK:

  > "Philips hat hier ein etwas abgeändertes Diskettenformat verwendet,
  > bei dem sich die physische und die logische Tracknummern
  > unterscheiden. Logische Tracknummer ist immer um 1 höher als die
  > physische." (*Philips used a slightly modified diskette format here,
  > where the physical and logical track numbers differ. The logical
  > track number is always 1 higher than the physical.*)

  So `C` in every READ/SEEK/RECALIBRATE command is the disk's own
  recorded (logical) track ID, one higher than the physical track this
  flat `.raw` dump is ordered by. Fixed in `do_read_data`: physical track
  = `C - 1`. Verified live: C=1,R=1 (the IPL's actual first real read)
  now resolves to byte offset 0 - exactly the disk's known, byte-exact
  boot sector - confirming this was never a "wrong request" (ISSUE-4's
  original framing) but this project's own geometry assumption being off
  by one track the whole time.

  **Result — the full boot now works**: the boot sector loads at `$1000`
  for real (`$1000`/`$1002`/`$1003`/`$1006` landmarks all hit,
  un-gated), execution reaches **`$4A00`** for real (un-gated - the
  actual Phase 1 success criterion), and the IPL goes on to load and
  jump into **CP/M's own CBIOS (`SYSPBI.PHI`)** - confirmed by its
  distinctly different `SPECIFY` parameters, DMA interrupt vector, and
  RAM target from the IPL's own. This is well past Phase 1 territory.

  **What it runs into next** (already diagnosed, not yet fixed): CBIOS
  issues its own `RECALIBRATE` and waits for the completion interrupt,
  but the interrupt never arrives because `machine.c`'s
  `resolve_im2_target` hardcodes the IM2 vector table page to `$FE`
  (`#define IM2_TABLE_PAGE 0xFE`) - correct for the IPL, but CBIOS sets
  up its own IM2 table at register `I = $FF` instead, so every interrupt
  it generates is silently suppressed
  (`log_suppressed`: `"I=$FF not yet at table page $FE"`). This is a
  straightforward next fix (read `I` live instead of assuming a fixed
  page) but belongs to Phase 2 (CBIOS/CP/M), not this document's P0-P2
  IPL-focused work queue - worth its own entry once picked up.
  Status: resolved and committed. Phase 1's own success criterion
  (`$4A00`) is met.

- **ISSUE-5: CBIOS deadlocks at `$EB02` on a semaphore that is never
  signalled again — RESOLVED by T17.** This is
  where a current `--disk` run ends up, and it is further than anything
  before it: CP/M page zero is genuinely built (`$0000` = `C3 03 E2` =
  `JP $E203`, CBIOS's warm-boot vector; `$0005` = `C3 06 D4`), CBIOS's
  `$E200` jump table is live, and the CCP is at `$CC00`.

  The loop is `$EB00: LD A,$01 / CP (HL) / JR NZ,$EB02` with `HL=$EB70`
  — one of four event slots (`$EB70`–`$EB73`), addressed by a 3-byte
  record table at `$EB5C` (`00 70 EB / 01 70 EB / 02 70 EB / 03 70 EB`).
  This is the generic "wait for event ID" dispatcher T16's write-up
  already noticed at `$EB7C`.

  Watching writes to `$EB70`–`$EB73` (a throwaway instrumented build —
  see T25) shows the pair working correctly for a while: `$EC18` signals
  (`0 → 1`), `$EAE1` consumes (`1 → 0`), alternating through step
  1,120,400 — and then `$EC18` is never reached again for the remaining
  3.9 M steps. So the wait side is fine; the **signal side stops being
  driven**.

  The attribution to T17 was right, though not for the reason guessed. The
  wait at `$EB02` is the tail of `$EAC0`, a generic "post a request, then
  spin until its status byte changes" routine; `$EC10` (reached via `$EC17`'s
  `INC (HL)`) is the completion signal. What was not arriving was the
  **disk** side, not the clock: with a single interrupt slot the DMA's
  end-of-block interrupt was overwritten by the PIO's every single time, so
  `$07F3` never ran and no transfer was ever reported complete.

  With the daisy chain in place (T17) the deadlock is gone outright: `$07F3`
  runs 87 times, CP/M finishes loading, prints `58K CP/M Ver. 2.2`, and sits
  at `A>` polling CONST/CONIN at `$E46C` — which is an *idle* loop, not a
  stall. `$EB02` is no longer reached at all in a normal run.

---

## Useful RAM addresses (all in the `$FE00`-based scratch area)

Established by hand-tracing; a memory dump of `$FE00`–`$FEFF` at any
breakpoint is the highest-value single debugging artifact this emulator
produces.

| Address | Contents |
|---|---|
| `$FE00`–`$FE60` | IM2 vector table + trampolines, copied from ROM `$01FB` at `$01EB` |
| `$FE04`/`$FE05`/`$FE06` | `JP nn` trampoline for vector `$00`; the operand is runtime-patched (`$0737` → `$0752`, `$0856` → `$0883`, `$08C6` → `$08EB`) |
| `$FE0A` | Device dispatch table (2 static entries; never grows from IPL code alone) |
| `$FE19`/`$FE45`/`$FE59`/`$FE5E` | `sub_0333h`'s own request blocks; `$FE45` status `$00` → `JP $1000` |
| `$FE83`–`$FE93` | 17 bytes copied from `(request descriptor)+3` at `$047B` |
| `$FE88` | Sectors per track (feeds FORMAT's `SC` and the format DMA length) |
| `$FE89` | `& 7` = sector-size code N (byte count = 128 << N) |
| `$FE95` | Device-table lookup key |
| `$FE9A` | Sector count for the transfer |
| `$FE9C`/`$FE9D` | **DMA Port B address — the transfer's RAM buffer** |
| `$FE9E` | Pointer to the current request descriptor |
| `$FEA0` | PIO port A interrupt vector |
| `$FEA1` | DMA interrupt vector (= `$FEA0` + 2) |
| `$FEA2` | **Length-prefixed µPD765 command buffer**, streamed to port `$15` |
| `$FEAC` | µPD765 result-phase buffer |
| `$FEB3` | DMA register-load template (20 bytes at `$FEB4`), streamed to port `$16` |
| `$FEC9` | Runtime-built trampoline target (`JP $FEC9` at `$07C0`) |
| `$FED5` | The flag the two famous busy-waits (`$06C6`, `$0798`) spin on |
| `$FED6` | Drive-mode flag (selects `$52`/`$56` vs `$42`/`$46` on port `$11`) |
| `$FED8` | Shadow of PIO port A output data (`$F0`; `OR $08`/`OR $0C` before `OUT ($10)`) |
| `$FEDD` | µPD765 command opcode for the pending operation (`$06` read / `$05` write / `$0D` format) |
| `$FF26` | Saved SP — interrupt handlers exit via `LD SP,($FF26)` + `JP`, **not** `RET`/`RETI` |

## Useful CP/M-era addresses (CBIOS / SYSPBI, `$E200`-`$FFFF`)

All read out of a live `--dump-ram` image with `tools/disasm_ram.sh` — the
on-disk `.phi` files are sector-interleaved, so the `org 0` listings in
`../Disk Images/disassembly/` cannot be used for addresses.

| Address | Contents |
|---|---|
| `$E200` | CP/M 2.2 CBIOS jump table (BOOT/WBOOT/CONST/CONIN/CONOUT/LIST/...) |
| `$E46C` | **CONST** - returns 0 unless `($E551)` != `$FF` and `($E553)` != `($0020)` |
| `$E48C` | **CONIN** - `CALL $E46C` / `JR Z` until CONST reports a character |
| `$E4C3` | **CONOUT** - the memory-mapped video path (start here for T27) |
| `$E54F` | Console-read request block: `[id][?][status][?][byte]`, status `$FF` = pending |
| `$EAC0` | Generic "post a request, then spin until its status changes" (the old ISSUE-5 wait at `$EB02` is its tail) |
| `$EB5C` | Event-slot table, 3-byte records `{id, ptr_lo, ptr_hi}` |
| `$EBA0` | Copies a request's count + destination into a driver's state block |
| `$EBAD` | Start a buffered read: drain the ring first, else record the request as pending and return `$FF` |
| `$EBDD` | IM2 vector-slot allocator - returns `($EC9C) + BC` |
| `$EBE2` | **`EI / RETI`** - how every CBIOS handler releases the daisy chain |
| `$EBE5` / `$EBFB` | Circular-buffer push / pop. Header is `[count][write_idx][read_idx]`, 32 bytes of data at `+3` |
| `$EC10` | Request-completion signal (`$EC17`'s `INC (HL)`) |
| `$ECDE` | CTC channel 3 (keyboard) init - vector base to `$00`, then `$C5`/tc=1 to `$03` |
| `$ED02` | Keyboard ring-buffer init: zeroes the 3-byte header at `$ED8F`. **Runs late** - anything strobed in before this is discarded (see `keyboard.h`) |
| `$ED2A` → `$ED2D` | **CTC channel 3 ISR** - one `IN A,($06)`, push to the ring at `$ED8F` |
| `$ED8B` | Console-read state: `[flags][count][dest_lo][dest_hi]`, flag bit 0 = read outstanding |
| `$ED8F` | Keyboard ring buffer (header + 32 bytes) |
| `$EE1C` | CTC channel 2 (clock) init - `$D5`/tc=1 to `$02` |
| `$F37F` → `$F382` | **CTC channel 2 ISR** - increments the 24-bit tick counter at `$F436` |
| `$F436` | 24-bit real-time tick counter (50 Hz) |
| `$F546` | Baud-rate table, 7 x `{control, time constant}`; see T18 |
| `$F597` → `$F59A` | **CTC channel 0 ISR** - serial transmit bit clock |
| `$F669` → `$F66C` | **CTC channel 1 ISR** - serial receive bit sampler |
| `$F727` | Baud index into `$F546` (5 = 1200 baud) |
| `$F731`/`$F732` | Live copy of the selected baud table entry |
| `$F733` | Serial flags; bit 6 = "check the port `$05` bit 6 handshake" |
| `$F73D` | Serial receive shift register |
| `$FF90` | CBIOS IM2 table at `I=$FF`: `$FF0C`/`$FF15`/`$FF03`/`$FEFA` for vectors `$90`/`$92`/`$94`/`$96` |
| `$FF0C`/`$FF15`/`$FF03`/`$FEFA` | Bank-switching stubs that self-patch the `JP` at `$FF51` with the real handler |

## Known-good invariants to assert in the harness

Scoped to **while the IPL still owns memory** — CP/M replaces page zero
and the IM2 table wholesale, so all three of these stop applying the
moment `SYSPBI` takes over. Any harness assertion must be scoped the same
way or it will fire on a correct boot.

- `$0000`–`$0002` must always be `C3 00 01` (`JP $0100`) and `$0003`–
  `$0005` must be `C3 DA 02` (`JP $02DA`) whenever port `$05` bit 3 is
  clear. Anything else means memory corruption.
- Register `I` must be `$FE` before any IM2 interrupt is delivered.
  **IPL only** — CBIOS builds its own table at `I=$FF`, which is correct
  and is why `machine.c` reads `I` live rather than assuming a page.
- Port `$14` must read exactly `$80` when idle (ROM `$0498` compares for
  equality, not a bit test).

Once CP/M is up, the equivalent invariants are (all four are asserted by
`make test`):

- `$0000`–`$0002` = `C3 03 E2` (CBIOS warm-boot vector) and `$0005`–
  `$0007` = `C3 06 D4` (BDOS entry). Nothing a stray NOP-sled can fake —
  see T24.
- `$E200`–`$E20F` is CP/M 2.2's standard BIOS jump table
  (BOOT/WBOOT/CONST/CONIN/CONOUT/LIST/PUNCH/READER), all `JP nn`.
- `I` = `$FF`, IM 2.
- The screen reads `Philips P2500` / `58K CP/M Ver. 2.2` / `A>`, and typing
  `dir\r` lists PIP, SYSGEN, SYSCBI, SYSLOAD, SYSPBI and SYSCPM — the six
  files the disk image actually contains.
- Every interrupt request is acknowledged exactly once. The exit report
  prints `requests/acknowledged` per device; the two numbers diverging means
  the daisy chain is dropping something (which is the whole of T17).
