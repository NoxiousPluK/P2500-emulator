# TODO — P2500 emulator

Written for a fresh session with no prior context. Read this **before**
`README.md`: large parts of that file describe a hardware model that has
since been shown to be wrong, and following its "next steps" will waste
time. `ROADMAP.md` has the longer-term shape; this file is the work queue.

Everything below is derived from evidence already in this repository — the
boot IPL EPROM dump, the CP/M `SYSPBI.PHI` disassembly, and the disk
images. **No new hardware measurements are needed to do any of it.**

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
  is gone. **Not yet reached in practice** — a live `--disk` run gets as
  far as PIO/FDC setup (mode config, `SPECIFY`, `SENSE INTERRUPT STATUS`,
  interrupt-enable) and then deadlocks in the `$06C6` busy-wait on `$FED5`
  *before* any `RECALIBRATE`/`SEEK`/`READ DATA` command is ever written to
  port `$15` — so no interrupt ever arrives to set `$FED5`. `$4A00` (the
  phase's actual success criterion) is still open; whatever ROM logic is
  supposed to run between "interrupt enabled" and issuing `RECALIBRATE`
  needs tracing next (T8's landmark-gating and a `P2500_TRACE_FROM`/`_TO`
  window around that point are the way in).

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

- [ ] **T16. Add a CTC at `$00`–`$03`** — not needed for disk boot, but
  needed the moment you want the keyboard or printer, both of which are
  bit-banged against CTC timing (HWTEST V100: port `$04` TX, port `$06`
  RX, 9600-8N-2).

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

- **ISSUE-3: a live run now gets all the way to programming the DMA
  correctly for READ DATA, then deadlocks *before* the actual `READ
  DATA` opcode is ever written to port `$15` — root cause found, fix
  identified but not yet applied.** Found immediately after ISSUE-2's fix
  let the boot run far enough to reach this point for the first time.
  Original write-up here proposed two candidate fixes inside `fdc.c`
  (narrow `CB` to `COMMAND`-phase-only, and/or let a fresh command byte
  abandon a stale unread result); `Datasheets and manuals/NEC UPD765.PDF`
  (the *real* µPD765 datasheet — `NEC UPD765C.PDF` is mislabeled and is
  actually a µPD780/Z80-clone CPU datasheet with no FDC content at all,
  worth renaming/flagging) rules **both of those out directly**:

  > "It is important to note that during the Result Phase all bytes
  > shown in the Command Table must be read... The µPD765 will not
  > accept a new command until all seven bytes have been read."

  This is explicit, unambiguous, and confirms `fdc.c`'s current behavior
  (refusing a new command while a result byte sits unread) is *correct*,
  real hardware fidelity - not a bug to soften. So the real question
  isn't "should fdc.c tolerate this," it's "why does the ROM leave that
  byte unread when a real board evidently boots fine."

  Tracing the exact instruction that never gets executed answers it. The
  ISR that issues this unread SENSE INTERRUPT STATUS is at `$0798` -
  reached as the **third** delivery of TODO.md ISSUE-1's synthetic
  "post-reset unsolicited interrupt" (budgeted at 4, one per PIO-arm
  event). Disassembling `$0798`:
  ```
  $0798  LD ($FF26),SP / LD SP,$FF26 / CALL $0853
  $07A3  LD A,$73 / DI / OUT ($12),A / EI      ; PIO int off, then on
  $07A9  CALL $0A87                             ; <- streams SIS, direct
  $07AC  LD A,($FEAC) / AND $C0 / CP $C0 / JR Z,$07C3
  $07B5  LD A,$01 / LD ($FED5),A / ... / JP $FEC9
  $07C3  ... / JP $03D9
  ```
  `$0A87` (`sub_0a87h`) is the **raw command streamer only** - it writes
  bytes out, full stop, with no result read anywhere in its body (verified
  by full disassembly: `PUSH AF/BC/HL`, `CALL $0AC2` wait-not-busy,
  stream loop, `POP HL/BC/AF`, `RET` - nothing touches port `$15` for
  input). Contrast with `$0883` and `$0752`'s ISRs, which both `CALL
  $0A78` (`sub_0a78h`) instead - the higher-level wrapper that streams
  *and then reads back* the result via `sub_0aa0h`. `$0798` is simply
  built differently: it re-issues a SENSE INTERRUPT STATUS as a bare
  nudge and inspects `($FEAC)` - the *previous* result, from whichever
  earlier SIS last wrote there - never the one it just issued. Nothing
  downstream of `$0798` ever reads the fresh result either.

  Put together: `$0798` is not a generic "drain the next pending startup
  interrupt" handler at all, unlike `$0883`/`$0752` - it's a
  differently-shaped handler for what's almost certainly a **different
  real interrupt source** (a drive-ready or timing-related event, given
  it specifically checks `($FEAC)` for the Drive Not Ready code `$C0`),
  and it was only reached here because ISSUE-1 fix 2's budget (4, generic,
  fired on every PIO-arm event) doesn't distinguish "another one of the
  same post-reset batch" from "a structurally different later event."
  The first two synthetic firings (→ `$0883`, → `$0752`) are the ones
  this ROM's own structure actually confirms need to happen this way
  (see ISSUE-1); this third one is very likely the fix 2 approximation
  overreaching past where it's warranted.

  **Not yet applied**: lowering the fix-2 budget from 4 to 2 is the
  obvious next experiment, but it only trades this hang for an earlier
  one (`$06C6`'s third wait, now fed by nothing) unless whatever *real*
  event `$0798` is meant to respond to gets modeled too - and this
  project doesn't yet know what that event is. Worth investigating
  alongside T4's own open note (confirm which PIO bit - PA0 or PA1 -
  actually carries the FDC's `INT`; `$0798`'s trigger may be a related,
  still-unconfirmed wiring question) rather than guessed at blind.
  Status: open, root cause pinned down precisely (the exact unread byte,
  the exact ISR, and the exact reason ISSUE-1 fix 2 over-fires into it);
  the actual fix needs a correct model of whatever event `$0798` expects,
  not a change to `fdc.c`.

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

## Known-good invariants to assert in the harness

- `$0000`–`$0002` must always be `C3 00 01` (`JP $0100`) and `$0003`–
  `$0005` must be `C3 DA 02` (`JP $02DA`) whenever port `$05` bit 3 is
  clear. Anything else means memory corruption.
- Register `I` must be `$FE` before any IM2 interrupt is delivered.
- Port `$14` must read exactly `$80` when idle (ROM `$0498` compares for
  equality, not a bit test).
