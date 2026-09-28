# Disk images

The P2500 media that is **known to work in this emulator**, checked by
booting each one. Flat sector dumps of the original ImageDisk files:
single-sided, 16 sectors of 256 bytes per track. CP/M uses 77 tracks
(315,392 bytes); the three 327,680-byte dumps were read out to 80.

| Image | Boots? | Contents |
|---|---|---|
| `P25K_B.raw` | yes | Minimal CP/M 2.2 system disk — the four `SYS*.PHI` files, `PIP`, `SYSGEN`. The reference disk `make test` runs against. |
| `P25K_S.raw` | yes | **SuperCalc2** (an OEM build whose splash reads `PHILIPS P2000`), plus SuperCalc 1 and a former owner's Austrian business spreadsheets. `SC.COM` is present but fails to load; `SC2` works. |
| `P25TEST.raw` | yes | A development disk — MACRO-80 (`M80`/`L80`/`T80`), `ASM`/`DDT`/`STAT`, **MBASIC** 5.21, Turbo Pascal, the VDE 2.66 editor, `NSWEEP`. |
| `P2500GAM.raw` | no | Data disk: 14 BASIC games (Star Trek, Hammurabi, Blackjack, Valley…). Mount it in drive B and run them from MBASIC. |

```sh
./p2500-gui --disk disks/P25K_B.raw --disk-b disks/P2500GAM.raw
./p2500-emu --disk disks/P25TEST.raw --disk-b disks/P2500GAM.raw \
            --push-at '4000:mbasic b:valley\r' --dump-screen valley.ppm
```

Writing is not implemented yet (`TODO.md` P3), so these images are only ever
read — nothing here can be modified by the guest.

## What is not here

The `.IMD` originals, the five images that do not work, and every analysis
writeup stay in `../../Disk Images/`. Of those five, `P25K_G` is the
interesting one: it is a clean dump that *should* boot and does not, which
makes it our bug (`TODO.md` T42). The other four were read by a drive that
stepped twice per track, so half of every disk was never captured (T45).

`demos/P2500DEMO.raw` is built by `make demos`, and `docs/porting-cpm-software.md`
builds an Othello disk from an upstream release — both are outputs, so
neither is committed here.
