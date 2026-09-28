# TODO — P2500 emulator

The work queue. `README.md` is the orientation document, `ROADMAP.md` is the
sequencing and the reasoning behind it.

> **Completed work has been removed from this file. It is in git history.**
> Rewritten 2026-09-28 (previous rewrite: `5e73169`). Recover anything with
> `git log -p --follow TODO.md`, or by task ID: `git log --all --grep 'T27'`.
> Source comments still cite the task IDs that produced them, which is why
> **open task IDs are never renumbered** even when the sections around them
> are. New work continues from T47.

---

## Where this is

**CP/M 2.2 boots in a window you can type into, and runs real applications.**
SuperCalc2 (an OEM build whose splash reads `PHILIPS P2000`) loads from
`P25K_S` and opens files; MBASIC-80 runs from `P25TEST`; `VALLEY.BAS` runs
off drive B with working screen attributes. Three of nine disk images boot.

`make test` is the proof and the guard: 69 checks, exit 1 on any failure.

```
make                 # libp2500.a, p2500-emu, and p2500-gui if SDL3 is present
make test
./p2500-gui --disk "../Disk Images/extracted/P25K_B/P25K_B.raw" \
            --disk-b "../Disk Images/extracted/P2500GAM/P2500GAM.raw"
```

What is modelled and working: the full IPL and CP/M boot, three floppy
drives, the IM2 daisy chain, CTC/PIO/DMA/µPD765, the MC6845 with a
CRTC-driven renderer, the video attribute plane, keyboard and serial input,
a styled ImGui menu bar, and a four-panel debugger behind it.

The biggest gap is now **writing to disk** (P3): nothing in CP/M's read-only
path needed it, which is why the prompt was reachable without it, and it is
what stands between this emulator and `CONFIG`, `PIP` copies and SESAM
initialization.

---

## The hardware model, as currently decoded

Every entry traces to firmware, a datasheet, or the P2219 manual. Where
something is still assumed, it says so.

| Port | Device | Notes |
|---|---|---|
| `$00`–`$03` | **Z80A-CTC** (Z8430) | `channel = port & 3`. ch0 = serial TX bit clock (TIMER, no CLK/TRG); ch1 = serial RX sampler, CLK/TRG is RXD; ch2 = 50 Hz clock strobe (**source unidentified, T29**); ch3 = keyboard byte-ready strobe |
| `$04` | Serial / printer TX | One data bit, clocked by CTC ch0 |
| `$05` | Bank latch (W) / status (R) | Write: bit 3 = EPROM out of `$0000`–`$0FFF`; bits 0–2 all clear = video DRAM at `$8000`–`$BFFF`, all set = main DRAM. The other six combinations are undecoded and trip a diagnostic. Read: bit 7 = RXD, bit 6 = TX handshake; bits 0–5 unknown, read back 1 |
| `$06` | Keyboard data | Byte-wide, one `IN` per ch3 interrupt. 7-bit ISO codes |
| `$08`/`$09` | **MC6845 CRTC** | 80×24, 12 scanlines/row, 311 scanlines/frame. Only R14–R17 read back, per the datasheet |
| `$0A` | **Video attribute latch + mode select** | Write-only. Low nibble = the attribute stored beside the next character written into the video window. **Bit 6 selects high-resolution graphics mode** |
| `$0F` | SESAM dongle | IPL-only baseline is 32 reads / 6 writes |
| `$10`–`$13` | **Z80A-PIO** (Z8420), FDD card | Mode 3 bit control. PA0 = µPD765 `INT` (**assumed, not traced**; try PA1 before concluding the model is wrong) |
| `$14`/`$15` | **µPD765 FDC** | Idle status exactly `$80`. Unit from command byte 1 bits 0–1. Read path complete; write/format decoded but unimplemented (T30) |
| `$16` | **Z80A-DMA** (Z8410) | Self-describing register stream, not a positional template |
| `$18`–`$1E` | Unclaimed | Appears in `SYS09.PHI`; most likely the 8-inch drive interface (E:–H:) |

**IM2 daisy chain** (`src/core/intctl.c`): DMA → PIO A → PIO B → CTC 0/1/2/3.
The DMA-before-PIO half is *derived*: the IPL's DMA and PIO handlers share
one saved-SP word at `$FF26` and one private stack, so only this order lets
them coexist. The CTC's position relative to the FDD card is the
conservative reading, not a measured fact.

**Video memory** is 16K words × 12 bits: an 8-bit character code the CPU
writes into `$8000`–`$BFFF`, plus a 4-bit attribute latched in port `$0A`.
Attribute bits: 0 underline, 1 low intensity, 2 reverse, 3 flash.

