# P2500 emulator

> ## ⚠ Read `TODO.md` before this file
>
> **Most of the hardware model described below has since been shown to be
> wrong**, and the "Open finding" / "Follow-up" / "Reaching READ DATA"
> sections narrate a long investigation into a chip that is not on this
> bus. In short:
>
> - Ports `$12`/`$13` are a **Z80A-PIO**, not a Z80-CTC. `src/ctc.c` and
>   every tuning constant in it are modelling the wrong device.
> - Port `$16` is the **Z80A-DMA** — the missing `READ DATA` data path.
>   `fdc.dest_ram` is never assigned anywhere in the tree, so `READ DATA`
>   cannot deliver a byte.
> - `CALL $3019` is this ROM's own **relocated RAM test**, not a disk
>   handoff. `main.c`'s `$3019` intercept invents a disk read that never
>   happens, and the boot sector belongs at `$1000`, not `$1100`.
>
> All three are provable from the ROM bytes and are written up with their
> evidence in **`TODO.md`**; **`ROADMAP.md`** has the plan. What is still
> true and valuable below: the build/run instructions, the credits under
> "Why this exists", and the SESAM byte-exact regression test under
> "Validation".
>
> ### Where the emulator actually is (2026-09-27)
>
> **CP/M 2.2 boots to the `A>` prompt and runs typed commands.** All three
> misidentifications above are fixed, and so is the interrupt model: the
> vendored CPU core's single pending-interrupt slot was silently discarding
> whole devices' interrupt streams, and `src/intctl.c` replaces it with a
> real IM2 daisy chain (`TODO.md` T17). Try it:
>
> ```
> make test        # 17 checks, ~8 s - keep this green
> ./p2500-emu --disk "../Disk Images/extracted/P25K_B/P25K_B.raw" \
>             --max-steps 20000000 --type 'dir\r' --dump-vram /tmp/vram.bin
> ```
>
> Current flags: `--rom --disk --sesam --type --type-at --type-after
> --swap-at --max-steps --verbose-io --peek --poke --watch --count --break
> --dump-vram --dump-ram --no-stuck-detect`. Disks can be changed mid-run,
> which CP/M handles the way it does on real hardware — swap, then Ctrl-C at
> the prompt to force a warm boot and re-read the directory:
>
> ```
> ./p2500-emu --disk "../Disk Images/extracted/P25K_B/P25K_B.raw" \
>   --max-steps 120000000 \
>   --type-at '4000:dir\r' \
>   --swap-at '11000:../Disk Images/extracted/P25TEST/P25TEST.raw' \
>   --type-at '13000:\x03' --type-at '20000:dir\r' \
>   --dump-vram /tmp/vram.bin
> ``` `tools/disasm_ram.sh` disassembles a `--dump-ram`
> image at its real addresses, which is the only way to read the CP/M
> system files (the on-disk `.phi` files are sector-interleaved, so the
> listings in `../Disk Images/disassembly/` have no usable addresses).

