# Porting CP/M software to the P2500 / P2000B

The CP/M layer is not the problem. A `.COM` built for another CP/M 2.2
machine loads and runs here unmodified — `zcc +cpm -create-app` output from
[z88dk](https://github.com/z88dk/z88dk) needs no relocation and no
per-machine glue, and BDOS/BIOS calls behave as CP/M specifies.

What differs is the **screen control codes**. They are close enough to be
misleading when compared to the P2000M: two of the five sequences a typical
full-screen CP/M program uses are identical on both machines, and some are
silently ignored rather than producing an error the program can see.

## The substitution table

Taking [ifilot/p2000m-othello](https://github.com/ifilot/p2000m-othello) as
the working example, since it is deliberately minimal — cursor addressing,
inverse video, two erases. The binary used below is `OTHELLO.COM` from the
[v1.0.0 release](https://github.com/ifilot/p2000m-othello/releases/tag/v1.0.0),
not vendored here, so fetch it to reproduce any of this:

```
curl -LO https://github.com/ifilot/p2000m-othello/releases/download/v1.0.0/OTHELLO.COM
```


| Purpose | P2000M | P2500 / P2000B | |
|---|---|---|---|
| Absolute cursor address | `ESC Y row+32 col+32` | `ESC Y row+32 col+32` | **unchanged** |
| Erase to end of line | `ESC K` | `ESC K` | **unchanged** |
| Cursor home | `ESC H` | *does not exist* — use `ESC Y $20 $20` | change |
| Erase to end of page | `ESC J` | `ESC k` — lowercase | change |
| Inverse video on | `ESC p` | `ESC 0 P` | change |
| Inverse video off | `ESC q` | `ESC 0 @` | change |

So a clear-screen becomes:

```c
/* P2000M */                    /* P2500 */
putchar(ESC); putchar('H');     putchar(ESC); putchar('Y');
putchar(ESC); putchar('J');     putchar(' '); putchar(' ');   /* row 0, col 0 */
                                putchar(ESC); putchar('k');
```

and the inverse-video pair gains a parameter byte:

```c
/* P2000M */                              /* P2500 */
putchar(ESC); putchar(on ? 'p' : 'q');    putchar(ESC); putchar('0');
                                          putchar(on ? 'P' : '@');
```

### The trap

`ESC p` on the P2000M means inverse video. On the P2500, `p` *is* a valid
`ESC 0` parameter — it means **reverse plus underline**. A port that keeps
the letter and only adds the `0` gets underlined text, which looks like a
font problem rather than a code problem.

The `ESC 0` parameter is a bitmask laid over `$40`:

| bit | value | attribute |
|---|---|---|
| 0 | `$01` | low intensity |
| 1 | `$02` | flash |
| 4 | `$10` | reverse |
| 5 | `$20` | underline |

which gives exactly sixteen legal parameters —
`@ A B C P Q R S` and `` ` a b c p q r s`` — and `P` (`$50`, bit 4 alone) is
plain reverse. Verified by probe: `ESC 0 P` sets attribute nibble `4`
(`P2500_ATTR_REVERSE`), `ESC 0 B` sets `8` (`P2500_ATTR_FLASH`), `ESC 0 @`
clears it. The attribute applies to characters written *after* the sequence,
which is the same model `ESC p` has.

### Everything else the P2500 offers

From CBIOS's dispatch table at `$F170`, matching the P2219 manual's CONTROL
CODES (SCREEN) page. Useful additions for a full-screen program:

| Sequence | Meaning |
|---|---|
| `ESC C` / `ESC c` | cursor visible / invisible — worth using while redrawing |
| `ESC 0 Z` | reverse the whole screen |
| `ESC S` / `ESC T` | roll up / down one line |
| `ESC U` / `ESC V` | next / previous page |
| `ESC 1` / `ESC 2` | start / end mosaic block graphics |
| `ESC 3` / `ESC 4` | start / end 512×256 high-resolution graphics |

An unrecognised sequence returns error code `$0D` to the driver and puts
nothing on screen — which is why a P2000M binary here loses its screen
clears and its inverse video without complaining. The screen is 80×24 on
both machines, so no layout changes are needed.

## How big a program may be

**405 records — 51,840 bytes.** A `.COM` of 406 records is rejected by
CP/M's CCP with `BAD LOAD` before a single instruction of it runs.

Measured, not computed: `tools/mk_cpm_probe.py --pad-records N` builds a
program of an exact size, and a bisection over N on the 58K CP/M these disks
carry puts the cutoff between 405 (runs) and 406 (`BAD LOAD`). The limit is
the CCP's own base at `$CC00`, not the BDOS base at `$D400` — CP/M will not
let a program overwrite the CCP *while loading*, even though a running
program may use that memory afterwards. So the usable load region is
`$0100`–`$CB7F`, about 3.5 KB less than the arithmetic on the BDOS entry
vector suggests.

The number belongs to this build. `CONFIG` can produce a 55K or a 58K
system (`TODO.md` T45), and a different system size moves the CCP.

## Getting a program onto a disk

The P2500 boots from 5¼-inch floppies as A:–D:.
To make an image this emulator (or real hardware) can use:

```
tools/cpm_build.py othello.raw path/to/OTHELLO.COM \
    --boot-from "../Disk Images/extracted/P25K_B/P25K_B.raw"
./p2500-gui --disk othello.raw
```

`--boot-from` is needed because a bootable P2500 disk takes **two** things,
not one: the loader in the two reserved tracks, *and* the CP/M system as
ordinary directory files — `SYSCPM.PHI`, `SYSCBI.PHI`, `SYSPBI.PHI`,
`SYSLOAD.PHI`. A disk with the loader but no `SYS*.PHI` gets no further than
the loader. Omit `--boot-from` for a data disk to put in B: alongside a
boot disk.

## Status of the Othello port

Unported, `OTHELLO.COM` v1.0.0 straight from the release:

- **The game runs.** Title screen, board, move generation, the CPU player,
  the status panel and the key handling all work.
- **No screen clears**, so each new screen draws over the last one —
  the board ends up interleaved with the title screen and the CP/M banner.
- **No inverse video**, so the cursor cell and the legal-move markers are
  not distinguishable from an empty square. The attribute plane comes back
  entirely zero.

Both symptoms are the three changed sequences above and nothing else.

## What a port would involve

Small, and confined to one file — `src/othello.c` holds every escape
sequence the game emits; `game.c` and `cpu.c` are platform-neutral and would
not be touched.

- [ ] `clear_screen()` — `ESC H` + `ESC J` → `ESC Y $20 $20` + `ESC k`
- [ ] `inverse()` — `ESC p` / `ESC q` → `ESC 0 P` / `ESC 0 @`
      (`P`, not `p` — see the trap above)
- [ ] `gotoxy()` and `clear_to_eol()` — no change
- [ ] Optional: hide the cursor while redrawing with `ESC c`, restore with
      `ESC C`. The P2500's cursor is a solid blinking block and sits on top
      of the board while the game draws.
- [ ] Rebuild with `zcc +cpm -create-app`; nothing in the toolchain changes.
- [ ] Package with `tools/cpm_build.py --boot-from <a bootable P2500 image>`
      instead of the SD-image step in the upstream Makefile.

The cleanest shape is probably a two-line terminal abstraction — a `#define`
block or a small `term.h` selected at build time — since the two machines
agree on cursor addressing and 80×24, and differ only in these three
sequences. That would let one source tree build for both.