**Character cell** is 8×12. The character ROM's stride is 16 bytes per code;
rows 0–11 are the glyph, 12–15 are padding. Reading 8 rows truncates every
descender.

**Swapping a disk mid-run** is read-transparent and write-opaque. CP/M
re-reads the directory on every search, so a `DIR` straight after a swap
lists the new disk with no warm boot. But `CKS` in the DPB is **16**, not 0
— these drives are checksummed as removable media — so BDOS finds the
directory checksum changed and sets the drive's bit in its **read-only
vector at `$E1AD`** (the pair `$E1AF` is the login vector; the BDOS reset at
`$E086` clears both, and `$E117` is the function that reads `$E1AD` back).
That costs write access, not visibility. It has no observable consequence
until T30 lands.

**Largest loadable `.COM`: 405 records (51,840 bytes)**, on the 58K CP/M
these disks carry. 406 records gets `BAD LOAD` from the CCP. The limit is
the CCP's base at `$CC00`, not the BDOS base at `$D400` — CP/M refuses to
overwrite the CCP while loading, though a running program may use that
memory afterwards. Measured by bisection with
`tools/mk_cpm_probe.py --pad-records N`.

**Disk geometry.** The logical track number in each sector's ID field is
**physical track + 1**. Sector skew within a track is `0,2,…,14,1,3,…,15`.
`lba = (C - 1) * sectors + (R - 1)`. Up to four 5-inch drives (A:–D:);
8-inch would be E:/F: (single density) and G:/H: (double).

**`.PHI` system files** are `[load_lo][load_hi][end_lo][end_hi][x_lo][x_hi]`
followed by the body, which loads at `load`. So they can be disassembled
statically — see T50.

---

## P1 — Finish the front-end *(complete)*

The GUI's purpose is instrumentation. Every advance this project has made
came from it: `--peek`, `--count`, `--watch`, `--dump-ram`, the layout
report. A live device-state panel would have made several multi-session
chases into single glances — so it exists now, and so does its headless
twin, because a panel nothing can assert against is decoration.

What is left in this phase is T34, which is also the fourth panel.

