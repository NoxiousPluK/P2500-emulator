#!/usr/bin/env python3
"""
Build a Philips P2500 CP/M disk image (.raw) from host files.

The exact inverse of tools/cpm_extract.py, and deliberately written against
that file's constants rather than its own copy of them: the sector skew and
the block-to-sector map are the two things this format gets wrong quietly,
and having one definition of each means the reader and the writer cannot
drift apart. Every build is verified by reading the result back through the
extractor and comparing bytes, so a wrong skew fails here rather than on the
machine.

The format, as measured off the live DPB at $E84D on a booted P25K_B:

    128-byte records, 16 x 256-byte sectors per track, 77 tracks
    BSH 4 / BLM 15  -> 2048-byte allocation blocks
    EXM 1           -> one directory entry spans two extents
    DSM 149         -> blocks 0..149; block 0 is the directory
    DRM 63          -> 64 directory entries
    AL0 $80         -> one directory block
    CKS 16          -> checksummed as removable media
    OFF 2           -> two reserved tracks ahead of the directory

Bootable disks need two things, not one. The reserved tracks carry the
loader the IPL jumps to, and the CP/M system itself lives in ordinary
directory files - SYSCPM.PHI, SYSCBI.PHI, SYSPBI.PHI, SYSLOAD.PHI - so a
disk with the loader but no SYS*.PHI boots no further than the loader does.
--boot-from copies both from a donor image.

usage:
    tools/cpm_build.py OUT.raw FILE [FILE ...] [--boot-from DONOR.raw]

    FILE may be HOSTPATH or HOSTPATH:NAME.EXT to rename it on the disk.

examples:
    # A data disk, readable in any drive.
    tools/cpm_build.py games.raw *.BAS

    # A disk that boots CP/M on its own, with one program added.
    tools/cpm_build.py othello.raw OTHELLO.COM \\
        --boot-from disks/P25K_B.raw
"""

import argparse
import contextlib
import io
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cpm_extract as cpm

SECTOR_SIZE = cpm.SECTOR_SIZE
SECTORS_PER_TRACK = cpm.SECTORS_PER_TRACK
RECORD_SIZE = cpm.RECORD_SIZE
DIR_ENTRY_SIZE = cpm.DIR_ENTRY_SIZE
SKEW = cpm.SKEW

TRACKS = 77          # OFF 2 + 150 blocks x 2048 / 4096 bytes per track
BLS = 2048
EXM = 1
OFF = 2              # reserved tracks; the directory starts here
DIR_BLOCKS = 1       # AL0 $80
TOTAL_BLOCKS = 150   # DSM 149, inclusive
DIR_ENTRIES = 64     # DRM 63, inclusive

SECS_PER_BLOCK = BLS // SECTOR_SIZE          # 8
RECS_PER_BLOCK = BLS // RECORD_SIZE          # 16
BLOCKS_PER_ENTRY = 16                        # 8-bit block numbers, bytes 16..31
RECS_PER_ENTRY = BLOCKS_PER_ENTRY * RECS_PER_BLOCK   # 256 = two extents

ERASED = 0xE5        # what CP/M leaves on unused media
SYSTEM_FILES = ("SYSCPM.PHI", "SYSCBI.PHI", "SYSPBI.PHI", "SYSLOAD.PHI")


