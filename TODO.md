# TODO — P2500 emulator

The work queue. `README.md` is the orientation document, `ROADMAP.md` is the
sequencing and the reasoning behind it.

> **Completed work has been removed from this file. It is in git history.**
> Rewritten 2026-09-28. Phases 1 and 2 (T1–T26, T33, T41a) are done and
> their write-ups — the evidence chains, the five root-caused ISSUEs, the
> three original misidentifications — were long and are no longer load-
> bearing. Recover any of it with `git log -p --follow TODO.md`, or by the
> task ID: `git log --oneline --all --grep 'T17'`. Source comments still
> cite the task IDs that produced them, which is why **open task IDs are
> never renumbered** even when the sections around them are.

---

## Where this is

**CP/M 2.2 boots to the `A>` prompt and runs typed commands**, on three of
the nine disk images. `make test` is the proof and the guard: 19 checks,
~8 s, exit 1 on any failure. Keep it green.

```
./p2500-emu --disk "../Disk Images/extracted/P25K_B/P25K_B.raw" \
            --max-steps 20000000 --type 'dir\r' --dump-vram /tmp/vram.bin
python3 tools/render_vram.py /tmp/vram.bin /tmp/screen.png
```

**It also runs in a window you can type into.** `make gui` builds
`p2500-gui`, an SDL3 front-end whose geometry, cursor position and cursor
shape all come from the MC6845's own registers (T35–T38). The headless
harness is unchanged and is still what `make test` drives.

The biggest remaining gap in P1 is the **debugger panels** (T39) — the
actual reason for having a GUI — plus T34, which the GUI currently works
without but a log panel and a reset button will need.

---

## The hardware model, as currently decoded

Every entry traces to firmware or a datasheet; nothing here is a tuning
constant. Where something is still assumed, it says so.

| Port | Device | Notes |
|---|---|---|
| `$00`–`$03` | **Z80A-CTC** (Z8430) | `channel = port & 3`. ch0 = serial TX bit clock (TIMER, no CLK/TRG); ch1 = serial RX bit sampler, CLK/TRG is the RXD line; ch2 = 50 Hz real-time clock strobe (**source unidentified, T29**); ch3 = keyboard "byte ready" strobe |
| `$04` | Serial / printer TX | One data bit, clocked by CTC ch0 |
| `$05` | Bank latch (W) / status (R) | Write: bit 3 = EPROM out of `$0000`–`$0FFF`; bits 0–2 all clear = video DRAM window at `$8000`–`$BFFF`, all set = main DRAM. **The other six combinations are undecoded and now trip a diagnostic (T27).** Read: bit 7 = RXD, bit 6 = TX handshake; bits 0–5 unknown, read back 1 |
| `$06` | Keyboard data | Byte-wide, one `IN` per ch3 interrupt |
| `$08`/`$09` | **MC6845 CRTC** | All 18 registers stored and printed at exit; **nothing renders from them yet (T37)**. 80×24, 12 scanlines/row, 311 scanlines/frame. Only R14–R17 read back, per the datasheet |
| `$0A` | **Video attribute latch** | Write-only. The low nibble is the 4-bit attribute every subsequent write into the video window stores alongside the character (T27). Bit 6 is set by `ESC 3`. Not a POST latch, which is what it had been mistaken for |
| `$0F` | SESAM dongle / bootable cartridge | Access is counted; the IPL-only baseline is 32 reads / 6 writes |
| `$10`–`$13` | **Z80A-PIO** (Z8420), FDD card | Mode 3 bit control. PA0 = µPD765 `INT` (**assumed, not traced**; if it ever misbehaves, try PA1 before concluding the model is wrong) |
| `$14`/`$15` | **µPD765 FDC** | Idle status is exactly `$80`. Read path complete; write/format decoded but not implemented (T30) |
| `$16` | **Z80A-DMA** (Z8410) | Self-describing register stream, not a positional template |
| `$18`–`$1E` | Unclaimed | Appears in `SYS09.PHI`; most likely the optional 8" drive interface. Untouched |

**IM2 daisy chain order** (`src/core/intctl.c`): DMA → PIO A → PIO B →
CTC 0/1/2/3. The DMA-before-PIO half is *derived*, not guessed — the IPL's
DMA and PIO handlers share one saved-SP word at `$FF26` and one private
stack, so only this order lets them coexist; the other dies in an `RST 38`
loop. The CTC's position relative to the FDD card is still the conservative
reading rather than a measured fact.

**Disk geometry.** The logical track number in each sector's ID field is
**physical track + 1** — a Philips-specific quirk, confirmed by an owner's
own 22DISK debugging writeup. Sector skew is odd-then-even (1,3,5…,2,4,6…).
`lba = (C - 1) * sectors + (R - 1)`.

---

## P1 — Make it an interactive machine

The project's centre of gravity. Everything advanced so far came from
instrumentation, so **the GUI's primary purpose is a debugger**, not a
settings dialog — build the panels before the file dialogs.

Target layout, of which the first third exists:

```
src/core/   -> libp2500.a   C11, zero dependencies, no stdio, no globals
src/cli/    -> p2500-emu    headless; what `make test` runs
src/gui/    -> p2500-gui    SDL3 + Dear ImGui; the only C++ in the tree
```

`make test` must keep working with no display and no SDL installed. That
suite is the only thing standing between this project and silently
regressing CP/M boot, and a headless check, a bisect or CI are exactly the
contexts where a GUI dependency would make it unrunnable.

