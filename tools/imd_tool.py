#!/usr/bin/env python3
"""
ImageDisk (.IMD) verifier and normalizer for Philips P2500 disks.

WHY THIS EXISTS
---------------
A `.raw` image is a flat linear dump: track 0's sectors, then track 1's, and
so on. That throws away the one thing some of these images depend on - the
*sector ID cylinder* recorded on the disk itself, which the `.IMD` keeps in a
per-track cylinder map.

On a healthy P2500 disk the relationship is `ID = physical + 1` and the two
layouts are the same thing, so nothing is lost. But four of the nine images
in this project were dumped by an ImageDisk configured for 48 TPI media in a
96 TPI drive: it double-stepped, so it read *every other track* and recorded
ID cylinders 1, 3, 5, ... 79. Flattened to `.raw` they look like ordinary
40-track disks, and the missing half is invisible.

It is not recoverable - the data was never read. What this tool does is make
it *visible*, and emit a `.raw` that positions what was captured correctly:

  verify   - per-image manifest: geometry, ID cylinder sequence, which IDs are
             present/absent, bad or deleted sectors. Says plainly whether an
             image is complete.
  convert  - write a `.raw` indexed by ID cylinder rather than by physical
             track position. For a healthy image this is byte-identical to the
             existing `.raw` (check it with --compare). For a double-stepped
             one, captured data lands at its true offset and the gaps are
             explicit filler instead of silently shifting everything.

The emulator needs no change either way: it already computes
`lba = (C - 1) * sectors_per_track + (R - 1)`, which is exactly an
ID-cylinder-indexed layout.

FORMAT
------
ASCII header terminated by 0x1A, then one variable-length record per track:
  mode, cylinder, head, sector-count, size-code
  sector-number map        (sector-count bytes)
  cylinder map             (sector-count bytes) - only if head bit 7 set
  head map                 (sector-count bytes) - only if head bit 6 set
  one record per sector, each a type byte plus data:
    0 = absent (no data at all)   1 = normal      2 = compressed (1 byte)
    3/4 = deleted-address-mark    5/6 = data error    7/8 = deleted + error
    (even types are the compressed variant: a single byte repeated)
Sector size = 128 << size-code.

usage:
  tools/imd_tool.py verify  <file.IMD> [...]
  tools/imd_tool.py convert <file.IMD> <out.raw> [--fill BYTE]
  tools/imd_tool.py convert <file.IMD> <out.raw> --compare <existing.raw>
"""

import sys
import os

MODES = {0: "500k FM", 1: "300k FM", 2: "250k FM",
         3: "500k MFM", 4: "300k MFM", 5: "250k MFM"}

# sector record type -> (has_data, compressed, deleted, error)
STYPES = {
    0: (False, False, False, False),
    1: (True,  False, False, False),
    2: (True,  True,  False, False),
    3: (True,  False, True,  False),
    4: (True,  True,  True,  False),
    5: (True,  False, False, True),
    6: (True,  True,  False, True),
    7: (True,  False, True,  True),
    8: (True,  True,  True,  True),
}


class Track:
    __slots__ = ("mode", "cyl", "head", "smap", "cmap", "hmap", "size", "sectors")

    def __init__(self, mode, cyl, head, smap, cmap, hmap, size, sectors):
        self.mode, self.cyl, self.head = mode, cyl, head
        self.smap, self.cmap, self.hmap = smap, cmap, hmap
        self.size, self.sectors = size, sectors

    @property
    def id_cyl(self):
        """The cylinder number in the sector ID fields, which is what the FDC
        matches against - not necessarily the physical track position."""
        return self.cmap[0] if self.cmap else self.cyl


def parse(path):
    d = open(path, "rb").read()
    try:
        i = d.index(b"\x1a") + 1
    except ValueError:
        raise SystemExit("%s: no 0x1A header terminator - not an IMD file" % path)
    header = d[:i - 1].decode("latin1", "replace")
    tracks = []
    while i < len(d):
        if i + 5 > len(d):
            raise SystemExit("%s: truncated track header at offset %d" % (path, i))
        mode, cyl, head, nsec, szc = d[i], d[i + 1], d[i + 2], d[i + 3], d[i + 4]
        i += 5
        smap = list(d[i:i + nsec]); i += nsec
        cmap = hmap = None
        if head & 0x80:
            cmap = list(d[i:i + nsec]); i += nsec
        if head & 0x40:
            hmap = list(d[i:i + nsec]); i += nsec
        size = 128 << szc
        sectors = []
        for _ in range(nsec):
            t = d[i]; i += 1
            if t not in STYPES:
                raise SystemExit("%s: unknown sector type %d at offset %d" % (path, t, i))
            has_data, compressed, deleted, error = STYPES[t]
            if not has_data:
                data = None
            elif compressed:
                data = bytes([d[i]]) * size; i += 1
            else:
                data = d[i:i + size]; i += size
            sectors.append((data, deleted, error))
        tracks.append(Track(mode, cyl, head & 0x3F, smap, cmap, hmap, size, sectors))
    return header, tracks


def analyse(tracks):
    """Returns (id_cyls, stride, missing_ids, absent, deleted, errored)."""
    id_cyls = [t.id_cyl for t in tracks]
    deltas = sorted({id_cyls[i + 1] - id_cyls[i] for i in range(len(id_cyls) - 1)})
    stride = deltas[0] if len(deltas) == 1 else None
    span = range(min(id_cyls), max(id_cyls) + 1)
    missing = [c for c in span if c not in set(id_cyls)]
    absent = deleted = errored = 0
    for t in tracks:
        for data, dele, err in t.sectors:
            if data is None: absent += 1
            if dele: deleted += 1
            if err: errored += 1
    return id_cyls, stride, missing, absent, deleted, errored