class Builder:
    def __init__(self, tracks=TRACKS):
        self.tracks = tracks
        self.data = bytearray([ERASED] * (tracks * SECTORS_PER_TRACK * SECTOR_SIZE))
        self.next_block = DIR_BLOCKS      # block 0 is the directory
        self.entries = []                 # 32-byte directory records
        self.added = []                   # (name, size, records, blocks) for the report

    # -- geometry ---------------------------------------------------------
    def put_logical_sector(self, track, lsec, payload):
        """Write one sector addressed the way CP/M addresses it."""
        assert len(payload) == SECTOR_SIZE
        phys = SKEW[lsec]
        off = (track * SECTORS_PER_TRACK + phys) * SECTOR_SIZE
        self.data[off:off + SECTOR_SIZE] = payload

    def put_block(self, block, payload):
        """Blocks are numbered from the directory track, not from track 0."""
        assert len(payload) == BLS
        first = block * SECS_PER_BLOCK
        for s in range(SECS_PER_BLOCK):
            ls = first + s
            self.put_logical_sector(OFF + ls // SECTORS_PER_TRACK,
                                    ls % SECTORS_PER_TRACK,
                                    payload[s * SECTOR_SIZE:(s + 1) * SECTOR_SIZE])

    def copy_reserved(self, donor: bytes):
        """The loader tracks, copied physically - no skew, it is a track image."""
        n = OFF * SECTORS_PER_TRACK * SECTOR_SIZE
        if len(donor) < n:
            raise SystemExit("donor image is smaller than its own reserved area")
        if all(b == ERASED for b in donor[:n]):
            raise SystemExit("donor's reserved tracks are blank - it is not bootable")
        self.data[:n] = donor[:n]

    # -- files ------------------------------------------------------------
    def add_file(self, content: bytes, name: str, ext: str, user: int = 0):
        # CP/M stores whole records and no exact length. Text pads with ^Z
        # because that is where a CP/M text reader stops; anything else pads
        # with zero, which for a .COM is the harmless choice.
        pad = 0x1A if ext.upper() in cpm.TEXT_EXTS else 0x00
        if len(content) % RECORD_SIZE:
            content += bytes([pad]) * (RECORD_SIZE - len(content) % RECORD_SIZE)
        records = len(content) // RECORD_SIZE
        if records == 0:
            raise SystemExit(f"{name}.{ext} is empty")

        blocks = []
        for i in range(0, len(content), BLS):
            chunk = content[i:i + BLS].ljust(BLS, bytes([pad]))
            if self.next_block >= TOTAL_BLOCKS:
                raise SystemExit("disk full - out of allocation blocks")
            self.put_block(self.next_block, chunk)
            blocks.append(self.next_block)
            self.next_block += 1

        # One directory entry per 256 records. Within an entry, EX's low bit
        # is the second extent's flag and RC counts the records above it -
        # which is exactly how the extractor reads it back:
        #     records = (EX & EXM) * 128 + RC
        for i in range(0, records, RECS_PER_ENTRY):
            in_entry = min(RECS_PER_ENTRY, records - i)
            full_ext = (i // RECS_PER_ENTRY) * (EXM + 1)
            if in_entry > 128:
                full_ext += 1
                rc = in_entry - 128
            else:
                rc = in_entry
            mine = blocks[i // RECS_PER_BLOCK:
                          i // RECS_PER_BLOCK + BLOCKS_PER_ENTRY]
            e = bytearray([0] * DIR_ENTRY_SIZE)
            e[0] = user
            e[1:9] = name.upper().ljust(8)[:8].encode("ascii")
            e[9:12] = ext.upper().ljust(3)[:3].encode("ascii")
            e[12] = full_ext & 0x1F
            e[13] = 0
            e[14] = (full_ext >> 5) & 0x3F
            e[15] = rc
            for bi, b in enumerate(mine):
                e[16 + bi] = b
            if len(self.entries) >= DIR_ENTRIES:
                raise SystemExit("directory full - 64 entries is the limit")
            self.entries.append(bytes(e))

        self.added.append((f"{name}.{ext}", len(content), records, len(blocks)))

    def finish(self) -> bytes:
        dirblk = bytearray()
        for e in self.entries:
            dirblk += e
        dirblk += bytes([ERASED]) * (BLS - len(dirblk))
        self.put_block(0, bytes(dirblk))
        return bytes(self.data)


def read_donor_files(donor_path: Path, wanted):
    """Pull files out of an existing image, through the extractor itself.

    keep_padding is on: these are binaries, and the extractor's trailing-^Z
    and trailing-zero trim is meant for text. Trimming a boot file's tail
    and then padding it back would quietly change its record count.
    """
    data = donor_path.read_bytes()
    img = cpm.Image(data)
    out = {}
    with tempfile.TemporaryDirectory() as td:
        with contextlib.redirect_stdout(io.StringIO()):
            cpm.extract(img, Path(td), keep_padding=True, quiet=True)
        for f in Path(td).iterdir():
            if f.name.upper() in wanted:
                out[f.name.upper()] = f.read_bytes()
    return out


def split_spec(spec: str):
    """HOSTPATH or HOSTPATH:NAME.EXT."""
    host, _, as_name = spec.partition(":")
    src = Path(host)
    target = as_name if as_name else src.name
    stem, _, ext = target.rpartition(".")
    if not stem:
        stem, ext = target, ""
    if len(stem) > 8 or len(ext) > 3:
        raise SystemExit(f"{target}: CP/M names are 8.3 - pass HOSTPATH:NAME.EXT")
    return src, stem.upper(), ext.upper()


def verify(image: bytes, expected):
    """Read the result back with the extractor and compare byte for byte."""
    img = cpm.Image(image)
    with tempfile.TemporaryDirectory() as td:
        with contextlib.redirect_stdout(io.StringIO()) as buf:
            problems = cpm.extract(img, Path(td), keep_padding=True, quiet=True)
        bad = []
        if problems:
            bad.append(f"the extractor reported {problems} problem(s):\n"
                       + "\n".join("    " + l for l in buf.getvalue().splitlines()
                                   if "!!" in l))
        for name, content in expected.items():
            got = (Path(td) / name).read_bytes() if (Path(td) / name).exists() else None
            if got is None:
                bad.append(f"{name}: not found when read back")
            elif got[:len(content)] != content:
                at = next((i for i in range(min(len(got), len(content)))
                           if got[i] != content[i]), None)
                bad.append(f"{name}: differs at offset {at}")
    return bad


def main():
    ap = argparse.ArgumentParser(
        description="Build a P2500 CP/M disk image.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("usage:")[-1])
    ap.add_argument("out", help="image to write (.raw)")
    ap.add_argument("files", nargs="*", help="HOSTPATH or HOSTPATH:NAME.EXT")
    ap.add_argument("--boot-from", metavar="DONOR",
                    help="copy the reserved tracks and SYS*.PHI from a bootable image")
    ap.add_argument("--tracks", type=int, default=TRACKS,
                    help=f"total tracks (default {TRACKS})")
    ap.add_argument("--user", type=int, default=0, help="CP/M user number (default 0)")
    ap.add_argument("--no-verify", action="store_true",
                    help="skip reading the result back (do not)")
    args = ap.parse_args()

    b = Builder(args.tracks)
    expected = {}

    if args.boot_from:
        donor = Path(args.boot_from)
        if not donor.exists():
            raise SystemExit(f"{donor}: not found")
        b.copy_reserved(donor.read_bytes())
        sysfiles = read_donor_files(donor, set(SYSTEM_FILES))
        missing = [f for f in SYSTEM_FILES if f not in sysfiles]
        if missing:
            raise SystemExit(f"{donor} has the loader but not {', '.join(missing)} "
                             "- it cannot make another disk bootable")
        for fn in SYSTEM_FILES:
            stem, _, ext = fn.partition(".")
            b.add_file(sysfiles[fn], stem, ext, args.user)
            expected[fn] = sysfiles[fn]
        print(f"System     : reserved tracks + {len(SYSTEM_FILES)} SYS files from {donor.name}")

    for spec in args.files:
        src, stem, ext = split_spec(spec)
        if not src.exists():
            raise SystemExit(f"{src}: not found")
        content = src.read_bytes()
        b.add_file(content, stem, ext, args.user)
        expected[f"{stem}.{ext}" if ext else stem] = content

    if not b.entries:
        raise SystemExit("nothing to put on the disk")

    image = b.finish()
    Path(args.out).write_bytes(image)

    print(f"Image      : {args.tracks} tracks x {SECTORS_PER_TRACK} x {SECTOR_SIZE}"
          f" = {len(image)} bytes")
    print(f"Directory  : {len(b.entries)} of {DIR_ENTRIES} entries used")
    print(f"Allocation : blocks {DIR_BLOCKS}..{b.next_block - 1} of {TOTAL_BLOCKS - 1}"
          f" ({(TOTAL_BLOCKS - b.next_block) * BLS // 1024} KB free)")
    for name, size, records, blocks in b.added:
        print(f"  {name:<14} {size:6d} bytes  {records:4d} records  {blocks:3d} blocks")

    if args.no_verify:
        print("\nNot verified (--no-verify).")
        return 0
    bad = verify(image, expected)
    if bad:
        print("\nVERIFY FAILED:")
        for line in bad:
            print("  " + line)
        return 1
    print(f"\nVerified   : read back through cpm_extract, {len(expected)} file(s) "
          "byte-for-byte identical.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