- [ ] **T34. Make the core embeddable: a log callback, and a real reset.**
  Do this first; both are concrete blockers found by audit.
  - **63 `fprintf(stderr, ...)` calls in the core** (`dma.c` 24, `fdc.c` 12,
    `pio.c` 8, `machine.c` 7, `ctc.c` 5, `intctl.c` 4, `keyboard.c` 3). A
    core that writes to `stderr` cannot feed a GUI log panel, a MAME
    `logerror()`, or a browser console. Replace with one `p2500_log_fn` on
    the machine (level + category + formatted message); the CLI installs a
    callback reproducing today's output verbatim, so this stays a no-
    behaviour-change refactor that `make test` can verify.
  - **There is no `p2500_reset()`.** `p2500_init()` does
    `memset(m, 0, sizeof(*m))`, which would wipe the loaded EPROM image and
    the `fdc.disk` / `sesam.stream` / `keyboard.queue` pointers. A GUI reset
    button needs state cleared while attached media *survives*. Split
    `p2500_init` into allocate-and-attach vs. reset; the CLI calls both.

  The core has **no non-const file-scope state at all**, which is the
  expensive property to retrofit and is already correct. Keep it that way.

- [x] **T35. Pace the machine from `z->cyc`, not from wall-clock sleeps.**
  The emulator knows exact emulated time at 4 MHz, and `machine.c` already
  generates the 50 Hz CTC channel-2 strobe — one field is 80,000 T-states.
  Run the core to the next strobe boundary, then present: the frame loop and
  the machine's own clock tick become the same event. If T29 confirms
  channel 2 is the video frame rate rather than mains, that alignment stops
  being a convenience and becomes correct.

  Expose it as `p2500_run_until_cyc()` in the core so both front-ends and
  any future embedding share it. Headroom is comfortable — roughly 10 M
  emulated instructions/sec against a real machine's ~1 M — so **keep it
  single-threaded**. One thread is what makes T39 safe to write with no
  locks at all.

- [x] **T36. SDL3 shell. — DONE (2026-09-28).** Use the callback app model (`SDL_AppInit` /
  `SDL_AppIterate` / `SDL_AppEvent`) rather than a hand-rolled main loop: it
  is the shape SDL3 is designed around and it makes T40 nearly free. One
  streaming `SDL_Texture` for the framebuffer, integer scaling, no per-glyph
  draw calls. Stay on `SDL_Renderer` throughout — no direct OpenGL — so
  T39's ImGui backend can share it. `sdl3` 3.4.16 is packaged and already
  installed (`extra/sdl3`), so there is no vendoring decision here.

- [x] **T37. A CRTC-driven renderer, replacing the hardcoded 80x24. — DONE (2026-09-28).**
  **No longer blocked** — T27 settled the framebuffer layout (8-bit code +
  4-bit attribute per cell) and T27a settled the glyph geometry (8×12, read
  12 bytes from `code × 16`). `tools/render_vram.py` is the working
  reference implementation of both, attributes included.

  **What the firmware actually programs, now printed by the exit report:**

  ```
  CRTC (MC6845): 61 50 53 0E 18 0B 18 18 00 0B 00 0B 00 00 01 E2 00 00
    80 cols x 24 rows, 12 scanlines/row, start $0000, cursor $01E2 (lines 0-11)
  ```

  Three things fall out of that, all measured:
  - **CP/M maintains the cursor in R14/R15 live.** After `DIR`, the cursor
    register reads `$01E2` = 482 = row 6 x 80 + 2, which is exactly where
    the `A>` prompt leaves it. So the cursor needs no guesswork: read the
    register.
  - **The cursor is a solid full-cell block**, not blinking: R10 = `$00`
    (B6B5 = 00 = non-blink) and R10/R11 span scanlines 0-11, the whole cell.
  - **Start address is `$0000` and stays there** — CP/M scrolls by moving
    memory, not by moving the CRTC's start address. A renderer can honour
    R12/R13 anyway, but nothing here exercises it.

  Take geometry from R0/R1/R6/R9 and start address from R12/R13. R12/R13 and
  R14/R15 are **14-bit pairs** - the high byte carries only 6 bits.

  **Done**, as `src/core/video.{c,h}`. `p2500_video_render()` fills a
  caller-supplied 32-bit framebuffer and both front-ends call it, so they
  agree by construction rather than by two implementations staying in step —
  the CLI reaches it through `--dump-screen`. Geometry comes from R1/R6/R9,
  start address from R12/R13, the cursor from R14/R15 with its shape and
  blink from R10/R11, and the 4-bit attribute plane is composited.

  The cursor is drawn by inverting the scanlines R10-R11 select, which is
  how the real chip composites it, and it lands where CP/M puts it with no
  heuristic: row 6 column 2 after `DIR`.

  **How the cursor is hidden, and why SuperCalc2 appeared to have none.**
  `ESC C` / `ESC c` (cursor on/off) do not touch the blink bits: they write
  CRTC **R10** through the driver's generated-code template at `$F46C`
  (`LD A,$0A / OUT ($08),A / LD A,<v> / OUT ($09),A`). "Off" writes `$10` —
  cursor start scanline 16, past the end of a 12-scanline cell, so the chip
  never composites it. Nothing to special-case; a renderer that honours
  R10/R11 as scanline bounds gets this right for free.

  SuperCalc2 hides the hardware cursor that way and marks the current cell
  with a **reverse-video attribute** instead, which is why it looked like it
  had no cursor until T27's attribute latch was modelled.

  Guarded by `make test`, which asserts on the rendered pixels: a 640x288
  frame, ink below the `p` of "Philips" (the T27a descender, verified to
  fail if the glyph is truncated back to 8 rows), and a solid cursor block
  at row 6 column 2.