def verify(path):
    header, tracks = parse(path)
    id_cyls, stride, missing, absent, deleted, errored = analyse(tracks)
    nsec = len(tracks[0].smap)
    size = tracks[0].size
    modes = sorted({t.mode for t in tracks})
    heads = sorted({t.head for t in tracks})

    print("=== %s" % os.path.basename(path))
    first = header.splitlines()[0] if header.splitlines() else ""
    print("    header      : %s" % first.strip())
    label = " / ".join(l.strip() for l in header.splitlines()[1:] if l.strip())
    if label:
        print("    label       : %s" % label[:110])
    print("    geometry    : %d track records, %d sector(s)/track, %d bytes/sector, head(s) %s"
          % (len(tracks), nsec, size, heads))
    print("    data rate   : %s" % ", ".join(MODES.get(m, "mode %d" % m) for m in modes))
    seq = "%d,%d,%d,...,%d" % (id_cyls[0], id_cyls[1], id_cyls[2], id_cyls[-1]) \
        if len(id_cyls) > 3 else str(id_cyls)
    print("    ID cylinders: %s   (stride %s)"
          % (seq, stride if stride is not None else "irregular"))

    problems = []
    if stride is not None and stride > 1:
        problems.append(
            "DOUBLE-STEPPED: ID cylinders advance by %d, so only every %s track was read.\n"
            "                  %d of %d cylinders in the range %d-%d are absent. This data was\n"
            "                  never captured and cannot be recovered - the disk needs re-imaging\n"
            "                  with single-stepping."
            % (stride, {2: "2nd", 3: "3rd"}.get(stride, "%dth" % stride),
               len(missing), len(missing) + len(id_cyls), min(id_cyls), max(id_cyls)))
    elif stride is None:
        problems.append("IRREGULAR ID cylinder sequence - inspect manually. Deltas seen: %s"
                        % sorted({id_cyls[i+1]-id_cyls[i] for i in range(len(id_cyls)-1)}))
    if absent:   problems.append("%d sector(s) marked absent (unreadable at dump time)" % absent)
    if errored:  problems.append("%d sector(s) flagged with a data error" % errored)
    if deleted:  problems.append("%d sector(s) carry a deleted address mark" % deleted)

    if problems:
        print("    VERDICT     : PROBLEMS")
        for p in problems:
            print("      - %s" % p)
    else:
        print("    VERDICT     : complete and clean (ID = physical + %d, contiguous)"
              % (id_cyls[0] - tracks[0].cyl))
    return not problems


def convert(path, out_path, fill=0xE5, compare=None):
    _, tracks = parse(path)
    nsec = len(tracks[0].smap)
    size = tracks[0].size
    if any(len(t.smap) != nsec or t.size != size for t in tracks):
        raise SystemExit("%s: mixed geometry across tracks - not supported" % path)

    max_id = max(t.id_cyl for t in tracks)
    # ID cylinders are 1-based on this platform (ID = physical + 1), so ID N
    # occupies raw track N-1 and the image spans max_id tracks.
    track_bytes = nsec * size
    img = bytearray([fill]) * (max_id * track_bytes)
    written = set()
    for t in tracks:
        base = (t.id_cyl - 1) * track_bytes
        for sec_id, (data, _d, _e) in zip(t.smap, t.sectors):
            if data is None:
                continue
            off = base + (sec_id - 1) * size
            img[off:off + size] = data
            written.add(t.id_cyl)
    open(out_path, "wb").write(img)

    gaps = [c for c in range(1, max_id + 1) if c not in written]
    print("wrote %s: %d bytes (%d tracks x %d sectors x %d bytes)"
          % (out_path, len(img), max_id, nsec, size))
    print("  tracks with data : %d" % len(written))
    if gaps:
        print("  GAPS (filled $%02X): %d track(s) -> ID cylinders %s"
              % (fill, len(gaps), compact(gaps)))
    else:
        print("  no gaps - every ID cylinder in 1..%d has data" % max_id)

    if compare:
        old = open(compare, "rb").read()
        if old == bytes(img):
            print("  --compare %s: BYTE-IDENTICAL" % compare)
        else:
            n = min(len(old), len(img))
            diff = sum(1 for i in range(n) if old[i] != img[i])
            print("  --compare %s: differs (%d bytes, sizes %d vs %d)"
                  % (compare, diff + abs(len(old) - len(img)), len(old), len(img)))
    return len(gaps) == 0


def compact(nums):
    """[1,3,5,7] -> '1,3,5,7'; long runs collapse to first..last."""
    if len(nums) <= 12:
        return ",".join(str(n) for n in nums)
    return "%d..%d (%d of them)" % (nums[0], nums[-1], len(nums))


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    cmd = argv[1]
    if cmd == "verify":
        if len(argv) < 3:
            print("usage: imd_tool.py verify <file.IMD> [...]"); return 2
        ok = True
        for p in argv[2:]:
            ok &= verify(p)
            print()
        print("All images complete and clean." if ok else
              "One or more images have problems - see above.")
        return 0 if ok else 1
    if cmd == "convert":
        if len(argv) < 4:
            print("usage: imd_tool.py convert <file.IMD> <out.raw> [--fill BYTE] [--compare old.raw]")
            return 2
        src, dst = argv[2], argv[3]
        fill, cmp_path = 0xE5, None
        rest = argv[4:]
        while rest:
            if rest[0] == "--fill" and len(rest) > 1:
                fill = int(rest[1], 0); rest = rest[2:]
            elif rest[0] == "--compare" and len(rest) > 1:
                cmp_path = rest[1]; rest = rest[2:]
            else:
                print("unknown argument: %s" % rest[0]); return 2
        return 0 if convert(src, dst, fill, cmp_path) else 1
    print("unknown command: %s" % cmd)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
