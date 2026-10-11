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
                                   [--double-step] [--holes FILE]

--double-step salvages a dump that captured only every second track (see
tools/imd_tool.py verify). Logical track t then lives at index t/2 and odd
t is a hole, zero-filled; each file reports how much of it survived.

--holes FILE is the general form of the same thing, and the better one. An
image normalised by `imd_tool.py convert` is indexed by ID cylinder, so a
track that was never read is already in its right place - but it is filled
with a constant, and a constant is indistinguishable from content once it is
in the file. The sidecar `imd_tool.py convert --holes` writes names those
regions per sector, so the recovery percentages mean the same thing they do
under --double-step without the gap pattern having to be guessed from a
stride. Prefer it: it also expresses a single unreadable sector, which a
stride cannot.
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


def read_hole_map(path: Path):
    """Parse an imd_tool.py hole map into {(track, sector_id), ...}.

    Returns (holes, declared_tracks). Lines are `track N` for a whole
    0-based raw track or `sector N S` for one 1-based sector of it."""
    holes, declared = set(), None
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] == "image":
            continue
        elif parts[0] == "geometry" and len(parts) >= 4:
            declared = int(parts[1])
            if (int(parts[2]), int(parts[3])) != (SECTORS_PER_TRACK, SECTOR_SIZE):
                raise SystemExit(
                    f"{path}:{lineno}: hole map is for {parts[2]}x{parts[3]}-byte "
                    f"tracks, this tool reads {SECTORS_PER_TRACK}x{SECTOR_SIZE}")
        elif parts[0] == "track" and len(parts) == 2:
            t = int(parts[1])
            holes.update((t, s) for s in range(1, SECTORS_PER_TRACK + 1))
        elif parts[0] == "sector" and len(parts) == 3:
            holes.add((int(parts[1]), int(parts[2])))
        else:
            raise SystemExit(f"{path}:{lineno}: cannot parse {line!r}")
    return holes, declared


class Image:
    def __init__(self, data: bytes, double_step: bool = False, holes=None):
        self.data = data
        self.tracks = len(data) // (SECTOR_SIZE * SECTORS_PER_TRACK)
        # Sectors the dump never captured, as {(raw track, 1-based sector id)}.
        # The image has bytes there - it has to have something - and this is
        # the only record that they are filler rather than data.
        self.holes = holes or set()
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
        """Sector by LOGICAL track; b"" when it was never captured."""
        idx = self.raw_track(track)
        if idx is None:
            return b""
        phys = SKEW[lsec]
        if (idx, phys + 1) in self.holes:
            return b""
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

    # A double-stepped dump holds only HALF the logical tracks, so the disk
    # it came from is twice as long as the file. Sizing the block space off
    # the file instead rejects every block in the top half as "past end of
    # disk" - including the ones that WERE captured, since a block at
    # logical track 2 + b/2 is present whenever that track is even. Files
    # living entirely up there came out as 0 bytes.
    logical_tracks = img.tracks * (2 if img.double_step else 1)
    data_tracks = logical_tracks - dir_track
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
        hole_ranges = []  # (start, end) of those bytes, as blob offsets
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
                # this file actually uses. Record WHERE it is as well as how
                # much: the padding trim below has to tell a zero that was
                # never captured from a zero that was.
                at = len(blob)
                for off in holes:
                    if off < take:
                        n = min(SECTOR_SIZE, take - off)
                        missing[0] += n
                        hole_ranges.append((at + off, at + off + n))
                blob.extend(chunk[:take])
                need -= min(need, recs_per_block)

        # CP/M records no exact byte length: a text file ends at the first
        # ^Z and the rest of its last block is whatever was there before.
        dropped = 0
        # But a trailing hole is not padding. The trim below drops trailing
        # zero bytes, and on a damaged dump a never-captured tail is zero
        # too - trimming that makes the file's extent lie about itself, and
        # deletes the file outright when its captured part was blank as well
        # (which is how a 1408-byte CFTABLES.PHI became 0 bytes). So the trim
        # stops at the last byte this dump never captured: no hole, no floor,
        # and a healthy image trims exactly as before.
        hole_end = max((b for _, b in hole_ranges), default=0)
        if not keep_padding:
            if ext.upper() in TEXT_EXTS:
                # A ^Z is real content, so it can never sit in a hole; cutting
                # at one is right even when a hole follows it.
                cut = blob.find(b"\x1a", max(0, len(blob) - bls))
                if cut != -1:
                    dropped = len(blob) - cut
                    blob = blob[:cut]
            while len(blob) > hole_end and blob[-1] in (0x1A, 0x00):
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
    argv, args = sys.argv[1:], []
    i = 0
    while i < len(argv):
        if argv[i] == "--holes":
            i += 2                      # flag and its value
        elif argv[i].startswith("--"):
            i += 1
        else:
            args.append(argv[i]); i += 1
    if len(args) < 2:
        print(__doc__.strip().splitlines()[-1])
        raise SystemExit(1)
    holes, declared = set(), None
    if "--holes" in sys.argv:
        i = sys.argv.index("--holes")
        if i + 1 >= len(sys.argv):
            raise SystemExit("--holes needs a file")
        holes, declared = read_hole_map(Path(sys.argv[i + 1]))
    if holes and "--double-step" in sys.argv:
        raise SystemExit("--holes and --double-step are two ways to say the same "
                         "thing; an imd_tool-normalised image needs only --holes")
    data = Path(args[0]).read_bytes()
    img = Image(data, "--double-step" in sys.argv, holes)
    if declared is not None and declared != img.tracks:
        raise SystemExit(f"hole map declares {declared} tracks but "
                         f"{args[0]} holds {img.tracks} - wrong pair of files?")
    rc = extract(img, Path(args[1]),
                 "--keep-padding" in sys.argv, "--quiet" in sys.argv)
    raise SystemExit(1 if rc else 0)


if __name__ == "__main__":
    main()