- [~] **T38. Live keyboard input — WORKING; two pieces deferred.** That
  constant is a 100 Hz retry loop standing in for "the user keeps pressing
  the key", and it exists only because the CLI has no concept of a key
  *event*. A GUI does: strobe CTC channel 3 exactly once per
  `SDL_EVENT_KEY_DOWN`. It is the last piece of the model not derived from
  firmware, and this is what retires it.

  What is already known, checked rather than assumed:
  - **Alphanumerics are plain ASCII** — raw ASCII on port `$06` drives CP/M
    end to end, which is what `--type 'dir\r'` does.
  - **Special keys are high-bit codes.** CBIOS's table at `$E274` decodes
    the cursor keys onto the WordStar diamond: `8B 05 / 87 13 / 89 04 /
    85 18`, so `$8B`=up (`^E`), `$87`=left (`^S`), `$89`=right (`^D`),
    `$85`=down (`^X`).
  - **The rest of that table is dead-key diacritic composition** (`A >`,
    `E ~`, `C ,`, `N ~`, `a <` — Â, Ê, Ç, Ñ, à), which is what a
    multilingual European keyboard needs. A naive SDL keysym mapping will
    get these wrong; decode the full table before guessing.
  - Still unknown: function keys, and whether the controller ever sends
    make/break pairs rather than single codes. Nothing traced suggests it
    does.

