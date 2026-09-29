# Porting CP/M software to the P2500 / P2000B

The CP/M layer is not the problem. A `.COM` built for another CP/M 2.2
machine loads and runs here unmodified — no relocation, no per-machine glue,
and BDOS/BIOS calls behave as CP/M specifies.

What differs between machines is the screen control codes.

## The full control-code table

All of CBIOS's escape dispatch table (`$F170`), which matches the P2219 CP/M
manual's CONTROL CODES (SCREEN) page entry for entry. Every sequence begins
with `ESC` (`$1B`).

| Sequence | Meaning |
|---|---|
| `ESC Y row+32 col+32` | absolute cursor address |
| `ESC K` | erase to end of line |
| `ESC k` | erase to end of page (**lowercase** — uppercase is a different code on some other CP/M machines) |
| `ESC C` / `ESC c` | cursor visible / invisible |
| `ESC S` / `ESC T` | roll up / down one line |
| `ESC U` / `ESC V` | next / previous page |
| `ESC 0 <c>` | set video attributes — see below |
| `ESC 0 Z` | reverse the whole screen |
| `ESC 1` / `ESC 2` | start / end mosaic block graphics |
| `ESC 3` / `ESC 4` | start / end 512×256 high-resolution graphics |

Anything else returns error code `$0D` to the driver and puts nothing on
screen. The screen in text mode is 80×24.

There is **no cursor-home code**. Where another machine has one (often
`ESC H`), use `ESC Y $20 $20` instead — absolute cursor address to row 0,
column 0 (the offsets are `+32` per axis, so `$20`/`$20` addresses `0,0`).

### Setting attributes: `ESC 0 <c>`

The parameter byte is a bitmask laid over `$40`:

| bit | value | attribute |
|---|---|---|
| 0 | `$01` | low intensity |
| 1 | `$02` | flash |
| 4 | `$10` | reverse |
| 5 | `$20` | underline |

which gives exactly sixteen legal parameters —
`@ A B C P Q R S` and `` ` a b c p q r s`` — and `P` (`$50`, bit 4 alone) is
plain reverse, `@` (`$40`, no bits) clears every attribute. Verified by
probe: `ESC 0 P` sets attribute nibble `4` (`P2500_ATTR_REVERSE`), `ESC 0 B`
sets `8` (`P2500_ATTR_FLASH`). The attribute applies to characters written
*after* the sequence.

## How big a program may be

**405 records — 51,840 bytes.** A `.COM` of 406 records is rejected by
CP/M's CCP with `BAD LOAD` before a single instruction of it runs.

## Getting a program onto a disk

The P2500 boots from 5¼-inch floppies as A:–D:.
To make an image this emulator (or real hardware) can use:

```
tools/cpm_build.py yourprog.raw path/to/PROGRAM.COM \
    --boot-from disks/P25K_B.raw
./p2500-gui --disk yourprog.raw
```

`--boot-from` is needed because a bootable P2500 disk takes **two** things,
not one: the loader in the two reserved tracks, *and* the CP/M system as
ordinary directory files — `SYSCPM.PHI`, `SYSCBI.PHI`, `SYSPBI.PHI`,
`SYSLOAD.PHI`. A disk with the loader but no `SYS*.PHI` gets no further than
the loader. Omit `--boot-from` for a data disk to put in B: alongside a boot
disk.

## Working out what a program does with the screen

If a program's screen behaviour is wrong after porting, the fastest way to
find out which sequence is at fault is to build a probe that emits exactly
the bytes in question and read back what landed on screen:

```
tests/mk_cpm_probe.py probe.COM 'BEFORE\eY\x20\x20\ek\e0PREVERSED\e0@ plain'
tools/cpm_build.py probe.raw probe.COM --boot-from disks/P25K_B.raw
p2500-emu --disk probe.raw --type-at '4000:probe\r' \
    --dump-vram out.bin --dump-vram-attr out.attr
```

`--dump-vram-attr` shows exactly which cells carry which attribute nibble,
which is how the table above was confirmed.
