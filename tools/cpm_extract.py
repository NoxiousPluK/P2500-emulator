#!/usr/bin/env python3
"""
Extract files from a Philips P2500 CP/M disk image (.raw).

Why this exists rather than cpmtools: the P2500's sectors are physically
interleaved on the track, and a flat .raw is in *physical* order. An
extractor that ignores that reads every other sector from the wrong place.
This tool derives the format from the image and then *checks* what it
produced.

What is derived rather than assumed:

  * Reserved tracks   - found by scanning for a track whose skewed logical
                        sector 0 parses as a CP/M directory.
  * Block size / EXM  - chosen from the candidates by requiring that EVERY
                        directory entry's record count predicts exactly the
                        number of allocation blocks it actually lists.
  * Sector skew       - the one thing taken as given (see SKEW below), and
                        it is verified: a wrong skew makes the directory
                        scan fail outright, because directory and file data
                        alternate physically.

Verification, printed per file:
  * allocation blocks must be in range and never claimed by two files
  * extents must be contiguous - no gaps
  * text files must be essentially all printable
  * every block boundary inside a text file must have valid text on both
    sides. This is the check that actually catches a wrong sector map:
    a misplaced block shows up as garbage or padding at a 2048-byte
    boundary, while a correct one splices mid-word ("QUAD.NAME" + "$(04)=").

  Note on what is NOT checked: BASIC line numbers are not required to
  increase. These listings are structured BASIC with labels, and
  subroutines legitimately appear after their callers - an ordering check
  flags four false positives on STARTREK.BAS alone.

usage: tools/cpm_extract.py IMAGE.raw OUTDIR [--keep-padding] [--quiet]
                                   [--double-step]

--double-step salvages a dump that captured only every second track (see
tools/imd_tool.py verify). Logical track t then lives at index t/2 and odd
t is a hole, zero-filled; each file reports how much of it survived.
"""

import sys
import re
from pathlib import Path

SECTOR_SIZE = 256
SECTORS_PER_TRACK = 16
RECORD_SIZE = 128
DIR_ENTRY_SIZE = 32

# Physical sector order on a P2500 track: the even physical sectors carry the
# first half of the logical sequence, the odd ones the second half. Measured
# directly on P2500GAM - its directory (block 0, eight 256-byte sectors) lands
# on physical sectors 0,2,4,6,8,10,12,14 of the directory track, while the
# odd ones hold the first data block, whose BASIC line numbers then run in
# order across 1,3,5,7,9,11,13,15.
SKEW = [(l * 2) % SECTORS_PER_TRACK + (l * 2) // SECTORS_PER_TRACK
        for l in range(SECTORS_PER_TRACK)]

# (block size, extent mask). One directory entry covers EXM+1 extents.
BLS_CANDIDATES = [(2048, 1), (1024, 0), (4096, 3), (16384, 15)]


class Image:
    def __init__(self, data: bytes, double_step: bool = False):
        self.data = data
        self.tracks = len(data) // (SECTOR_SIZE * SECTORS_PER_TRACK)
        # A double-stepped dump holds only the EVEN logical tracks, stored
        # consecutively. CP/M logical track t is read as FDC cylinder t + 1
        # (the Philips +1 convention), and these images carry ID cylinders
        # 1,3,5,...,79 - so t is present exactly when t is even, at index
        # t/2. Everything else is a hole.
        self.double_step = double_step

    def raw_track(self, logical: int):
        """Index of a logical track in this image, or None if never captured."""
        if not self.double_step:
            return logical if 0 <= logical < self.tracks else None
        if logical % 2:
            return None
        idx = logical // 2
        return idx if idx < self.tracks else None

    def logical_sector(self, track: int, lsec: int) -> bytes:
        """Sector by LOGICAL track; b"" when that track was never captured."""
        idx = self.raw_track(track)
        if idx is None:
            return b""
        phys = SKEW[lsec]
        off = (idx * SECTORS_PER_TRACK + phys) * SECTOR_SIZE
        return self.data[off:off + SECTOR_SIZE]


def entry_is_plausible(e: bytes) -> bool:
    # A deleted entry is only marked by user byte $E5 - the rest of the slot
    # keeps the old filename and allocation list. Rejecting those made every
    # CP/M system disk here look like it had no directory at all.
    if e[0] == 0xE5:
        return True
    if e[0] > 0x0F:
        return False                     # user number out of range
    return all(0x20 <= (b & 0x7F) < 0x7F for b in e[1:12])


def safe_name(name: str) -> str:
    """CP/M allows bytes in a filename that a host filesystem does not."""
    return "".join(c if c.isalnum() or c in "-_.$!#%&()@^{}~" else "_" for c in name)