- [x] **T39. The ImGui debugger panels.** All four are in, plus the
  headless half of each. The shared code is
  `src/core/debug.{c,h}` — a Z80 disassembler, the watch/counter/breakpoint
  tables, and the live-state lines — which both front-ends drive, so
  `p2500-emu --state` and the GUI's device panel cannot report different
  things.

  *(Settled, so it is not re-litigated: the menu bar is drawn by ImGui
  rather than being native. [SDL PR #13752](https://github.com/libsdl-org/SDL/pull/13752)
  is Windows and macOS only and still unmerged;
  [thomashope/native-menu-bar](https://github.com/thomashope/native-menu-bar)
  wires Win32 only in its SDL example and its GTK backend needs a
  `GtkWindow`. The blocker under both is Wayland: no foreign-window
  reparenting, and KDE exports no global app menu. `SDL_ShowOpenFileDialog`
  **is** properly native and is used.)*

  Done:
  1. **Device state** (F1) — the daisy chain live, the four CTC channels
     with down-counters and CLK/TRG, PIO masks, DMA registers, FDC phase
     and unit and its command/result bytes, CRTC registers and geometry,
     the attribute latch. "Copy all" emits exactly what `--state` prints.
  2. **Memory** (F2) — bank-aware, with the character and attribute planes
     as separate views, and watches. A watch names the instruction that
     wrote the byte — the poll runs between instructions, so the naive
     answer is the one *after* the write, which is what this used to
     report. Ticking `stop` makes it a watchpoint.
  3. **Disassembly** (F3) — around PC, clickable breakpoint gutter,
     Step / Step 100 / Step field. Forwards from an anchor only: a Z80
     stream cannot be decoded backwards, and a guess would look confident.
  4. **Log** (F4) — every diagnostic the core produces, now that T34 put
     them behind a callback, plus the watch hits and breakpoint stops.
     Filterable by level and by substring; warnings are amber, deliberately
     off the phosphor palette, because they are the lines saying the
     emulator declined to act. *Debug > Verbose device logging* arms the
     device switches; off by default, since they emit a few thousand lines
     per emulated second.

  The disassembler is cross-checked against `z80dasm` by
  `tools/disasm_crosscheck.py` — ~34,000 instructions per `make test` run,
  zero mismatches. Worth knowing when reading its output: where z80dasm
  renders an undecodable byte as a lone `defb`, this decoder reports what
  the CPU really does, so a DD/FD prefix on an opcode with no index form,
  and an undefined `ED`, are **two-byte instructions**. That matches
  `exec_opcode_ddfd`'s default case and is what the PC actually follows.

  The menu bar's lamps are controls as well as indicators: click to load a
  disk (right-click ejects), to pause, or to toggle capitals lock, with an
  inverted cell on hover to say they are clickable. `--mouse X,Y[,left|right]`
  drives a synthetic pointer so `make test` covers them, and the front-end
  reports its own lamp columns to the log rather than the suite pinning pixel
  positions a fourth menu would invalidate.

  Still worth adding when a chase calls for it: run-to-cursor, a
  step-over that runs past a `CALL`, and symbol names from the CBIOS tables
  so the listing reads `SELDSK` rather than `$E620`.

- [x] **T34. A log callback in the core.** Done. `src/core/log.{c,h}` is one
  `P2500Log` on the machine; every device holds a pointer to it and writes
  through `p2500_logf(level, category, …)`. The `[cat] ` prefix each message
  used to carry inline is now the category argument, and the CLI's sink puts
  it back, so **stderr output is byte-for-byte what it was** — verified by
  diffing a full CP/M boot plus `DIR` with every verbose switch on, 183 KB
  of diagnostics, against a build of the previous commit.

  The one thing that needed care: a device's `_init` must not take its own
  sink away with it. The CLI re-initialises the keyboard to install a
  keystroke queue, which silently cost four `[kbd]` lines until each `_init`
  learned to carry `log` across its `memset`. That is the project's usual
  failure mode in miniature — the messages did not error, they just stopped.

  Changed behaviour, deliberately and separately: `--verbose-io` now also
  enables `intctl.verbose`, so the harness can see the daisy chain the GUI's
  panel could already show.

  The core still has **no non-const file-scope state at all** — the expensive
  property to retrofit, already correct. Keep it that way.

---

## P2 — The rest of the video hardware *(complete)*

Both paths are complete. The character path renders through the character
ROM; the graphics path renders one bit per pixel out of the same 16 KB,
with the layout derived from the firmware rather than guessed.

- [x] **T47. High-resolution graphics mode.** Done, and every part of it
  measured rather than assumed. `ESC 3` already worked end to end before any
  of this — CBIOS writes `$40` to port `$0A` *and reprograms the CRTC* to
  64 × 64 cells of 4 scanlines, so `p2500_video_info()` reported
  **512 × 256** on its own. The documented resolution was never asserted
  anywhere; it falls out of the registers the firmware programs.

  **The layout, derived by driving CBIOS's own set-point call and reading
  back where it wrote:**

  ```
  addr = (y % 4) * 4096 + (y / 4) * 64 + x / 8      bit = 7 - (x % 8)
  ```

  Not a linear framebuffer. The MC6845's raster address is wired to address
  bits 12–13, so it selects one of four 4 KB banks and the CRTC's own memory
  address indexes within a bank. **That wiring is why CBIOS programs R9 to
  3** — four scanlines per row is all two bits of raster address can reach,
  and 4 × 4096 = the whole 16 KB = 512 × 256 bits exactly.

  Verified at both ends of both axes (x = 0, 1, 8, 16, 504, 511; y = 0–8,
  10–13, 32, 33, 255) and for all four raster addresses. Rendering is
  cross-checked between `p2500_video_render()` and `tools/render_vram.py
  --graphics`: **0 of 131,072 pixels disagree** on a full-screen diagonal.

  **Mosaic (`ESC 1`/`ESC 2`) needs nothing**, now confirmed rather than
  assumed: port `$0A` stays `$00` across both, and the character codes land
  in video RAM untranslated. Whatever code CBIOS decides to store is
  rendered through the same 256-glyph ROM, so the emulator is correct by
  construction either way.

  **Three demos exercise it** (`demos/`, `make demos`): a bouncing P2000
  wordmark, a starfield, and a Lissajous plotter. They write video memory
  directly through the port `$05` window rather than through CBIOS's
  set-point call, which is about four hundred times too slow to animate —
  so they are also an independent check on the layout above, arrived at from
  the guest's side rather than the emulator's.

  **One measured quirk of the interface**: a coordinate byte of `$09` is
  eaten by the console path's tab handling and arrives as `$20`, so a dot
  meant for row 9 lands on row 32. Every other value 0–255 passes through,
  `$0D` and `$0A` included. This is CBIOS's, not the video card's.

---

## P3 — Writing to disk

Nothing in CP/M's read-only path needed either of these, which is why the
prompt was reachable without them. They are what stands between this
emulator and running `CONFIG`, `PIP` copies, or saving from SuperCalc.

- [ ] **T30. Invert the DMA↔FDC data path, then finish the µPD765 command
  set.** `do_read_data()` hands its whole block to the DMA in one `memcpy`,
  so data flows FDC→DMA by direct call rather than the DMA pulling bytes
  through port `$15`. Writing needs the opposite direction, which means the
  transfer has to be driven from the DMA side. `WRITE DATA` (`$05A1` →
  `$0B43`) and `FORMAT A TRACK` (`$05A6` → `$0B19`) are decoded from the
  IPL's own command tables; neither is implemented.

  Two things fall out of the same change, both currently unmodelled: the
  FDC's `EXM` status bit, which can never read 1 while a whole block moves
  in one `memcpy`, and per-byte pacing of the execution phase across
  multiple `p2500_step()` calls.

  **Writing needs a policy decision first**: disk images are currently
  mapped read-only from a `const uint8_t *`. Decide between copy-on-write in
  memory (safe, loses changes), write-back to the file (dangerous with the
  only copies of rare media), or an explicit "save disk as…" — the GUI can
  make that a menu item rather than a silent behaviour.

- [ ] **T31. Multi-track reads past `EOT`.** Independent of the inversion.
  `do_read_data()` hands the DMA the whole remaining disk image as one flat
  span, so a multi-sector read running past the last sector of a track walks
  into the next track instead of terminating. CP/M's directory reads stay
  inside one track, which is why `DIR` is correct; a large sequential read
  is not.

---

## P4 — The FDC's missing drive-ready model

Two symptoms that are probably one bug. Fix the cause, not either symptom.

- [ ] **T43. Model the µPD765's drive-ready line and its polling.** A real
  µPD765 polls each drive's READY and raises an unsolicited interrupt when
  it *changes*. This emulator stands in for that with exactly two synthetic
  post-reset interrupts, permanently disabled once a real command is issued
  (ISSUE-1 fix 2, narrowed by ISSUE-3). That is fine with media present and
  is why two things hang:

  **With no disk attached, the machine sits on a blank screen forever.** It
  blocks in `$06C6`, an unbounded spin with no timeout:
  `ld a,($FED5) / cp $01 / jr nz,-5`. `$FED5` is set only by the vector-0
  ISR at `$0883`, i.e. only by a real FDC interrupt. Four PIO-A interrupts
  arrive (two synthetic, RECALIBRATE, the failed READ DATA), the result
  phase is read, a SPECIFY and a SENSE INTERRUPT STATUS are streamed —
  neither can interrupt — and it waits, with the request status at `$FE47`
  left pending so `sub_0333h` can never return and print the IPL's banner.

  **Genuinely open: whether real hardware hangs here too.** With an empty
  drive READY never changes, so a real P2500 may also sit blank until a disk
  is inserted. Settling it needs the drive's READY wiring traced or a real
  unit powered with an empty drive. Until then **do not invent an interrupt
  to make the banner appear** — that is tuning a mechanism until something
  moves.

  Already done: `READ DATA` with no media reports **NR** (ST0 bit 3, `$48`)
  per the datasheet rather than a bare `$40`. Accuracy only; it does not
  resolve the hang.

  The banner itself works — a disk that is readable but not bootable reaches
  `$013C`, prints `PHILIPS / MICROCOMPUTER / P2000/B` and halts at `$014D`.
  `make test` asserts it against `P2k5_LOGIC_deinterleaved.raw`.

- [ ] **T42. `P25K_G` blocks after its last disk read.** The one complete,
  clean image that does not reach a prompt, so this failure is ours.

  Comparing the two builds statically (possible now that `.PHI` files are
  known to be memory-mapped): P25K_G's `SYSPBI.PHI` loads at `$EB80` against
  P25K_B's `$EAC0`, so its blocked loop at `$EBC2` is **the same routine** as
  P25K_B's `$EB02`. Its device table (`$EC16` vs `$EB56`) has **6 entries
  against 8** — no ids `$02`/`$03`, i.e. two drive units rather than four —
  and the blocked slot `$EC2A` is the record shared by ids `$00`/`$01`: the
  **disk device**, not the keyboard or clock an earlier note assumed.

  The hang follows a *successful* read (`READ DATA C=47 R=13`, result all
  zeros, 82 DMA transfers), then `SPECIFY`, a `SENSE INTERRUPT STATUS` that
  correctly returns Invalid Command, and a `SENSE DRIVE STATUS` returning
  ST3 `$20` — then silence. **That is T43's shape exactly.**

  So T42 is probably an instance of T43. **Not established**: the obvious
  experiment — letting the synthetic post-reset interrupt keep firing —
  is invalid, breaking the IPL at `$0AC7` long before CP/M loads. Do T43
  properly and re-test this.

  **Corroborated externally**: the image's own author describes it as a
  *"Bootable disk with games"* and lists what is on it, so reaching a prompt
  is what it is supposed to do. The failure is ours.

  Ruled out: not the keyboard (CTC ch3 acknowledges 4 of 4, no effect), not
  SESAM (baseline access only), not the clock (ch2 fires 3451 times).

---

## P5 — Media, and what is still locked up in it

- [ ] **T45. Re-image the double-stepped disks.** `P2k5_CPM`, `p25k_prg`,
  `P2k5_LOGIC` and `P2k5_TKS` were read by a drive that stepped twice per
  track, so every second cylinder was never captured
  (`tools/imd_tool.py verify` reports it). They are the most valuable media
  this project has:

  | Disk | Holds |
  |---|---|
  | `P2k5_CPM` | **The P2219 system diskette itself** — its IMD label reads `8702 221 90021 P 2219`. `CONFIG.COM`, `CPM58`/`PBI58`/`CBI58`, `ED`, `ASM`, `DDT`, `SUBMIT`, `CPYDSK` |
  | `p25k_prg` | `CONFIG.BAS/.COM/.HLP`, **`SYS09` `SYS11` `SYS12` `SYS13`** (alternate BIOS profiles), **`CFTABLES.PHI`**, `MBASIC`, `BACKUP` |
  | `P2k5_LOGIC`, `P2k5_TKS` | UCSD p-System (see T32b) |

  **The write-back procedure is known and has been done.** The author of the
  `P25K_*` images put them back on real disks with ImageDisk using
  `250 kbps --> 300 kbps`, `Singleside`, `Doublestep: On`, `80 Tracks`
  (`../Information from the internet/P25Kinfo.txt ...`). The rate
  translation is the load-bearing part: those images are recorded at
  250 kbps, a PC drive's rate, while the P2500's own format is 300 kbps.
  The same procedure puts a `tools/cpm_build.py` image onto real media.

  **There is a person with a working P2500 at the other end of this.** The
  same notes say they ran these disks on their own machine and saw BDOS
  errors — so some bad sectors are the media's, not the dump's. They are the
  obvious lead for both halves of this task and for T43.

  **About 60% of each file is already recoverable** —
  `tools/cpm_extract.py --double-step`. Logical track `t` is present exactly
  when `t` is even, at `.raw` index `t/2`, because CP/M reads logical track
  `t` as cylinder `t + 1` and these images carry cylinders 1,3,5,…,79. A
  block `b` lives at logical track `2 + b/2`, so blocks come two-on/two-off
  and files land between 40% and 100% intact. Holes are zero-filled and
  every file reports its own recovery percentage.

  What the fragments already gave up: `CONFIG.COM` names the files it swaps
  (`CPM55`/`CPM58`, `CBI55`/`CBI58`, `PBI55`/`PBI58` over the live
  `SYS*.PHI` — the manual's "economy of memory" is a 55K or 58K build);
  `CONFIG.HLP` came out whole; and the `SYSxx.PHI` drive-type tables differ,
  with `SYSTEM` and `SYS09` listing only `5s`/`5d` while **`SYS12` and
  `SYS13` contain `hd`**. If that means what it looks like, those are
  Winchester-configured profiles — the condition `ROADMAP.md` set for taking
  the FXD/SASI card seriously. Hedge: 61–67% recovered, tables undecoded.

- [ ] **T32b. UCSD p-System.** `P2k5_LOGIC` and `P2k5_TKS` are certainly
  p-System (`SYSTEM.INTERP`, `SYSTEM.PBIOS`, `PASCALIO`), and a genuinely
  different bootstrap would be the best independent test of the disk path
  this project could get. Blocked twice over: both are double-stepped, and
  **no volume directory is findable** in the plain `.raw`, in the `.raw`
  re-ordered with this platform's sector skew, or in the existing
  `_deinterleaved` variants — matching earlier `ucsdpsys_disk` attempts, and
  *not* explained by the double-stepping, since the directory should sit on
  track 0, which is present. See `../UCSD p-System Repair/README.md` for the
  viability write-up.

- [ ] **T41b. Read `.IMD` images directly.** Lower priority than it looks:
  `tools/imd_tool.py` showed the healthy images normalise to exactly the
  `.raw` the emulator already reads, so this buys fidelity rather than
  unblocking anything — sector lookup by real ID, honest "sector not found",
  and bad-sector modelling, which `P25K_S` needs and which is currently
  invisible. Keep `.raw` support; select on extension or magic.

---

## P6 — Research: what is still locked in the firmware and the manual

- [ ] **T50. Mine the `.PHI` files statically.** Newly possible: a `.PHI` is
  a 6-byte `[load][end][x]` header plus a body that loads at `load`, verified
  byte-for-byte against a live RAM dump. The long-standing note that these
  files are "sector-interleaved, so the `org 0` listings have no usable
  addresses" was an artifact of the old broken extractor, not of the format.

  So every disk's CBIOS and PBIOS can be disassembled without booting it,
  and different builds diffed against each other. Worth doing:
  - **`SYS09/11/12/13` on `p25k_prg`** (61–71% recovered) — decode the
    drive-type tables properly and settle whether `hd` is a hard disk.
  - **`CFTABLES.PHI`** — named by `CONFIG.HLP`; the obvious home for the
    printer translation tables and the national keyboard tables.
  - **The `$E274` translation table** — cursor keys are decoded (the
    WordStar diamond); the rest is dead-key diacritic composition and is
    what T38 defers on.
  - **P25K_G's PBIOS** against P25K_B's, for T42.

- [ ] **T51. Model SESAM properly, including initialization.** The manual
  documents behaviour the emulator does not have: a P2219 system disk is
  **initialized against a key on first boot**, writing to itself (the
  write-protect tab must be removed), after which the wrong key or no key
  gives `INIT ERROR` on cold *or warm* boot. That confirms this project's
  early "self-modified boot" hypothesis as documented behaviour.

  Consequences: a real key will not necessarily boot these images, since
  they were initialized against their original owner's; and `INIT ERROR` is
  the string to watch for. Needs T30 first — initialization writes to the
  disk. Measured baseline for reference: the three booting disks do the
  IPL-only baseline **+3 reads / +3 writes** on port `$0F`, every other disk
  exactly baseline.

- [ ] **T52. Finish reading the P2219 manual.** OCR is checked in at
  `../Information from the internet/P2219-manual-OCR/` with a README
  marking which pages are reliable prose, which are tables needing visual
  reading, and which are blank. Already mined: the attribute encoding, the
  screen control codes, the video function codes, capitals lock, CONFIG, the
  drive map, SESAM initialization.

  Not yet mined: the printer interface and its translation tables, the
  supported disk formats in detail, the CP/M utility descriptions, and the
  8-bit code table on page 27 — which is printed sideways, is detected as
  such, and is **too dense for OCR**; it has to be read from the page image.

---

## P6b — Software for the machine

- [ ] **T53. Port `p2000m-othello` to the P2500.** Not started, and not
  urgent — but the analysis is done and written up in
  `docs/porting-cpm-software.md`, so it is a short job for whoever picks it
  up (upstream, most likely — it is Ivo's program).

  `OTHELLO.COM` v1.0.0 **runs unmodified**: the board, the CPU player and
  the key handling all work, so nothing about the CP/M layer or the z88dk
  build needs touching. Only three escape sequences differ, all in
  `src/othello.c`: `ESC H`/`ESC J` (clear screen) and `ESC p`/`ESC q`
  (inverse video). Cursor addressing and erase-to-end-of-line are already
  identical, and both machines are 80×24.

  Measured, not assumed — each sequence was put in a probe built with
  `tools/mk_cpm_probe.py` on a disk built with `tools/cpm_build.py`.

---

## P7 — Loose ends

Small, independent, none of them blocking.

- [ ] **T38 residue.** Live keyboard input works. Two pieces deferred:
  `P2500_KEYSTROKE_HZ` still exists, because that 100 Hz retry is also what
  makes a byte survive CBIOS zeroing its ring-buffer header late in init
  (`$ED02`) — removing it needs the init-order question settled first; and
  accented input is dropped rather than guessed, pending the `$E274` decode
  (T50). Capitals lock is authentic and has a GUI toggle.

- [ ] **T28. Serial transmit, end to end.** Everything is in place and
  nothing exercises it: CTC ch0 is the TX bit clock, its ISR is `$F597`,
  port `$04` is the data bit, port `$05` bit 6 is the handshake the ISR
  waits on. `PIP LST:=FILE.TXT` should make the path observable through the
  existing `[tx]` logging — and it is a real test of the CTC timing model,
  because a wrong bit rate produces recognisably mangled characters rather
  than nothing. With `$F72A` bit 0 set, `$F5A6` will not transmit until bit
  6 reads high, and `$F72D`'s timeout gives up if it never does.

- [ ] **T29. Identify what drives CTC channel 2's CLK/TRG.** The model
  assumes 50 Hz (`P2500_CLOCK_TICK_HZ`), right for both candidates — mains
  and the video frame rate — so CP/M keeps good time either way. It is the
  only frequency not derived from the 4 MHz crystal. **Cheapest route is now
  arithmetic**: the CRTC is programmed for `(R4+1)×(R9+1)+R5` = 311
  scanlines of `R0+1` = 98 character times, so the frame rate is the video
  card's dot clock over 243,824. A 12.19 MHz crystal gives exactly 50 Hz —
  and the card's crystal (the can at ref `5101`) is reported to start with
  "12". Reading it settles this.

- [ ] **T14. Read `$0422` handlers 2, 3, 6, 7** (`$04F9`, `$0539`, `$056B`,
  `$057D`) — the only IPL dispatch IDs still unidentified.

- [ ] **T40. An Emscripten build.** SDL3's callback app model makes this
  mostly a Makefile target. A browser-playable P2500 is a disproportionately
  good outcome for a machine with this little surviving software.

---

## Reference: `.PHI` system files

| Offset | |
|---|---|
| 0–1 | load address, little-endian |
| 2–3 | end address |
| 4–5 | third address (CCP base for `SYSCBI`, load address again for `SYSPBI`) |
| 6… | body, loading at the load address |

| File | P25K_B | P25K_G |
|---|---|---|
| `SYSCBI.PHI` | `$E200`–`$EABE` | `$E200`–`$E833` |
| `SYSPBI.PHI` | `$EAC0` | `$EB80` |
| `SYSLOAD.PHI` | `$1000` | identical to P25K_B's |

Extract with `tools/cpm_extract.py` (add `--double-step` for a half dump).

## Reference: CBIOS screen escape codes

CBIOS's dispatch table at `$F170`, which matches the P2219 manual's CONTROL
CODES (SCREEN) table entry for entry.

| ESC | meaning | handler |
|---|---|---|
| `0 <c>` | set video attributes (16 values, `@ \` P p B b R r A a Q q C c S s`) | `$F1D1` → `$F252` |
| `0 Z` | reverse screen | special-cased at `$F266` |
| `1` / `2` | start / end mosaic graphics | `$F1D9` / `$F1DE` (flag bit 6) |
| `3` / `4` | start / end high-resolution graphics | `$F1E3` / `$F1FB` (flag bit 5, port `$0A` bit 6) |
| `C` / `c` | visible / invisible cursor | `$F207` / `$F203` (CRTC R10; "off" writes `$10`, a start scanline past the cell) |
| `K` / `k` | erase to end of line / page | `$F1A9` / `$F1BF` |
| `S` / `T` | roll up / down | `$F098` / `$F0A5` (cursor address ±80) |
| `U` / `V` | next / previous page | `$F0AD` / `$F0B5` (±1920) |
| `Y rr cc` | absolute cursor address, offsets `$20` | `$F1A1` |

Anything else yields error code `$0D`. The `ESC 0` parameter byte's bits map
to the attribute nibble as 0→1, 1→3, 4→2, 5→0; the manual names them bit 0
low intensity, bit 1 flash, bit 4 reverse, bit 5 underline.

## Reference: RAM addresses

A dump of `$FE00`–`$FEFF` at any breakpoint is the highest-value single
debugging artifact this emulator produces.

### IPL era

| Address | Contents |
|---|---|
| `$FE00`–`$FE60` | IM2 vector table + trampolines, copied from ROM `$01FB` at `$01EB` |
| `$FE0A` | Device dispatch table |
| `$FE19`/`$FE45`/`$FE59`/`$FE5E` | `sub_0333h`'s request blocks; `$FE45` status `$00` → `JP $1000` |
| `$FE88` | Sectors per track |
| `$FE89` | `& 7` = sector-size code N (bytes = 128 << N) |
| `$FE9A` | Sector count for the transfer |
| `$FE9C`/`$FE9D` | **DMA Port B address — the transfer's RAM buffer** |
| `$FE9E` | Pointer to the current request descriptor |
| `$FEA0`/`$FEA1` | PIO port A interrupt vector / DMA vector (= +2) |
| `$FEA2` | **Length-prefixed µPD765 command buffer**, streamed to port `$15` |
| `$FEAC` | µPD765 result-phase buffer |
| `$FEB3` | DMA register-load template (20 bytes at `$FEB4`) |
| `$FED5` | The flag the two busy-waits (`$06C6`, `$0798`) spin on |
| `$FED6` | Drive-mode flag (`$52`/`$56` vs `$42`/`$46` on port `$11`) |
| `$FEDD` | µPD765 opcode for the pending operation (`$06` read / `$05` write / `$0D` format) |
| `$FF26` | Saved SP — IPL handlers exit via `LD SP,($FF26)` + `JP`, **not** `RET`/`RETI` |

### CP/M era (addresses are P25K_B's; P25K_G's PBIOS is `$C0` higher)

| Address | Contents |
|---|---|
| `$E200` | CP/M 2.2 CBIOS jump table. `$E21B` = SELDSK, `$E218` = HOME |
| `$E274` | Special-key translation table — cursor diamond plus dead-key diacritics (T50) |
| `$E34C` | **Capitals-lock mask**, `$20` as shipped; CONIN XORs alphabetic input with it at `$E4B7`. Toggled at `$E482` |
| `$E34D` | CONOUT's escape-length counter — suppresses the `$E4F8` translation inside a sequence |
| `$E46C` / `$E48C` / `$E4C3` | CONST / CONIN / CONOUT |
| `$E620` | **SELDSK** — per-drive records at `$E880`, availability map at `$E87C`, DPHs at `$E81D` (stride 16), shared DPB at `$E84D` |
| `$E84D` | **DPB**: SPT 32, BSH/BLM 4/15 (2048-byte blocks), EXM 1, DRM 63, AL0 `$80`, OFF 2 |
| `$EAC0` | Generic "post a request, then spin until its status changes" |
| `$EB56` | **Device table**: count byte then 3-byte `{id, ptr_lo, ptr_hi}` records. `$31` console out, `$30`, `$00`–`$03` disk, `$40`, `$FF`. Each pointer is `[semaphore][driver_lo][driver_hi]` |
| `$EBE2` | **`EI / RETI`** — how every CBIOS handler releases the daisy chain |
| `$EBE5` / `$EBFB` | Circular-buffer push / pop; header `[count][write_idx][read_idx]`, 32 bytes at `+3` |
| `$EDD5` | **Screen driver** (device `$31`). Function table at `$EDFC`: `$00` prepare, `$01` reset, `$02`, `$03` status, `$05` write → `$EE91` |
| `$ED2A` → `$ED2D` | **CTC channel 3 ISR** — one `IN A,($06)`, push to the ring at `$ED8F` |
| `$ED02` | Keyboard ring init; **runs late**, so anything strobed in earlier is discarded |
| `$F170` | Escape command table (see above); `$F199` is the alternate set armed by `ESC 3` |
| `$F37F` → `$F382` | **CTC channel 2 ISR** — increments the 24-bit tick counter at `$F436` |
| `$F43B` | Current attribute byte; its low nibble reaches port `$0A` |
| `$F454` | `3E 00 D3 0A` — the `LD A,<nibble> / OUT ($0A),A` template |
| `$F46C` | `3E 0A D3 08 3E 00 D3 09` — the CRTC R10 write template used by `ESC C`/`ESC c` |
| `$F475` | Write pointer into the driver's generated-code queue at `$FD23`, terminated with `$C9` and then called |
| `$F546` | Baud table, 7 × `{control, time constant}` — 75/110/150/300/600/1200/2400, each within 0.3% |
| `$F597` / `$F669` | CTC channel 0 / 1 ISRs — serial transmit clock, receive sampler |
| `$FF90` | CBIOS IM2 table at `I=$FF` |

## Reference: invariants the harness asserts

Scoped by era — CP/M replaces page zero and the IM2 table wholesale, so an
IPL-era assertion left unscoped will fire on a *correct* boot.

**While the IPL owns memory:**

- `$0000`–`$0002` = `C3 00 01` and `$0003`–`$0005` = `C3 DA 02` whenever
  port `$05` bit 3 is clear. Anything else is memory corruption.
- Register `I` = `$FE` before any IM2 interrupt is delivered. **IPL only** —
  CBIOS builds its own table at `I=$FF`.
- Port `$14` reads exactly `$80` when idle (ROM `$0498` compares for
  equality, not a bit test).

**Once CP/M is up** — all asserted by `make test`:

- `$0000`–`$0002` = `C3 03 E2` and `$0005`–`$0007` = `C3 06 D4`.
- `$E200`–`$E20F` is CP/M 2.2's standard BIOS jump table, all `JP nn`.
- `I` = `$FF`, IM 2.
- The screen reads `Philips P2500` / `58K CP/M Ver. 2.2` / `A>`, and typing
  `dir\r` lists the six files the disk actually contains.
- `DIR B:` lists drive B's disk and **not** drive A's.
- The rendered frame is 640×288, has ink below the `p` of "Philips" (the
  8×12 cell), and a solid cursor block where R14/R15 point.
- The menu bar draws and the screen is offset below it.
- An unbootable disk prints the IPL banner and halts at `$014D`.
- The attribute latch is silent on a plain boot.
- **Every interrupt request is acknowledged exactly once.** The exit report
  prints `requests/acknowledged` per device; the two diverging means the
  daisy chain is dropping something, which is the failure mode that cost
  this project the most time.