*A from-scratch emulator for the Philips P2000B/P2500 CPU card.* Named
"P2500" for brevity and to stay distinct from the unrelated P2000T/P2000M/
P2000C machines (a different, if related, product family - see "Why this
exists" below for how one of their emulators informed this one) - the
P2000B and P2500 are the same hardware under different case colors
(`../P2500-general-findings.md`), so one name covers both.

Built to continue the dynamic-emulation work in the parent
research project's `ROM Dumps/CPU-Card-Boot-EPROM/emulation/` once that
work hit a hard limit: the Python `z80` package used there has no clean way
to inject real IM2 interrupts, and the ROM's own floppy driver
(`sub_0333h`/`sub_091Ah`, see `../ROM Dumps/CPU-Card-Boot-EPROM/
disassembly/findings.md`) turns out to depend on them for real.

## Why this exists, and why not just patch the Python harness

- **Z80 core**: [`superzazu/z80`](https://github.com/superzazu/z80) (MIT),
  vendored in `src/vendor/superzazu_z80/`. Chosen over continuing to fight
  the Python `z80` package's binding because it exposes a single clean
  call, `z80_gen_int(z, vector_byte)`, to fire a real maskable interrupt at
  the exact moment we want - no global state, no polling callback. Also
  plain 8-bit port numbers (`port_in(z80*, uint8_t)`), avoiding a gotcha
  the earlier Python work had to work around (that core exposed the real
  Z80 16-bit port bus, including the accumulator's high byte).
- **FDC design informed by [`ifilot/p2000m-emulator`](
  https://github.com/ifilot/p2000m-emulator)**: that project emulates the
  P2000M, a real sibling machine from the same Philips engineering
  lineage with a genuine uPD765-based floppy driver, and ships an
  excellent primary-source reference doc (`P2000M_FLOPPY_CONTROLLER.md`,
  transcribed from a real Philips Field Support Manual) plus a clean,
  complete `P2000Fdc` C++ class. **Not reused verbatim** - the P2500's
  ports, control-latch bits, and disk geometry are all different from the
  P2000M's, and this project's own reverse-engineered ROM command tables
  were the actual source of truth for `src/fdc.c` - but the P2000M
  reference document independently confirmed this project's own command
  decode (same command family, same order: Sense Interrupt Status ->
  Specify -> Recalibrate, then Seek) and the phase-state-machine design
  approach. Their bundled `p2000.rom` was checked byte-for-byte against
  this project's own P2500 IPL dump: **completely different firmware**
  (35/4096 bytes coincidentally match - statistical noise), as expected
  given the different machine class - no shared ROM content.

## Status

Headless only so far, per plan - a CLI harness (`p2500-emu`), no display.
It reaches real, previously-unreachable territory: with a real disk image
attached, the ROM's floppy driver now issues genuine uPD765 commands
(`SPECIFY`, `SENSE INTERRUPT STATUS`, `RECALIBRATE` confirmed so far) through
the real port `$14`/`$15` model - something the earlier Python harness's
blind `$FF`-stub could never produce. Execution now reaches the loaded disk
sector's own `CALL $0003` dispatch and, repeatably, the leading candidate
`READ DATA` handler itself (`$04D5`) - no `READ DATA` command issued yet.
See "Reaching READ DATA" below for the current state and what's still
blocking it.

Visual output works and is validated: `--dump-vram` + `tools/render_vram.py`
reproduces the boot-screen render from the Python-era work byte-for-byte
identical (see "Validation" below) - same pipeline, now backed by a real C
core instead of a Python one.

## Building

```
make
```

Needs a C11 compiler; no other dependencies (the Z80 core is vendored, no
external libraries).

## Running

```
./p2500-emu --rom roms/ipl.bin
./p2500-emu --rom roms/ipl.bin --disk "../Disk Images/extracted/P25K_B/P25K_B.raw" --verbose-io
./p2500-emu --rom roms/ipl.bin --sesam roms/sesam_banner_test.bin --dump-vram /tmp/vram.bin
python3 tools/render_vram.py /tmp/vram.bin /tmp/boot_screen.png
```

Flags: `--rom PATH` (default `roms/ipl.bin`), `--disk PATH` (attaches a raw
disk image for the `$3019` intercept and real `READ DATA` commands),
`--sesam PATH` (attaches a raw byte stream to the SESAM port - use this to
test the bootable-cartridge mechanism, see `roms/sesam_banner_test.bin` for
a working example that runs the ROM's own banner-print routine),
`--dump-vram PATH` (writes the 16KB video RAM region to a file after the
run stops), `--max-steps N` (default 2,000,000), `--verbose-io` (logs every
I/O port access, very noisy).

## Validation

`roms/sesam_banner_test.bin` is the same synthetic bootable-SESAM-cartridge
stream the Python-era `emulate_sesam_cartridge.py` used (16-byte header +
a 7-byte payload that calls the ROM's own `$0174` banner-print routine
against its own real banner table, then halts). Running it through this
harness and diffing the resulting `--dump-vram` output against the
Python-era `vram_after_banner.bin` gives a **byte-for-byte identical**
result - the new core, FDC/SESAM/CRTC port models, and memory map all
agree with the previously-validated behavior.

## Open finding: the CTC hypothesis is confirmed mechanically, but one more layer of indirection remains

With a real disk image attached, the floppy driver gets far enough to
issue genuine `SPECIFY` and `SENSE INTERRUPT STATUS` commands, then gets
stuck in `sub_06c6h` (`$06C6`-`$06CD`) - a tight busy-wait entered right
after `EI`:

```
$06C6  LD A,($FED5)
$06C9  CP 1
$06CB  JR NZ,$06C6
$06CD  RET
```

**A real Z80-CTC model (`src/ctc.c`, ports `$12`/`$13`) is now implemented
and confirmed working end-to-end**, not just hypothesized:

- The observed byte stream to port `$12` (`$FF,$03,<computed>,$37,$FC`)
  decodes exactly as real Z80-CTC control-word/time-constant pairs (each
  control byte has bit 0 set; bit 2 requests a following time-constant
  byte, per the real chip's protocol) - confirmed by implementing that
  literal state machine and watching it correctly consume every byte with
  no leftover/misaligned bytes.
- Per real CTC convention, a byte with bit 0 clear arriving outside a
  time-constant sequence is the interrupt vector, needed only for channel
  0 (other channels inherit `vector + 2*channel`). The `<computed>` byte
  (sourced from RAM `$FEA0`, itself populated by a `$FE0A` device-ID table
  lookup earlier in the ROM) turned out to be `$00` at runtime - genuinely
  bit-0-clear, correctly recognized as a vector byte by the model without
  any special-casing.
- Firing that interrupt via `z80_gen_int()` **is delivered correctly and
  instantly** - confirmed by reading `superzazu/z80`'s own `z80_step()`:
  it runs the current opcode, then calls `process_interrupts()` in the
  *same* call, so a `z80_gen_int()` triggered from a port-out callback
  mid-instruction is serviced before the next fetch, no extra step needed.

**So why doesn't it unblock the busy-wait?** Vector `$00` (and channel 1's
inherited `$02`) resolve, via the real `(I<<8)|vector` IM2 table lookup
against the ROM's own static IM2 template (`I`=`$FE`, copied from ROM
`$01FB`-`$025B` to RAM `$FE00`-`$FE60` at boot - see `../ROM Dumps/
CPU-Card-Boot-EPROM/disassembly/findings.md`), to `$FE04`/`$FE07` - and
both of those addresses hold nothing but `JP $03DB`, a trivial `EI`/`RETI`
no-op. The interrupt round-trips through it instantly, which is exactly
the "nothing visibly changes" behavior observed. Confirmed directly: none
of the 97 static template bytes anywhere encode a pointer to either of the
two genuine ISR-shaped routines this ROM contains (`$03E3` and `$0752` -
the latter is the one that actually sets `$FED5`, found via `grep` for
every instruction that writes it).

**Refined next step (superseded by the follow-up below, kept for the
record)**: the gap is no longer "is there a CTC" - it's "why does the
device-ID lookup that produces this vector byte resolve to a value whose
IM2 slot was never patched with a real handler address." That's the same
`$FE0A`/`$FEA0` device-table-lookup chain flagged elsewhere in the
disassembly findings as not fully traced (`sub_0949h`'s bit-shuffling
computation over `$FE8D`-`$FE8F`).

## Follow-up: the `$FE8D`-`$FE8F` chain traced fully - it's SPECIFY timing data, not the vector

Traced by hand (cross-checked against a live memory dump at the stuck
point, `--verbose-io`'s new `$FE00`-`$FEFF` dump): `$FE8D`/`$FE8E`/`$FE8F`
turn out to hold `$0C`/`$12`/`$0F` - raw device-characteristic bytes that
`sub_0949h` bit-shuffles into the **`SPECIFY` command's SRT/HUT and
HLT/ND timing bytes** (confirmed: the computed result, `$30`/`$24`, is
exactly what the FDC model logs as `SPECIFY`'s arguments). Not related to
the interrupt vector at all - that's a separate, more direct lookup
(`$FE95` -> `$FE0A` table -> `$FEA0`). So the user's "not finding the
security cartridge" hypothesis doesn't hold *literally* here - the SESAM
check happens once, much earlier in cold boot, fully independent of this
later FDC/CTC setup (confirmed empirically too, see "Checked: this
blocker is disk-independent" below, run the same day) - but it turned out
to be right in spirit: exhaustively grepping the whole ROM confirms
**`$FE0A` (the dispatch table's own entry count) and `$FE95` (its lookup
key) are never written anywhere in this 4KB image** - the table
genuinely never grows past its 2 static placeholder entries from IPL code
alone. Real handlers have to come from somewhere external to this ROM,
which is the same shape of idea as "needs an external cartridge/disk" even
though the specific mechanism (SESAM) isn't the one gating this
particular wait.

**The real mechanism, found by hand-tracing the actual runtime-patched
trampoline**: `l0737h` (called from `sub_0333h` right before the busy-wait
begins) does `LD HL,$0752 / LD ($FE05),HL`, patching address `$FE05`
(which `superzazu/z80`'s IM2 dispatch reads as the *operand* of a fixed
`JP` opcode living at `$FE04` - confirmed by reading `z80_step()`'s
`call(z, rw(z, (i<<8)|int_data))`, which fully resolves and calls the
target in one step) from its default "do nothing" value (`$03DB`, `EI`/
`RETI`) to the real handler at `$0752` - which is the routine that
actually sets `$FED5`. **So vector `$00` was correct all along** - the
CTC model just fired its one-shot interrupts *before* `l0737h` had a
chance to patch the target, and then never fired again.

**Fix**: the CTC model now re-fires on a later software re-trigger pattern
(a reset-only control word on an already-armed channel - real hardware
sends exactly this shape of follow-up write), rate-limited by a step-count
cooldown (`P2500_CTC_RETRIGGER_COOLDOWN_STEPS`, currently 10) rather than
firing unconditionally (which causes reentrant chaos - the same control
word gets re-written from *inside* the handler this fires, before it can
finish) or firing exactly once (which resolves the first busy-wait but
stalls identically at a second one further along, `$0798`-`$07A9`, a
near-identical ISR-shaped routine gating a second, distinct wait). This
value is a pragmatic tuning knob, not reverse-engineered ground truth -
documented as such in `src/ctc.c`.

**Result, confirmed clean (no memory corruption - see the note below on
why that mattered)**: `$0752` gets reached and legitimately sets `$FED5`
for the first time ever. The `$3019` disk intercept is now one-shot
(`main.c`) - a real bug found and fixed along the way: with interrupts
firing this much more often, execution revisited `$3019` itself down an
unintended path, and the intercept was blindly overwriting **live RAM**
(observed: 256 bytes of disk sector dumped onto the `$FE0A` dispatch-table
region) rather than a real one-time hardware handoff. Real hardware
wouldn't have this problem (no software stands in for `$3019`'s actual
memory-mapped content there) - it's an artifact of this harness's
high-level-emulation shortcut, now guarded against.

**Honest limits of that result**: past this point, execution continued
into a second `$06C6`-shaped busy-wait (`$0798`-`$07A9`) that the same
cooldown heuristic didn't cleanly resolve - larger cooldowns (25+) let
execution run far further address-wise, but with only one real disk sector
loaded, everything beyond `$1100`-`$11FF` is zero RAM, and a `NOP`-sled
through zero memory produces a large, ever-growing "distinct addresses"
count without being meaningful execution. That's indistinguishable from
real progress using address-diversity alone - this was flagged as
unconfirmed, not claimed, at the time.

## Follow-up: the pulse-driven CTC model - a real structural fix, and a real structural correction

Replaced the step-count cooldown heuristic entirely with genuine countdown
semantics (`src/ctc.c`/`ctc.h`, fully rewritten): each counter-mode channel
now decrements on simulated clock pulses (derived from the Z80 core's own
T-state counter, `z80.cyc` - a real, not invented, timing source) and,
matching real Z80-CTC behavior exactly, **auto-reloads and keeps counting
after firing** rather than stopping - free-running, not one-shot. A
software reset (control word, bit 1 set, no time-constant following)
correctly stops a channel until it's re-armed, matching the real chip. The
pulse rate itself (`P2500_CTC_TSTATES_PER_PULSE`, defaulted to 8 - a 4MHz
Z80 clock over the P2000M reference's documented 500kHz FDC write clock)
is a sourced-but-unconfirmed assumption, clearly flagged as such in code -
this project hasn't identified the literal signal wired to the CTC's
input pin.

**This surfaced a real correction to last session's understanding, not
just a cleaner implementation.** With genuine countdown timing (tested
across several plausible pulse rates, 8 through 200 T-states/pulse),
**channel 0 gets stopped by a software reset write a small, fixed number
of instructions after being armed - consistently, at every rate tested.**
Since the reset always arrives long before 252 pulses could plausibly
complete regardless of rate, this is a rate-independent finding, not a
tuning artifact: **channel 0 was never intended to complete via a real
timeout in this code path.** The earlier session's "success" (reaching
`$0752`) came from an ad-hoc heuristic that fired *on* the reset write
itself, which happened to unblock the right routine for the wrong
mechanistic reason.

**Reading `sub_071bh` (not examined before) explains why, and reframes the
whole picture.** It's a pure RAM-state check - `LD A,($FED6) / CP 0 / ...`
comparing a saved pointer (`$FED1`) against a fixed constant (`$07D0`) -
completely unrelated to CTC/FDC timing. Its result gates which of two
branches `sub_0333h` takes: reaching `l0737h`/`$0752` (the path this
project traced in detail) requires a specific, uncommon condition
(`$FED6≠0` *and* `$FED1==$07D0`) - **it's the exceptional branch, not the
main one.** The default/common branch goes to `$0798` instead, which sets
`$FED5` through plain synchronous code (a direct FDC status check via
`sub_0a78h`, no interrupt-vector patching involved) once it's allowed to
run uninterrupted. This means the *real* main-line success path likely
doesn't need channel 0 to fire at all - it needs `$0798`'s own straight-line
sequence to complete without being repeatedly interrupted mid-flight,
which is exactly what the earlier session's over-eager re-firing was
preventing.

**Update - fixed, and it works.** Two changes on top of the pulse-driven
model:

1. **No-op-target suppression** (`machine.c`'s `ctc_interrupt_trampoline`):
   before delivering any CTC interrupt, follow the real IM2 indirection
   chain (vector -> table slot -> the `JP nn` trampoline stored there ->
   its operand) and check whether it currently resolves to the ROM's
   fixed "do nothing" stub (`$03DB`, `EI`/`RETI`). If so, skip delivery
   entirely rather than spending a real interrupt round-trip on it. This
   is what stops a channel with nothing useful to do (like channel 1's
   target, which nothing in this ROM ever patches) from crowding out
   delivery of whatever the code is actually waiting on.
2. **Stopped honoring the software-reset-as-permanent-stop behavior.**
   Traced why it mattered: in every observed sequence, channel 0's reset
   write arrives *before* whichever runtime patch (`l0737h`'s `$0752`, or
   a second, newly-found patch site `$0856`'s `$0883` - see below) has
   even been installed yet. Stopping the channel there, as real hardware
   would, means it can never usefully fire again unless something later
   re-arms it - and nothing in any traced flow does. Letting it keep
   counting and auto-reloading in the background, made safe by the no-op
   suppression above, reaches a real target once one exists. A genuine,
   flagged deviation from real hardware's reset semantics, justified by
   what the traces actually showed, not assumed.

**Result, confirmed via real landmark hits, not just address-diversity**:
`$0752` reached again (as before), then genuinely new territory -
`RECALIBRATE` issued for the first time in this project's entire
emulation history, clean repeated `$03E3`/`$FEC9` interrupt-dispatch
cycles (not reentrant chaos), and - the strongest evidence yet - **the
`$0003` dispatch vector landmark fires twice**, meaning execution
genuinely reached back into the *loaded disk sector's own bootstrap code*
and its `CALL $0003` request, the mechanism this project traced all the
way back in the SESAM-cartridge work. That's confirmed by a specific,
known landmark address being hit, not inferred from how many distinct
addresses got visited.

Along the way, found a *second* runtime-patched handler this project
hadn't seen before: `$0856` (reached via `sub_071bh`'s Z-branch, the
*common* path, unlike `l0737h`/`$0752`'s exceptional one) patches the
same `$FE05`/`$FE06` slot to `$0883` instead, then issues `SPECIFY` for
real and calls into `sub_0a78h`. `$0883` itself unconditionally sets
`$FED5=1` after its own `SENSE INTERRUPT STATUS` check - no FDC-status
gating the way `$0752` has, which fits it being the plain/expected
completion path rather than an edge case.

**Honest caveat, unchanged from before**: execution continues well past
these confirmed landmarks into address ranges (`$E000`+) this project
hasn't independently verified, and no `READ DATA` command was ever issued
in this run - so nothing populated that memory with real disk content.
That specific deep stretch is not claimed as confirmed meaningful
execution, the same caution as always. What *is* now confirmed, concretely
and repeatably, is genuine progress through real ROM logic as far as the
`$0003` dispatch vector - a substantially stronger result than anything
reached in this project's emulation work before it.

**Checked: this blocker is disk-independent, confirmed empirically, not
just assumed.** This project's disk collection actually contains three
distinct boot-sector families, not one - `../Disk Images/extracted/`
shows the CP/M group (`P25K_B`/`P25K_G`/`P25K_S`/`P25TEST`/`P2k5_CPM`, all
byte-identical, the one used above), a UCSD p-System group (`P2k5_LOGIC`/
`P2k5_TKS`, genuinely different bootstrap code), and `p25k_prg` (different
again). `P2500GAM`'s sector 0 is all `$E5` - likely blank/unformatted, not
a real boot disk. Re-running against one of each: **identical trace, step
counts, and final state every time.** Makes sense once you see where the
block actually is - it's entirely inside `sub_0333h`'s own ROM-resident
CTC/FDC setup, reached and stuck *before* execution ever reaches the
loaded sector's own code at `$1100` - so which disk is attached genuinely
can't matter for this specific problem. Confirms the earlier reasoning
rather than just assuming it.

## Follow-up: Reaching READ DATA

Picking up from `$0003` firing twice above - the next question was whether
that dispatch actually reaches a real disk-read handler, and if not, why.

**Decoded the real dispatch table.** `l02dah`'s own gate check (byte at
`DE+1`, table at `$FE0A`, 2 entries) turns out to be a shallow pre-filter -
the *real* handler selection happens one level deeper, at the ISR prologue
`$03E3` it hands off to, which does its own lookup keyed on the byte at
`DE+0` against a real, 9-entry table at `$0422`: ID 0 → `$043E` (drive
prep, already a confirmed landmark), ID 1 → `$04D5`, ID 2 → `$04F9`, ID 3 →
`$0539`, ID 4 → `$059C`, ID 5 → `$05A1`, ID 6 → `$056B`, ID 7 → `$057D`, ID
8 → `$05A6`. Reading `$04D5`'s body against the ROM's own established
"copy descriptor bytes from `(DE)+3` to `$FE83`" pattern (the same shape
this project already confirmed feeds real `READ DATA` command construction
elsewhere) makes it the leading candidate for the real read handler -
IDs 2-8 not yet read in the same detail.

**A `--poke ADDR:HEXBYTES` CLI flag** (`main.c`) was added to seed RAM with
a synthetic request descriptor at `$1030` (matching the `LD DE,$1030` the
sector's own bootstrap issues right before `CALL $0003`) - same synthetic-
data methodology already validated for the SESAM cartridge work. First
attempt showed *zero* effect versus an unpoked run, byte-for-byte - not a
poke bug (verified via an echo-back printout), but the ROM's own repeated
cold-boot cycles re-running their RAM test and re-zeroing `$1100-$11FF`
*after* the poke landed but *before* the sector's own code, at `$1100`,
ever got to read it. Fixed with an unconditional refresh: whenever PC
reaches `$1100` and its contents don't match the source disk buffer, it
gets re-supplied on the spot, regardless of how execution got there. This
is what finally made real sector-triggered `CALL $0003` dispatch happen at
all.

**Then a very different problem showed up: the ROM's own vector table was
getting overwritten during execution.** A watchpoint on `$0003` (the same
address that should permanently hold `JP $02DA`, confirmed intact by an
early sanity check) caught it changing mid-run, with no code of this
project's own anywhere near it. Traced to two distinct mechanisms, both in
`machine.c`'s CTC/FDC interrupt trampolines:

1. **Delivering into an unpatched IM2 vector-table slot.** The existing
   no-op-target check (`$03DB`) only caught one specific stub. Two more
   unpatched states turned out to be just as dangerous: a slot still
   holding the ROM's static `$0030-$00FF` filler (`0xFF`, i.e. `RST 38h`)
   is a one-instruction self-loop - `RST 38h` at `$0038` pushes a return
   address and jumps straight back to `$0038` - so delivering there drives
   SP down through the *entire* 64KB address space over hundreds of
   thousands of iterations, spraying that return address over everything
   it passes, low ROM included. A slot resolving into genuinely
   never-written RAM (`0x00`, decoding as `NOP`) is milder but just as
   meaningless - PC free-walks through however much zeroed memory follows,
   and a later, unrelated interrupt firing mid-walk picks up from wherever
   that wander left PC, producing traces that jump to wildly different
   addresses with no semantic content. `resolve_im2_target` now refuses
   delivery for both cases, plus a third: register `I` not yet at `$FE`,
   the real, established table page this whole investigation is anchored
   on - before the ROM's own init code sets it there, `(I<<8)|vector`
   points at whatever happens to be at low/incidental addresses instead.
2. **An SP-baseline "still in flight" gate turned out to be the wrong
   model entirely.** The instinct was reasonable - stop a new interrupt
   from nesting into a handler that hasn't returned yet, since that's
   *also* capable of walking SP down into the same `RST 38h` wall - so it
   was tried: track SP at delivery time, refuse a new delivery until SP
   returns to at least that level. It livelocked real, legitimate
   execution (the CPU got stuck cycling 11 addresses forever). Tracing why
   revealed something new about this ROM: its interrupt-exit convention at
   `$0847-$0850` doesn't use `RET`/`RETI` at all - it does
   `LD SP,($FF26)`, an explicit restore from a fixed saved slot, followed
   by a plain `JP`. SP never naturally unwinds back to a delivery-time
   baseline under that convention, so any gate built on that assumption
   can only ever see "still in flight." Reverted; the fix that stayed is
   relying on the Z80 core's own native IFF1 gating (already correct -
   `process_interrupts` in the vendored core already requires
   `int_pending && iff1`) plus the `resolve_im2_target` checks above.

**The remaining piece was rate, not correctness.** Even with both fixes,
corruption still recurred at an identical step whether or not `--poke` was
present - meaning it happens early, independent of the disk-descriptor
work entirely. The mechanism: the ROM's own handlers legitimately `EI`
early (e.g. `$1102`, right before real work begins) - correct Z80 practice
on hardware where the interrupt source is paced slowly enough that this
rarely matters. This project's CTC model, deliberately free-running and
never permanently stopped by software reset (see the pulse-driven section
above), was firing far more often than that assumption holds, nesting a
new interrupt in almost every time IFF1 flipped back on, before the
previous handler had gotten anywhere near its own exit. The CTC header
comment already flagged the 500kHz-sourced pulse rate
(`P2500_CTC_TSTATES_PER_PULSE`, 8 T-states/pulse) as justified only once
real FDC data transfer begins - not during this SPECIFY/setup phase, which
is all that's happened so far. Slowing it 100x (800 T-states/pulse, still
clearly marked as an experiment in code, not a sourced hardware value)
confirmed the theory: zero corruption events, and execution reached
dramatically further - `$1000`, repeated real `CALL $0003` dispatch, and,
for the first time in this project's history, **`$04D5` itself, the
candidate `READ DATA` handler, hit five times.**

**Still open.** A second, different stack-corruption path remains: PC
still ends up inside the `$0030-$00FF RST 38h` wall around step 800K-900K
in longer runs, but reached this time via normal execution flow (something
like a stray `RET` to a bad address) rather than through the interrupt
trampoline's own target resolution - so it's outside what
`resolve_im2_target` can catch, and hasn't been root-caused yet. No
`READ DATA` command has been issued in any run to date. The regression
baseline (`sesam_banner_test.bin` + `--dump-vram`, byte-identical against
the Python-era reference) was re-checked after every change described
here and never broke.

**New debugging mechanisms added along the way**, all in `main.c`:
`--poke ADDR:HEXBYTES` (seed RAM with synthetic data before the run
starts, up to 8 regions), and `P2500_TRACE_FROM`/`P2500_TRACE_TO`
environment variables (fine-grained per-step PC/opcode/register trace over
a bounded step range - essential for catching the `$0003` corruption live
rather than just inferring it from final state).

## Files

The tree is split three ways (`TODO.md` T33). `src/core/` is the machine
model and depends on nothing but libc; `src/cli/` is the headless harness
that `make test` drives; `src/gui/` will be the SDL3 + Dear ImGui front-end
(`TODO.md` T36+) and does not exist yet. Nothing in `core/` may depend on
either front-end.

**`src/core/` → `libp2500.a`**

- `machine.{c,h}` - memory map, port dispatch, Z80 core wiring, the
  per-instruction step that advances every device and arbitrates interrupts
- `intctl.{c,h}` - the IM2 daisy chain (`TODO.md` T17). Hold-until-
  acknowledged, priority by chain position, release on `RETI`
- `ctc.{c,h}` - Z80A-CTC at ports `$00`-`$03`, T-state driven with a real
  16/256 prescaler, and per-channel CLK/TRG sources decoded from CBIOS's own
  ISRs (`TODO.md` T18/T19)
- `pio.{c,h}` - Z80A-PIO at ports `$10`-`$13` (the FDD card)
- `dma.{c,h}` - Z80A-DMA at port `$16`, a real self-describing
  register-stream parser
- `fdc.{c,h}` - the uPD765 model (ports `$14`/`$15`)
- `keyboard.{c,h}` - port `$06` keyboard in, port `$04` serial out
- `sesam.{c,h}` - the SESAM dongle / bootable-cartridge port (`$0F`)
- `vendor/superzazu_z80/` - the vendored Z80 core (MIT). One local addition,
  marked as such: an optional `on_reti` callback, without which the
  interrupt daisy chain cannot see `RETI` and so cannot model IEO release

**`src/cli/` → `p2500-emu`**

- `main.c` - the headless harness: landmark tracking, state-based stuck-loop
  detection, `--watch` / `--count` / `--break` / `--peek` / `--poke`,
  `--dump-vram` / `--dump-ram`, `--type` / `--type-after`, and the
  `P2500_TRACE_FROM`/`P2500_TRACE_TO` per-step trace
- `roms/ipl.bin`, `roms/charrom.bin` - copies of this project's own dumped
  ROMs (see `../ROM Dumps/`)
- `roms/sesam_banner_test.bin` - the validation SESAM stream, see above
- `tools/render_vram.py` - video-RAM-dump-to-PNG renderer (ported from the
  Python-era `render_boot_screen.py`, generalized to take a file argument)
- `tools/run_tests.sh` - the regression suite behind `make test`
- `tools/disasm_ram.sh` - disassembles a `--dump-ram` image at its real
  addresses; the only usable way to read the CP/M system files, whose
  on-disk `.phi` form is sector-interleaved
- `tools/imd_tool.py` - ImageDisk verifier and normalizer. `verify` reports
  whether an image is a complete dump (four of this project's nine are not -
  they were double-stepped and are missing every other track); `convert`
  writes a `.raw` indexed by sector-ID cylinder, which for a healthy image is
  byte-identical to the existing one
