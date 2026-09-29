# Disk images

The P2500 media that is known to work in this emulator.
Flat sector dumps of the original ImageDisk files:
single-sided, 16 sectors of 256 bytes per track. CP/M uses 77 tracks
(315,392 bytes); the three 327,680-byte dumps were read out to 80.

| Image | Boots? | Contents |
|---|---|---|
| `P25K_B.raw` | yes | Minimal CP/M 2.2 system disk — the four `SYS*.PHI` files, `PIP`, `SYSGEN`. The reference disk `make test` runs against. |
| `P25K_S.raw` | yes | **SuperCalc2** (an OEM build whose splash reads `PHILIPS P2000`) and a former owner's spreadsheets. |
| `P25TEST.raw` | yes | A development disk — MACRO-80 (`M80`/`L80`/`T80`), `ASM`/`DDT`/`STAT`, **MBASIC** 5.21, Turbo Pascal, the VDE 2.66 editor, `NSWEEP`. |
| `P2500GAM.raw` | no | Data disk: 14 BASIC games (Star Trek, Hammurabi, Blackjack, Valley…). Mount it in drive B and run them from MBASIC. |

Use as (for example):

```sh
./p2500-gui --disk disks/P25TEST.raw --disk-b disks/P2500GAM.raw
```

`../demos/P2500DEMO.raw` is built by `make demos` - these are some custom made programs to test high resolution mode or benchmark the system.

Sourced from [the Home Computer Museum](https://download.homecomputer.museum/#Files%2FPhilips%2FP2500).