def find_directory_track(img: Image) -> int:
    """First LOGICAL track whose sector 0 parses cleanly as a directory."""
    for track in range(min(6, img.tracks * (2 if img.double_step else 1))):
        sec = img.logical_sector(track, 0)
        if not sec:
            continue
        entries = [sec[i:i + DIR_ENTRY_SIZE] for i in range(0, SECTOR_SIZE, DIR_ENTRY_SIZE)]
        if all(entry_is_plausible(e) for e in entries) and not all(
                all(b == 0xE5 for b in e) for e in entries):
            return track
    raise SystemExit("no CP/M directory found - wrong image, or the skew is wrong")


def read_directory(img: Image, track: int, blocks: int = 1, bls: int = 2048):
    """Read the directory area (block 0) as a list of 32-byte entries."""
    secs_per_block = bls // SECTOR_SIZE
    raw = b""
    for i in range(blocks * secs_per_block):
        raw += img.logical_sector(track + i // SECTORS_PER_TRACK, i % SECTORS_PER_TRACK)
    return [raw[i:i + DIR_ENTRY_SIZE] for i in range(0, len(raw), DIR_ENTRY_SIZE)]


def live_entries(entries):
    return [e for e in entries if e[0] <= 0x0F and not all(b == 0xE5 for b in e)]


def entry_blocks(e: bytes, wide: bool):
    if wide:
        nums = [e[16 + 2 * i] | (e[17 + 2 * i] << 8) for i in range(8)]
    else:
        nums = list(e[16:32])
    return [n for n in nums if n]


def derive_format(entries, total_blocks_guess):
    """Pick (bls, exm) by requiring record counts to predict block counts."""
    live = live_entries(entries)
    for bls, exm in BLS_CANDIDATES:
        recs_per_block = bls // RECORD_SIZE
        wide = total_blocks_guess(bls) > 256
        ok = True
        for e in live:
            ex, rc = e[12], e[15]
            records = (ex & exm) * 128 + rc
            want = -(-records // recs_per_block)      # ceil
            if want != len(entry_blocks(e, wide)):
                ok = False
                break
        if ok and live:
            return bls, exm, wide
    raise SystemExit("could not derive a block size that explains the directory")


def extract(img: Image, outdir: Path, keep_padding: bool, quiet: bool) -> int:
    dir_track = find_directory_track(img)
    entries = read_directory(img, dir_track)
    live = live_entries(entries)

    data_tracks = img.tracks - dir_track
    derive_total = lambda bls: data_tracks * SECTORS_PER_TRACK * SECTOR_SIZE // bls
    bls, exm, wide = derive_format(entries, derive_total)
    total_blocks = derive_total(bls)
    secs_per_block = bls // SECTOR_SIZE
    recs_per_block = bls // RECORD_SIZE

    print(f"Image      : {img.tracks} tracks x {SECTORS_PER_TRACK} sectors x {SECTOR_SIZE} bytes")
    print(f"Reserved   : {dir_track} track(s); directory on track {dir_track}")
    print(f"Derived    : block size {bls}, EXM {exm}, "
          f"{'16' if wide else '8'}-bit block numbers, {total_blocks} blocks")
    deleted = sum(1 for e in entries
                  if e[0] == 0xE5 and not all(b == 0xE5 for b in e))
    print(f"Directory  : {len(entries)} slots, {len(live)} in use"
          + (f", {deleted} deleted (not extracted)" if deleted else "") + "\n")

    # group entries by (user, name)
    files = {}
    for e in live:
        name = "".join(chr(b & 0x7F) for b in e[1:9]).rstrip()
        ext = "".join(chr(b & 0x7F) for b in e[9:12]).rstrip()
        key = (e[0], name, ext)
        full_ext = ((e[14] & 0x3F) << 5) | (e[12] & 0x1F)
        files.setdefault(key, []).append((full_ext, e))

    claimed = {}
    problems = 0
    outdir.mkdir(parents=True, exist_ok=True)

    for (user, name, ext), parts in sorted(files.items()):
        parts.sort(key=lambda p: p[0])
        blob = bytearray()
        blocks_used = []
        missing = [0]  # bytes that fell on tracks this dump never captured
        for full_ext, e in parts:
            base_rec = (full_ext & ~exm) * 128
            records = (e[12] & exm) * 128 + e[15]
            blks = entry_blocks(e, wide)
            if len(blob) != base_rec * RECORD_SIZE:
                print(f"  !! {name}.{ext}: extent gap at extent {full_ext}")
                problems += 1
                blob.extend(b"\x00" * (base_rec * RECORD_SIZE - len(blob)))
            need = records
            for bi, b in enumerate(blks):
                if b >= total_blocks:
                    print(f"  !! {name}.{ext}: block {b} past end of disk")
                    problems += 1
                    continue
                owner = claimed.get(b)
                if owner and owner != (name, ext):
                    print(f"  !! {name}.{ext}: block {b} also claimed by {owner[0]}.{owner[1]}")
                    problems += 1
                claimed[b] = (name, ext)
                blocks_used.append(b)
                first_sec = b * secs_per_block
                chunk = b""
                holes = []
                for s in range(secs_per_block):
                    ls = first_sec + s
                    part = img.logical_sector(
                        dir_track + ls // SECTORS_PER_TRACK, ls % SECTORS_PER_TRACK)
                    if not part:
                        part = b"\x00" * SECTOR_SIZE
                        holes.append(s * SECTOR_SIZE)
                    chunk += part
                take = min(need, recs_per_block) * RECORD_SIZE
                # Only count a hole if it lies inside the part of the block
                # this file actually uses.
                for off in holes:
                    if off < take:
                        missing[0] += min(SECTOR_SIZE, take - off)
                blob.extend(chunk[:take])
                need -= min(need, recs_per_block)

        # CP/M records no exact byte length: a text file ends at the first
        # ^Z and the rest of its last block is whatever was there before.
        dropped = 0
        # Trailing holes are not content; drop them before trimming padding.
        while blob and missing[0] and blob[-1] == 0 and len(blob) > 1:
            break
        if not keep_padding:
            if ext.upper() in TEXT_EXTS:
                cut = blob.find(b"\x1a", max(0, len(blob) - bls))
                if cut != -1:
                    dropped = len(blob) - cut
                    blob = blob[:cut]
            while blob and blob[-1] in (0x1A, 0x00):
                blob.pop()
                dropped += 1

        fname = safe_name(f"{name}.{ext}" if ext else name)
        if user:
            fname = f"user{user}_{fname}"
        (outdir / fname).write_bytes(bytes(blob))

        complaint, info = verify(bytes(blob), ext, bls)
        if complaint:
            problems += 1
        if not quiet:
            tail = f"  (+{dropped} B of padding/stale tail dropped)" if dropped > 1 else ""
            if missing[0]:
                total = len(blob) + missing[0]
                pct = 100.0 * len(blob) / total if total else 0.0
                tail = (f"  **{pct:.0f}% of {total} B recovered** "
                        f"({missing[0]} B never captured)")
            print(f"  {fname:<16} {len(blob):>7} bytes  {len(blocks_used):>3} blocks"
                  f"  {complaint or info}{tail}")

    print(f"\n{len(files)} file(s) written to {outdir}")
    unused = [b for b in range(1, total_blocks) if b not in claimed]
    print(f"Blocks: {len(claimed)} allocated, {len(unused)} free")
    if problems:
        print(f"\n{problems} problem(s) reported above.")
    else:
        print("No problems: no overlapping blocks, no extent gaps, all content checks passed.")
    return problems


TEXT_EXTS = ("BAS", "TXT", "ASM", "DOC", "SUB", "PRN", "DAT")


def printable_ratio(blob: bytes) -> float:
    if not blob:
        return 0.0
    ok = sum(1 for b in blob if 0x20 <= b < 0x7F or b in (0x09, 0x0A, 0x0C, 0x0D))
    return ok / len(blob)


def verify(blob: bytes, ext: str, bls: int):
    """Return (complaint_or_None, info_string)."""
    if not blob:
        return "EMPTY", ""
    ratio = printable_ratio(blob)
    if ext.upper() == "BAS" and blob[:1] == b"\xff":
        # Microsoft BASIC saves tokenised by default; $FF is its marker.
        return None, f"tokenised BASIC, {ratio:.0%} printable"
    if ext.upper() in TEXT_EXTS:
        if ratio < 0.99:
            return f"NOT TEXT ({ratio:.1%} printable)", ""
        bad = []
        for k in range(1, -(-len(blob) // bls)):
            o = k * bls
            if printable_ratio(blob[o - 32:o]) < 0.9 or printable_ratio(blob[o:o + 32]) < 0.9:
                bad.append(k)
        if bad:
            return f"BAD BLOCK BOUNDARY at block {bad[0]} of {-(-len(blob)//bls)}", ""
        lines = len(re.findall(rb"(?m)^\s*\d{1,5}[ \t]", blob))
        nb = -(-len(blob) // bls) - 1
        return None, f"text, {lines} numbered lines, {nb} clean boundaries"
    if ext.upper() == "COM":
        return None, f"binary, {ratio:.0%} printable"
    return None, f"{ratio:.0%} printable"


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        print(__doc__.strip().splitlines()[-1])
        raise SystemExit(1)
    img = Image(Path(args[0]).read_bytes(), "--double-step" in sys.argv)
    rc = extract(img, Path(args[1]),
                 "--keep-padding" in sys.argv, "--quiet" in sys.argv)
    raise SystemExit(1 if rc else 0)


if __name__ == "__main__":
    main()