- [ ] **T39. ImGui debugger panels — the actual point of the GUI.** Vendor
  Dear ImGui (not in the Arch repos, and designed to be vendored — the same
  treatment `vendor/superzazu_z80` already gets) with the `imgui_impl_sdl3`
  + `imgui_impl_sdlrenderer3` backends so it shares T36's renderer. **Keep
  all C++ inside `src/gui/`**; the core never sees it.

  Panels, in the order they would have paid for themselves historically:
  1. **Device state** — the IM2 daisy chain (`requested` / `under_service` /
     vector per source, live), the four CTC channels with down-counters and
     CLK/TRG levels, DMA registers, FDC phase. This panel would have made
     the two hardest bugs in this project's history obvious by inspection
     instead of by archaeology.
  2. **Memory viewer** with live watches, bank-aware (`p2500_peek`).
  3. **Disassembly around PC** with breakpoints. `tools/disasm_ram.sh`
     proves z80dasm gives usable output; inline an equivalent.
  4. **Port/IRQ log**, fed by T34's callback.

  `--watch` / `--count` / `--break` already exist as CLI concepts. Lift them
  into a small `core/debug.h` both front-ends drive rather than
  reimplementing them against ImGui.

  ### Menus: use ImGui's, not a native menu bar (assessed 2026-09-28)

  A native menu bar is the obvious thing to want and it is the wrong tool
  here, for a platform-specific reason worth writing down so it is not
  re-litigated.

  | Option | Verdict |
  |---|---|
  | [SDL PR #13752](https://github.com/libsdl-org/SDL/pull/13752) | **Windows and macOS only**, and still unmerged - opened 2025-08-15, last touched 2026-03-22. Its Linux section is the author writing "Basically I'm not sure where to start here", with GIO/portals and GTK floated as ideas. Adopting it means vendoring an SDL fork to get nothing on this machine |
  | [thomashope/native-menu-bar](https://github.com/thomashope/native-menu-bar) | Genuinely nice - Unlicense, two files, clean C. But its SDL example wires up **Win32 only** (`wmInfo.info.win.window`), and the GTK backend takes a `GtkWindow*`: it is for apps that already are GTK apps. Its own TODO still has "test GTK backends on WSL" unchecked, and it targets SDL2 |

  The blocker underneath both is **Wayland**, which is what this machine
  runs. There is no foreign-window reparenting, so a GTK menu bar cannot be
  attached to an SDL window; and KDE on Wayland has no global application
  menu by default, so there is no bar to export to either. A native menu bar
  is best supported on exactly the two platforms this project is not
  developed on.

  **So draw the menu bar in-window with ImGui** (`BeginMainMenuBar`). It
  behaves identically on every platform and backend because the application
  draws it, it works on Wayland today, and ImGui is being vendored for the
  panels anyway - the menu bar is very nearly free once it is. This is also
  what most ImGui-based emulators do.

  Keep the menu *model* - ids, captions, checked state, the action each item
  fires - as plain data in `src/gui/`, separate from the code that renders
  it. Then a native backend can be added later if SDL ever lands one,
  without the menu definitions moving.

  **One genuinely native piece is available right now and should be used:**
  `SDL_ShowOpenFileDialog` (`SDL3/SDL_dialog.h`, present in 3.4.16). On
  Linux it goes through the XDG desktop portal, so "Load disk image..."
  gets KDE's own file dialog on Wayland rather than a hand-rolled browser.
  There is no reason to build a file picker.

---

## P2 — Video attributes (gates T37)

- [x] **T27. Model the video card's real memory organisation. — SOLVED
  (2026-09-28).** The layout was settled earlier; this entry now records the
  write path, which was the part nothing could derive until the screen
  driver was disassembled.

  **The CPU does not address the attribute nibble through memory at all. It
  writes it to PORT `$0A`, and the card latches it**: the next byte written
  into the `$8000`-`$BFFF` window is stored as one 12-bit word, eight bits
  from the data bus and four from the latch.

  That is why every earlier search failed. There was no memory window to
  find, so port `$05`'s six spare bits-0-2 combinations were never going to
  be it, and the CGA-style char/attribute interleave the traces had already
  ruled out was ruled out for the right reason: a 12-bit word cannot be
  split into two CPU-addressable bytes, so this board latches instead.

  **Port `$0A` was hiding in plain sight** — it had been written off as a
  write-only "diagnostic/POST latch" because the IPL pokes it twice during
  init. Those pokes are the attribute latch being cleared before anything is
  printed. This also closes the port-`$0A` half of T15.

  ### How it was found

  The chain from a character to the hardware, each step read out of a live
  `--dump-ram` image:

  | | |
  |---|---|
  | `$E4C3` | CONOUT. Posts the byte as a request; `$E354` is `JP $EAC0` |
  | `$EAC0` | generic request poster. Reads the device id from request byte +1 (`$31`), finds it in a stride-3 table at `$EB56`, and calls through a double indirection |
  | `$EDD5` | **the screen driver**. Dispatches the request's function code through a table at `$EDFC` |
  | `$EDFC` | function table: `$00` prepare → `$EE0D`, `$01` reset → `$EE3B`, `$02` → `$EE7D`, `$03` status → `$EE57`, **`$05` write → `$EE91`**. Exactly the function codes the P2219 manual documents |
  | `$EE91` | the write loop. A two-bit state machine: bit 4 of the driver flags → `$F141`, bit 5 → `$F0C6`, otherwise normal output at `$EEDE` |
  | `$F141` | escape handler. Dispatches on `$F439` through `$F151`; entry `$00` → `$F15F`, which matches the byte against the **escape command table at `$F170`** |
  | `$F1D1` | **`ESC 0`**. Sets sub-state 3 and returns with **carry set**, which keeps the state armed for one more byte |
  | `$F252` | the parameter byte. Validates it against a 16-entry table, scatters its bits into a nibble, merges into `$F43B`, calls `$F2CF` |
  | `$F2CF` → `$F35A` | appends a 4-byte record to a queue that is later **executed as code**. The template at `$F454` is literally `3E 00 D3 0A` = `LD A,<nibble> / OUT ($0A),A` |

  **The driver compiles screen updates into Z80 code and calls them** (at
  `$FD23`, terminated with a `RET` it writes itself). That incidentally
  explains the unrolled `LD A,imm / LD (nnnn),A` blocks at `$FD20`-`$FDAF`
  that an earlier session found executing and could not account for.

  ### The escape command set, decoded from `$F170`

  | ESC | handler | | ESC | handler |
  |---|---|---|---|---|
  | `Y` | `$F1A1` cursor address | | `S` | `$F098` |
  | `K` | `$F1A9` erase | | `T` | `$F0A5` |
  | `k` | `$F1BF` erase | | `U` | `$F0AD` |
  | **`0`** | **`$F1D1` attribute** | | `V` | `$F0B5` |
  | `C` | `$F207` cursor on | | `1` | `$F1D9` (sets flag bit 6) |
  | `c` | `$F203` cursor off | | `2` | `$F1DE` (clears bit 6) |
  | | | | `3` | `$F1E3` (arms an alternate table; also `OUT ($0A),$40`) |

  An alternate table at `$F199` applies once `ESC 3` has set flag bit 5:
  `0` → `$F1D1`, `4` → `$F1FB` (which clears bit 5 again). `ESC C`/`ESC c`
  landing on cursor on/off independently confirms the decode — that is
  exactly what `VALLEY.BAS` declares them as.

  ### The parameter encoding

  `$F252` scatters the parameter byte's bits into the nibble:

  | parameter bit | → attribute bit |
  |---|---|
  | 0 | 1 |
  | 1 | 3 |
  | 4 | 2 |
  | 5 | 0 |

  The 16 legal parameters are therefore `@` `` ` `` `P` `p` `B` `b` `R` `r`
  `A` `a` `Q` `q` `C` `c` `S` `s`, reaching all 16 nibble values (`Z` is a
  17th, an alias for `$C`). **Verified by execution, not just by reading**:
  driving all 16 through MBASIC-80 and dumping the attribute plane
  reproduces the table exactly.

  ```
  ./p2500-emu --disk ".../P25TEST.raw" --max-steps 260000000 \
    --push-at '4000:mbasic\r' \
    --push-at '100000:FOR N=0 TO 15:P=64+(N AND 3)+16*INT(N/4):' \
    --push-at '104000:PRINT CHR$(27);"0";CHR$(P);CHR$(65+N);:NEXT\r' \
    --dump-vram-attr /tmp/t.attr
  ```

  ### The bit names, from the manual

  Not derivable from the driver — it only passes the nibble to the hardware,
  so the names live in the card's discrete logic, not in firmware. `$F30C`
  turned out to be the screen-clear code generator and `ESC S/T/U/V` are
  cursor movement (`+80`, `-80`, `+1920`, `-1920` on the cursor address at
  `$F447`), so neither names anything.

  The answer is in `../Information from the internet/(P2000C) P2219 CPM
  Manuals.pdf`, which is a **scanned** PDF with no text layer — it had to be
  OCR'd (`pdftoppm` + `tesseract`) before it could be searched. Its "CONTROL
  CODES (ESCAPE SEQUENCES)" section defines the parameter byte:

  ```
  bit 0 = 1  display character at low intensity
  bit 1 = 1  flash character
  bit 4 = 1  display character in reverse video
  bit 5 = 1  display character underlined
  ```

  and tabulates all 16 sequences — **the same 16 values already recovered
  from the firmware's validation table and confirmed by execution**, which
  is as good a cross-check as this project gets. Composing the manual's
  parameter bits with `$F252`'s scatter gives the plane:

  | attribute bit | meaning | from parameter bit |
  |---|---|---|
  | 0 (`$1`) | underline | 5 |
  | 1 (`$2`) | low intensity | 0 |
  | 2 (`$4`) | reverse video | 4 |
  | 3 (`$8`) | flash | 1 |

  `src/core/video.h` and `tools/render_vram.py` now carry that instead of a
  placeholder. Only *underline* had been guessed correctly.

  Confirmed against real software: SuperCalc2's grid puts nibble `$4`
  (reverse) on the current-cell pointer and `$6` (reverse + low intensity)
  on the column headers — see `shots/sc2_grid.png`. `VALLEY.BAS` draws
  terrain with `$6` too.

  The manual also notes what the driver's behaviour already implied: an
  attribute "takes effect from the current attribute position to the next
  position where an attribute is set", i.e. it is latched and applies to
  everything written after it. Exactly the port `$0A` model.

- [x] **T27a. The character cell is 8×12, not 8×8. — DONE (2026-09-28).**
  Fell out of T27 and fixes a visible bug, so it landed immediately rather
  than waiting for T37. The character ROM's stride is **16 bytes per code**
  (4096 / 256): rows 0–11 are the glyph the CRTC clocks out (R9 = 11, i.e.
  12 scanlines per row) and rows 12–15 are unused padding — which is where
  the ROM's packed Z80 code lives.

  `tools/render_vram.py` was reading 8 rows from `code × 16`, silently
  truncating every descender. Proof it is exactly 12: across the printable
  ASCII range the only codes with ink in rows 8–11 are `$ , ; @ f g j p q y`
  — the descender set and nothing else. The `p` in "Philips" has rendered
  wrong in every screenshot this project produced until now.

  This also unifies the character ROM's "three independent things in a
  512-slot structure" reading into one 12-row cell — see the restatement
  added to `../ROM Dumps/Video-Card-Character-ROM/findings.md`.

## P3 — Write to disk

Nothing in CP/M's read-only path needed either of these, which is why the
prompt was reachable without them. `PIP` copying a file onto the disk is the
first thing that needs them, and is also the natural test.

- [ ] **T30. Invert the DMA↔FDC data path, then finish the µPD765 command
  set.** `do_read_data` hands its whole block to the DMA in one `memcpy`,
  so the data flows FDC→DMA by direct call rather than the DMA pulling
  bytes through port `$15`. Writing needs the opposite direction, which
  means the transfer has to be driven from the DMA side. `WRITE DATA`
  (`$05A1` → `$0B43`) and `FORMAT A TRACK` (`$05A6` → `$0B19`) are both
  already decoded from the IPL's own command tables; neither is implemented.

  Two things fall out of the same change, both currently unmodelled: the
  FDC's `EXM` status bit (bit 5), which can never read 1 while a whole block
  moves in one `memcpy`, and per-byte pacing of the execution phase across
  multiple `p2500_step()` calls.

- [ ] **T31. Multi-track reads past `EOT`.** Independent of the inversion.
  `do_read_data()` hands the DMA the whole remaining disk image as one flat
  span, so a multi-sector read running past the last sector of a track walks
  into the next track instead of terminating. CP/M's directory reads stay
  inside one track, which is why `DIR` is correct; a large sequential file
  read will not be.

---

## P4 — The disk images that still do not boot

Status of all 11 `.raw` images in `../Disk Images/extracted/`. Verify any
`.IMD` with `tools/imd_tool.py verify`; the checked-in manifest is
`../Disk Images/findings/IMD-integrity-manifest.txt`.

| Image | Tracks | State |
|---|---|---|
| `P25K_B` | 80 | **CP/M to `A>`**, `DIR` works. The `make test` reference |
| `P25K_S` | 80 | **CP/M to `A>`**. One sector flagged with a data error, matching the uploader's reported BDOS errors |
| `P25TEST` | 80 | **CP/M to `A>`**, 34 files |
| `P25K_G` | 80 | Banner, then no prompt — **T42**, and the only clean image that fails |
| `P2500GAM` | 77 | **Not bootable by design** — sector 0 is all `$E5`, formatted and empty. Nothing to fix |
| `P2k5_CPM`, `P2k5_LOGIC`, `P2k5_TKS`, `p25k_prg` | 40 | **Double-stepped dumps. Unrecoverable; need re-imaging** |

**The four failures are a dumping artifact, not an emulator bug.** Their
`.IMD` cylinder maps run `1,3,5,…,79` where every working image runs
`1,2,3,…,n` — 48 TPI media read by a 96 TPI drive that double-stepped, so
every other track was never read at all. The correlation with bootability is
perfect. No repair is possible: `P2k5_CPM` needs ID cylinder 2 as the
*second* thing its loader touches, so the gap is at the start, not the tail.

SESAM protection is not the blocker for any of them — port `$0F` access
*correlates with success*: the three working disks do one extra transaction
over the IPL-only baseline, and every failing disk sits at or below it.

Two notes before reading any failure as something else. `$013C` is the IPL's
**failure** path, not a splash: if `sub_0333h` returns instead of `JP $1000`
-ing into the boot sector, `$013C` writes an error code to the port `$0A`
latch, prints `PHILIPS MICROCOMPUTER P2000/B` from `$0184`, and halts at
`$014C` (`DI / JR $`). And the per-file extractions that used to live under
`../Disk Images/extracted/` were deleted (2026-09-28) as unreliable — 38 of
142 were wrong. Only the `.raw` images remain; regenerate per-file sets from
the `.IMD` originals with `cpmtools` if research needs them.

- [ ] **T42. `P25K_G` blocks on an event that never signals.** The one
  complete, clean image that does not reach a prompt — so unlike the
  double-stepped disks, this one is ours. Its CBIOS is a different build
  (banner `PHILIPS P2000B / CP/M 2.2 - 58K`) but page zero is identical to
  `P25K_B`'s (`C3 03 E2 / C3 06 D4`) and it loads fine: 82 DMA transfers,
  all acknowledged, clock ticking.

  It ends in a 2-instruction spin at `$EBC2`: `LD A,$01 / CP (HL) /
  JR NZ,-3` with `HL=$EC2A` — the generic "post a request, wait for its
  status to become 1" loop, at a different address in this build. The
  machinery works: slot `$EC2D` (device id 5, `B=5`) is posted and signalled
  repeatedly by `$ECD3`, while slot `$EC2A` (device id 4, `B=4`) is posted
  at step 1,019,307 and never signalled again.

  Ruled out: **not** the keyboard (`--type` delivers and CTC ch3
  acknowledges 4 of 4, with no effect), not SESAM, not the clock (ch2 fires
  3451 times). One traced hardware difference worth noting: this build
  programs ch2 as `$C5` (falling edge) where `P25K_B` uses `$D5` (rising);
  the emulator's square wave supplies both edges so it ticks either way.

  **Next step: identify which ISR is supposed to signal device 4.**

- [ ] **T32b. UCSD p-System boot.** `P2k5_LOGIC` and `P2k5_TKS` use a
  genuinely different bootstrap, so they are the best independent check on
  the disk path that exists — and the first thing to test the IM2 chain
  against software that was never considered while building it. **Blocked
  until they are re-imaged**: both are double-stepped.

- [ ] **T41b. Read `.IMD` images directly in the emulator.** Lower priority
  than it first looked — `tools/imd_tool.py` proved the five healthy images
  normalize to exactly the `.raw` the emulator already reads, so this buys
  fidelity rather than unblocking anything.

  What it buys: sector lookup by *real ID* rather than computed LBA (so the
  `physical + 1` convention stops being special-cased and becomes a property
  of the media), honest "sector not found" instead of silently reading the
  wrong track, real interleave rather than relying on the `.raw` being
  pre-deskewed, and somewhere to model bad sectors — which `P25K_S` has and
  which is currently invisible.

  The format is simple: an ASCII header terminated by `$1A`, then one
  variable-length record per track — `mode, cylinder, head, sector-count,
  size-code`, a sector-number map, optional cylinder and head maps (flagged
  by head bits 7 and 6), then one record per sector whose leading type byte
  says whether it is absent, stored whole, or run-length compressed. Check a
  C implementation against `tools/imd_tool.py` and
  `../Disk Images/tools/python/imd_decode.py`.

  **Keep `.raw` support** — it is what `make test` and the SESAM regression
  use. Select on extension or magic.

---

## P5 — Loose ends

Small, independent, none of them blocking.

- [ ] **T28. Serial transmit, end to end.** Everything is in place and
  nothing exercises it: CTC ch0 is the TX bit clock, its ISR is `$F597`,
  port `$04` is the data bit, port `$05` bit 6 is the handshake input the
  ISR waits on. `PIP LST:=FILE.TXT` at the prompt should make the whole
  path observable through the existing `[tx]` logging — and it is a genuine
  test of the CTC timing model, because a wrong bit rate produces
  recognisably mangled characters rather than nothing. Note the handshake:
  with `$F72A` bit 0 set, `$F5A6` will not transmit until bit 6 reads high,
  and the timeout at `$F72D` gives up if it never does.

- [ ] **T29. Identify what really drives CTC channel 2's CLK/TRG.** The
  model assumes 50 Hz (`P2500_CLOCK_TICK_HZ`), right for both candidates —
  mains and the video frame rate — so CP/M keeps good time either way. It is
  still the only frequency in the emulator not derived from the 4 MHz
  crystal. Firmware may settle it: find what reads the 24-bit counter at
  `$F436` and what it divides by. If something converts it to seconds with a
  constant, that constant *is* the tick rate.

  **New route, if channel 2 turns out to be the video frame rate.** The CRTC
  registers give the frame geometry exactly: `(R4 + 1) x (R9 + 1) + R5` =
  `25 x 12 + 11` = **311 scanlines per frame**, at `R0 + 1` = 98 character
  times per line. So the frame rate is the card's dot clock divided by
  `8 x 98 x 311` = 243,824. A 12.19 MHz dot clock gives exactly 50 Hz. That
  turns T29 into arithmetic the moment someone reads the video card's
  crystal — the can-shaped part at ref `5101`, next to the analog section
  (`../Actual P2500 hardware/P2500 Video Card/`), which has never been
  identified. Worth adding to the measurement list as a cheap win.

- [ ] **T14. Read `$0422` handlers 2, 3, 6, 7** (`$04F9`, `$0539`, `$056B`,
  `$057D`) — the only IPL dispatch IDs still unidentified.

- [x] **T15. Port `$0A` identified and modelled. — DONE (2026-09-28).** It
  is not a diagnostic/POST latch: it is the **video attribute latch** (T27).
  The IPL's two writes at `$013C` and `$01C3` are clearing the attribute
  before it prints anything, not posting an error code. Modelled in
  `machine.c`; the exit report prints how many video writes carried a
  non-zero attribute.

- [ ] **T40. An Emscripten build.** Optional once T36 lands; SDL3's callback
  model makes it mostly a Makefile target. A browser-playable P2500 is a
  disproportionately good outcome for a machine with this little surviving
  software, and it costs little if T36 uses the callback API from the start.

---

## Reference: external sources on the MC6845

Assessed 2026-09-28. The CRTC is the one chip on this machine that is a
plain commodity part, so outside material exists — but be clear about what
it can and cannot tell us. **The 6845 generates timing and addresses and
nothing else.** It emits a 14-bit memory address (MA) and a 5-bit row
address (RA) per character clock; turning those into pixels — the character
ROM, the attribute handling, the pixel serializer — is entirely the host
board's external logic. So a 6845 reference settles our *geometry and
cursor* questions and can say nothing about T27's attribute selector.

- **<https://book.martypc.net/display-graphics/6845>** — the more useful of
  the two. Register-by-register semantics, the VMA counter's load/increment/
  reset rules, R5 vertical total adjust, the cursor-visible condition (VMA
  matches R14/R15 **and** the scanline is within R10–R11 **and** blink
  allows it), and the fact that an out-of-range register select is a no-op
  on write and implementation-defined on read. Written against IBM CGA/MDA,
  which is a different board, and that turns out to be the interesting part
  — see below.
- **<https://github.com/obscuredcode/mc6845-emulation>** — a 219-line
  Verilog module for a DE10-Lite FPGA, self-described as a toy, no licence,
  last touched March 2024. Not reusable here (wrong language, wrong target,
  and unlicensed), but useful as a **second independent reading of the
  datasheet**: its register decode shows read paths for R14/R15 and R16/R17
  only, and 6-bit high bytes on the 14-bit pairs. That corroboration is why
  the `$09` read behaviour above was changed.

**The genuinely useful thing they gave us is a ruled-out hypothesis.** On
CGA/MDA the board makes each 6845 address fetch a **16-bit** word — a
character byte plus an attribute byte — and exposes them to the CPU
interleaved at `2N` / `2N+1`. That is the industry-standard way to attach
attributes to a 6845, and it is emphatically **not** what the P2500 does:
our own traces put characters at consecutive addresses (`$8000`, `$8001`,
…) with the screen reading contiguously out of a `--dump-vram`. The reason
is visible in the chip count — the P2500's word is **12** bits, not 16, and
a 12-bit word cannot be split into two CPU-addressable bytes. So the CGA
trick is unavailable to this board, which is exactly why its attribute
nibble must be reached out-of-band. That strengthens T27's conclusion
rather than changing it.

---

## Reference: RAM addresses

Established by hand-tracing. A dump of `$FE00`–`$FEFF` at any breakpoint is
the highest-value single debugging artifact this emulator produces.

### IPL era (`$FE00`-based scratch area)

| Address | Contents |
|---|---|
| `$FE00`–`$FE60` | IM2 vector table + trampolines, copied from ROM `$01FB` at `$01EB` |
| `$FE04`/`$FE05`/`$FE06` | `JP nn` trampoline for vector `$00`; operand is runtime-patched (`$0737` → `$0752`, `$0856` → `$0883`, `$08C6` → `$08EB`) |
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
| `$FED5` | The flag the two busy-waits (`$06C6`, `$0798`) spin on |
| `$FED6` | Drive-mode flag (selects `$52`/`$56` vs `$42`/`$46` on port `$11`) |
| `$FED8` | Shadow of PIO port A output data (`$F0`; `OR $08`/`OR $0C` before `OUT ($10)`) |
| `$FEDD` | µPD765 command opcode for the pending operation (`$06` read / `$05` write / `$0D` format) |
| `$FF26` | Saved SP — IPL interrupt handlers exit via `LD SP,($FF26)` + `JP`, **not** `RET`/`RETI` |

### CP/M era (CBIOS / SYSPBI, `$E200`–`$FFFF`)

All read out of a live `--dump-ram` image with `tools/disasm_ram.sh` — the
on-disk `.phi` files are sector-interleaved, so the `org 0` listings in
`../Disk Images/disassembly/` cannot be used for addresses.

| Address | Contents |
|---|---|
| `$E200` | CP/M 2.2 CBIOS jump table (BOOT/WBOOT/CONST/CONIN/CONOUT/LIST/…) |
| `$E274` | Special-key translation table (cursor diamond + dead-key diacritics; see T38) |
| `$E34C` | **Shift-lock mask.** CONIN XORs alphabetic input with it (`$E4B7`-`$E4BB`); it holds `$20`, so an unshifted key produces uppercase. Authentic - see T38 |
| `$E46C` | **CONST** — returns 0 unless `($E551)` != `$FF` and `($E553)` != `($0020)` |
| `$E48C` | **CONIN** — `CALL $E46C` / `JR Z` until CONST reports a character |
| `$E4C3` | **CONOUT** — ESC state machine at `$E34D`, translation table at `$E257`, then posts the byte as a request. Never touches an attribute (T27) |
| `$EB14` / `$EB15` | Bank-latch shadow and bank-stack pointer. `$FF2F`/`$FF49`/`$FF65`/`$FF80` push/pop around video access — only `$08` and `$0F` are ever pushed |
| `$E54F` | Console-read request block: `[id][?][status][?][byte]`, status `$FF` = pending |
| `$EAC0` | Generic "post a request, then spin until its status changes" |
| `$EB5C` | Event-slot table, 3-byte records `{id, ptr_lo, ptr_hi}` |
| `$EBA0` | Copies a request's count + destination into a driver's state block |
| `$EBAD` | Start a buffered read: drain the ring first, else record the request as pending and return `$FF` |
| `$EBDD` | IM2 vector-slot allocator — returns `($EC9C) + BC` |
| `$EBE2` | **`EI / RETI`** — how every CBIOS handler releases the daisy chain |
| `$EBE5` / `$EBFB` | Circular-buffer push / pop. Header is `[count][write_idx][read_idx]`, 32 bytes of data at `+3` |
| `$EC10` | Request-completion signal (`$EC17`'s `INC (HL)`) |
| `$ECDE` | CTC channel 3 (keyboard) init — vector base to `$00`, then `$C5`/tc=1 to `$03` |
| `$ED02` | Keyboard ring-buffer init: zeroes the 3-byte header at `$ED8F`. **Runs late** — anything strobed in before this is discarded |
| `$ED2A` → `$ED2D` | **CTC channel 3 ISR** — one `IN A,($06)`, push to the ring at `$ED8F` |
| `$ED8B` | Console-read state: `[flags][count][dest_lo][dest_hi]`, flag bit 0 = read outstanding |
| `$ED8F` | Keyboard ring buffer (header + 32 bytes) |
| `$EE1C` | CTC channel 2 (clock) init — `$D5`/tc=1 to `$02` |
| `$F37F` → `$F382` | **CTC channel 2 ISR** — increments the 24-bit tick counter at `$F436` |
| `$F436` | 24-bit real-time tick counter (50 Hz) |
| `$F546` | Baud-rate table, 7 × `{control, time constant}` — decodes to 75/110/150/300/600/1200/2400 baud, each within 0.3% |
| `$F597` → `$F59A` | **CTC channel 0 ISR** — serial transmit bit clock |
| `$F669` → `$F66C` | **CTC channel 1 ISR** — serial receive bit sampler |
| `$F727` | Baud index into `$F546` (5 = 1200 baud on this disk) |
| `$F731`/`$F732` | Live copy of the selected baud table entry |
| `$F733` | Serial flags; bit 6 = "check the port `$05` bit 6 handshake" |
| `$F73D` | Serial receive shift register |
| `$EDD5` | **The screen driver** (device id `$31`), function table at `$EDFC`; function `$05` (write) is `$EE91` |
| `$F170` | **Escape command table**, 13 entries. `$F199` is the alternate set armed by `ESC 3` |
| `$F1D1` | **`ESC 0`** - arms a one-parameter state; `$F252` decodes the parameter into the attribute nibble |
| `$F43B` | Current attribute byte; its low nibble is what reaches port `$0A` |
| `$F454` | `3E 00 D3 0A` - the `LD A,<nibble> / OUT ($0A),A` template the driver copies into its code queue |
| `$F475` | Write pointer into the driver's generated-code queue at `$FD23`, which it terminates with `$C9` and then calls |
| `$FF90` | CBIOS IM2 table at `I=$FF`: `$FF0C`/`$FF15`/`$FF03`/`$FEFA` for vectors `$90`/`$92`/`$94`/`$96` |
| `$FF0C`/`$FF15`/`$FF03`/`$FEFA` | Bank-switching stubs that self-patch the `JP` at `$FF51` with the real handler |

---

## Reference: invariants the harness asserts

Scoped by era — CP/M replaces page zero and the IM2 table wholesale, so an
IPL-era assertion left unscoped will fire on a *correct* boot.

**While the IPL owns memory:**

- `$0000`–`$0002` = `C3 00 01` (`JP $0100`) and `$0003`–`$0005` = `C3 DA 02`
  (`JP $02DA`) whenever port `$05` bit 3 is clear. Anything else is memory
  corruption.
- Register `I` = `$FE` before any IM2 interrupt is delivered. **IPL only** —
  CBIOS builds its own table at `I=$FF`, which is why `machine.c` reads `I`
  live rather than assuming a page.
- Port `$14` reads exactly `$80` when idle (ROM `$0498` compares for
  equality, not a bit test).

**Once CP/M is up** — all of these are asserted by `make test`:

- `$0000`–`$0002` = `C3 03 E2` (CBIOS warm boot) and `$0005`–`$0007` =
  `C3 06 D4` (BDOS entry). Nothing a stray NOP-sled can fake.
- `$E200`–`$E20F` is CP/M 2.2's standard BIOS jump table, all `JP nn`.
- `I` = `$FF`, IM 2.
- The screen reads `Philips P2500` / `58K CP/M Ver. 2.2` / `A>`, and typing
  `dir\r` lists PIP, SYSGEN, SYSCBI, SYSLOAD, SYSPBI and SYSCPM — the six
  files the disk image actually contains.
- **Every interrupt request is acknowledged exactly once.** The exit report
  prints `requests/acknowledged` per device; the two diverging means the
  daisy chain is dropping something, which is the failure mode that cost
  this project the most time.
