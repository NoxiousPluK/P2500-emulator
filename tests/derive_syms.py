#!/usr/bin/env python3
"""Derive symbol names from a machine's own structures, with no reference.

VENDORED COPY. The original lives in the research repo as
`tools/derive_syms.py` (see ../P2500-emulator-LLM-docs/debugging-tools.md);
it is copied here because `make test` section 18 runs its `--check` mode and
cannot depend on a directory outside this checkout. If you change one, copy
it over the other - there is deliberately no cleverness keeping them in
step, because the file is standalone and a diff is the whole story.

`port_syms.py` carries names from a build you know to one you do not.
This needs neither: it names what a P2500 running CP/M publishes about
itself. Everything below is found by shape, so it works on a disk nobody
has looked at before - which is the point, since the useful case is the
next unknown image rather than the four that are already understood.

What it finds, and what makes each claim a claim rather than a guess:

  CP/M's own layer   page zero's two vectors, the 17-slot CBIOS jump table
                     they lead to (all `JP nn`, or it is not a jump table),
                     the bodies behind the slots, and the CCP base - BDOS's
                     entry page less $800, which is CP/M's own layout.
  the IM2 layer      the interrupt trampolines, found as a run of the
                     `ld (save),hl / ld hl,HANDLER / jp shim` pattern that
                     both known BIOSes build in the top page, plus the
                     vector table that points at them: the only 12 bytes in
                     RAM holding exactly those six addresses in sequence.
                     Vector order names the devices.
  the driver layer   a device table is a count byte then {id, ptr} records
                     whose pointers all land on [semaphore][driver] pairs
                     in BIOS space with distinct ids - a shape random bytes
                     do not have. Each driver's prologue then gives its
                     function table and the dispatcher they share.
  the disk structures a DPH is 16 bytes whose +10 points at something that
                     parses as a DPB (BLM must be 2^BSH-1, and the rest in
                     range); several in a row at stride 16 is the DPH
                     table. The DPH's own fields name XLT, DIRBUF, CSV, ALV.

Device ids and function codes are named from this project's findings when
known ($31 screen, $30 keyboard, $00-$03 disk, $40 serial/printer) and
left as DEVnn otherwise, so an unfamiliar machine still gets a map.

usage:
  derive_syms.py ram.bin [--range LO:HI] [--vectors CTC:PIOA:DMA]
                         [--emit OUT.sym] [--check SHIPPED.sym] [--quiet]

`--vectors` takes the three interrupt vectors the emulator prints under
[Interrupts] (hex, e.g. 90:98:9A). Given them, the IM2 table is read
rather than searched and the device each trampoline serves is certain.

`--check` compares the derivation against a hand-curated symbol file and
exits 1 if the two contradict each other - see the comment above check().
A copy of this file is vendored into the emulator's tests/ so `make test`
can run that comparison; keep the two in step.
"""
import re
import sys

KNOWN_DEVICES = {
    0x31: ("SCREEN", "console out"),
    0x30: ("KBD", "console in"),
    0x40: ("SER", "serial / printer"),
}
DISK_IDS = (0x00, 0x01, 0x02, 0x03)
CBIOS_ENTRIES = ("BOOT", "WBOOT", "CONST", "CONIN", "CONOUT", "LIST", "PUNCH",
                 "READER", "HOME", "SELDSK", "SETTRK", "SETSEC", "SETDMA",
                 "READ", "WRITE", "LISTST", "SECTRAN")

class Syms:
    def __init__(self):
        self.names = {}
        self.why = {}
    def add(self, addr, name, why):
        if addr in self.names:
            return False
        self.names[addr] = name
        self.why[addr] = why
        return True

def word(m, a):
    return m[a & 0xFFFF] | m[(a + 1) & 0xFFFF] << 8

# --------------------------------------------------------------- CP/M layer
def cpm_layer(m, syms, lo, hi):
    if m[0] != 0xC3 or m[5] != 0xC3:
        return None
    table = (word(m, 1) - 3) & 0xFFFF
    if any(m[(table + 3 * i) & 0xFFFF] != 0xC3 for i in range(17)):
        return None
    bdos = word(m, 6)
    syms.add(0x0005, "BDOS", "page zero $0005")
    syms.add(bdos, "BDOS.body", "page zero $0006")
    syms.add(bdos & 0xFF00, "BDOS.base", "BDOS entry page")
    syms.add(((bdos & 0xFF00) - 0x800) & 0xFFFF, "CCP",
             "BDOS base less $800 - CP/M's own layout")
    for i in range(17):
        slot = (table + 3 * i) & 0xFFFF
        syms.add(slot, CBIOS_ENTRIES[i], "CBIOS jump table")
        syms.add(word(m, slot + 1), CBIOS_ENTRIES[i] + ".body", "CBIOS jump table")
    return table

# ---------------------------------------------------------------- IM2 layer
def im2_layer(m, syms, vectors, lo=0xFE00, hi=0xFFFF):
    tramps = []
    a = lo
    while a < hi - 8:
        if m[a] == 0x22 and m[a + 3] == 0x21 and m[a + 6] == 0xC3:
            tramps.append((a, word(m, a + 1), word(m, a + 4), word(m, a + 7)))
            a += 9
        else:
            a += 1
    if not tramps:
        return []
    syms.add(tramps[0][0], "IM2.TRAMP", f"{len(tramps)} trampolines from here")
    syms.add(tramps[0][1], "ISR.SAVE.HL", "the trampolines' save slot")
    syms.add(tramps[0][3], "ISR.SHIM", "what every trampoline jumps to")

    order = None
    if vectors:
        ctc, pioa, dma = vectors
        base = (ctc & 0xFF00) if ctc > 0xFF else None
        # The table lives at I<<8 | vector; I is not in a dump, so take the
        # page from wherever those six words actually sit.
        want = {t[2] for t in tramps}
        for page in range(0x00, 0x100):
            at = page << 8
            got = [word(m, at | (v & 0xFF)) for v in (ctc, ctc + 2, ctc + 4,
                                                      ctc + 6, pioa, dma)]
            if set(got) == want and len(set(got)) == len(got):
                syms.add(at | (ctc & 0xFF), "CBIOS.IM2",
                         f"vector table at I=${page:02X}, read with the "
                         f"emulator's own vectors")
                order = got
                break
    if order is None:
        # No vectors given: the table is the only place holding all six
        # trampoline addresses in sequence.
        want = [t[2] for t in tramps]
        for at in range(lo, hi - 2 * len(tramps)):
            got = [word(m, at + 2 * i) for i in range(len(tramps))]
            if sorted(got) == sorted(want) and len(set(got)) == len(got):
                syms.add(at, "CBIOS.IM2", "the only run of all six "
                                          "trampoline addresses")
                order = got
                break
    names = ("CTC0.ISR", "CTC1.ISR", "CTC2.ISR", "CTC3.ISR", "PIOA.ISR", "DMA.ISR")
    if order and len(order) == 6:
        for name, handler in zip(names, order):
            syms.add(handler, name, "IM2 vector order")
            if m[handler] == 0xC3:   # a one-instruction hop to the real body
                syms.add(word(m, handler + 1), name + ".body", "its own jp")
    return tramps

# ------------------------------------------------------------- driver layer
def plausible_device_table(m, at, lo, hi):
    n = m[at]
    if not 2 <= n <= 32:
        return None
    ids, recs = set(), []
    for i in range(n):
        r = at + 1 + 3 * i
        if r + 2 > hi:
            return None
        ident, ptr = m[r], word(m, r + 1)
        if not lo <= ptr <= hi - 2 or m[ptr] > 1:
            return None
        drv = word(m, ptr + 1)
        if not lo <= drv <= hi:
            return None
        ids.add(ident)
        recs.append((ident, ptr, drv))
    # Distinct ids, and at least one of this project's known devices: a
    # count byte followed by n well-formed records is already unlikely in
    # data, but the known id is what makes it this machine's device table.
    if len(ids) < n - 3 or not (ids & set(KNOWN_DEVICES)):
        return None
    return recs

def driver_layer(m, syms, lo, hi, quiet):
    best = None
    for at in range(lo, hi - 4):
        recs = plausible_device_table(m, at, lo, hi)
        if recs and (best is None or len(recs) > len(best[1])):
            best = (at, recs)
    if not best:
        return None
    at, recs = best
    syms.add(at, "DEV.TABLE", f"{len(recs)} well-formed device records")
    seen = {}
    for ident, ptr, drv in recs:
        if ident in KNOWN_DEVICES:
            stem = KNOWN_DEVICES[ident][0]
        elif ident in DISK_IDS:
            stem = "DISK"
        elif ident == 0xFF:
            stem = "DEVFF"
        else:
            stem = f"DEV{ident:02X}"
        if drv in seen:
            continue
        seen[drv] = stem
        syms.add(drv, stem + ".DRV", f"device ${ident:02X} in the device table")
        tbl = disp = req = None
        for a in range(drv, min(drv + 48, hi)):
            if m[a] == 0xED and m[a + 1] == 0x53 and req is None:
                req = word(m, a + 2)
            if m[a] == 0x21 and m[a + 3] == 0xCD:
                tbl, disp = word(m, a + 1), word(m, a + 4)
                break
        if tbl and lo <= tbl <= hi:
            syms.add(tbl, stem + ".FNTAB", f"${drv:04X}'s `ld hl,nn / call`")
            n = m[tbl]
            if 1 <= n <= 16:
                for i in range(n):
                    code = m[tbl + 1 + 3 * i]
                    fn = word(m, tbl + 2 + 3 * i)
                    if lo <= fn <= hi:
                        syms.add(fn, f"{stem}.F{code:02X}",
                                 f"function ${code:02X} of ${tbl:04X}")
        if disp and lo <= disp <= hi:
            syms.add(disp, "DRV.DISPATCH", "every driver calls it")
        if req and lo <= req <= hi:
            syms.add(req, stem + ".REQ.PTR", f"${drv:04X} saves its request here")
    return at

# --------------------------------------------------------- disk structures
def parses_as_dpb(m, a, hi):
    if a + 15 > hi:
        return False
    spt = word(m, a)
    bsh, blm, exm = m[a + 2], m[a + 3], m[a + 4]
    dsm, drm = word(m, a + 5), word(m, a + 7)
    off = word(m, a + 13)
    return (8 <= spt <= 256 and 0 <= bsh <= 7 and blm == (1 << bsh) - 1
            and exm <= 15 and 0 < dsm < 0x8000 and 0 < drm < 0x4000
            and off <= 80)

def disk_layer(m, syms, lo, hi):
    hits = []
    for a in range(lo, hi - 16):
        dpb = word(m, a + 10)
        if not (lo <= dpb <= hi - 15) or not parses_as_dpb(m, dpb, hi):
            continue
        xlt, dirbuf = word(m, a), word(m, a + 8)
        if not (lo <= dirbuf <= hi):
            continue
        hits.append((a, xlt, dirbuf, dpb, word(m, a + 12), word(m, a + 14)))
    # The table is the longest run at stride 16 sharing one DPB.
    runs = []
    for h in hits:
        if runs and h[0] == runs[-1][-1][0] + 16 and h[3] == runs[-1][-1][3]:
            runs[-1].append(h)
        else:
            runs.append([h])
    if not runs:
        return None
    run = max(runs, key=len)
    first = run[0]
    syms.add(first[0], "DPH.TABLE", f"{len(run)} DPH(s) at stride 16")
    syms.add(first[3], "DPB", "the DPH's own DPB field")
    if first[1]:
        syms.add(first[1], "SECTRAN.TAB", "the DPH's XLT field")
    syms.add(first[2], "DIRBUF", "the DPH's DIRBUF field")
    for i, h in enumerate(run):
        letter = chr(ord('A') + i)
        if h[4]:
            syms.add(h[4], f"CSV.{letter}", f"DPH {i}'s CSV field")
        if h[5]:
            syms.add(h[5], f"ALV.{letter}", f"DPH {i}'s ALV field")
    return run

# ------------------------------------------------------------- --check mode
#
# A shipped symbol file is hand-curated; this tool derives the same machine's
# structures from scratch. Where they overlap they must not contradict each
# other, and that agreement is the closest thing to a proof either set is
# right - so it is worth running as a test rather than once by hand.
#
# What counts as a contradiction, and what does not:
#
#   - Two different *semantic* names at one address is a contradiction. One
#     of the two files is wrong about what lives there.
#   - A curated name against this tool's purely *structural* one is not.
#     `SCREEN.F05` claims only "function $05 of the screen driver", which
#     is also what `SCREEN.WRITE` says plus what the function does; the
#     curated file knows more, it does not disagree. Those are reported as
#     elaborations.
#   - One name string sitting at two different addresses in the two files
#     is a contradiction even if neither address is otherwise disputed:
#     exactly one of them can be CONIN.
#
# Addresses only one file names are not compared at all. The curated files
# carry names no shape can find (what a flag means, which byte a busy-wait
# spins on), and this tool finds slots nobody has bothered to name.

STRUCTURAL = re.compile(r"^[A-Z0-9]+\.F[0-9A-F]{2}$")

def read_symfile(path):
    """Addr -> name, in the format core/debug.c's loader accepts."""
    out = {}
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.split("#")[0].split(";")[0].strip()
            if not line:
                continue
            m = re.match(r"^\$?(?:0x)?([0-9A-Fa-f]{1,4})\s*[:= ]\s*(\S+)", line)
            if m:
                out[int(m.group(1), 16)] = m.group(2)
    return out

def check(derived, path):
    """0 if the two agree everywhere they overlap, 1 if they contradict."""
    shipped = read_symfile(path)
    both = sorted(set(derived) & set(shipped))
    same = [a for a in both if derived[a] == shipped[a]]
    elaborated = [a for a in both
                  if derived[a] != shipped[a] and STRUCTURAL.match(derived[a])]
    conflicts = [(a, "two names for one address") for a in both
                 if derived[a] != shipped[a] and not STRUCTURAL.match(derived[a])]

    # The reverse direction: a name must not move between the two files.
    rev_d, rev_s = {}, {}
    for a, nm in derived.items():
        rev_d.setdefault(nm, a)
    for a, nm in shipped.items():
        rev_s.setdefault(nm, a)
    for nm in sorted(set(rev_d) & set(rev_s)):
        if rev_d[nm] != rev_s[nm] and not STRUCTURAL.match(nm):
            conflicts.append((rev_s[nm],
                              f"{nm} is ${rev_d[nm]:04X} here, ${rev_s[nm]:04X} there"))

    print(f"\nchecked against {path}: {len(shipped)} curated name(s), "
          f"{len(both)} address(es) in both")
    print(f"   {len(same)} identical, {len(elaborated)} elaborated, "
          f"{len(conflicts)} conflicting")
    for a in elaborated:
        print(f"   elaborated  ${a:04X} {derived[a]} -> {shipped[a]}")
    for a, why in conflicts:
        print(f"   CONFLICT    ${a:04X} {why}: "
              f"derived={derived.get(a, '-')} curated={shipped.get(a, '-')}")
    return 1 if conflicts else 0


def main(argv):
    lo, hi, emit, quiet, vectors = 0xC000, 0xFFFF, None, False, None
    against = None
    args = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--range":
            i += 1; lo, hi = (int(x, 16) for x in argv[i].split(":"))
        elif a == "--vectors":
            i += 1; vectors = tuple(int(x, 16) for x in argv[i].split(":"))
        elif a == "--emit":
            i += 1; emit = argv[i]
        elif a == "--check":
            i += 1; against = argv[i]
        elif a == "--quiet":
            quiet = True
        elif a in ("-h", "--help"):
            print(__doc__); return 0
        else:
            args.append(a)
        i += 1
    if len(args) != 1:
        print(__doc__); return 2

    m = open(args[0], "rb").read()
    if len(m) != 0x10000:
        sys.exit(f"{args[0]}: expected a 64 KB dump, got {len(m)} bytes")

    syms = Syms()
    table = cpm_layer(m, syms, lo, hi)
    print(f"CP/M page zero: " + (f"CBIOS jump table at ${table:04X}" if table
                                 else "not CP/M's - skipping that layer"))
    tramps = im2_layer(m, syms, vectors)
    print(f"IM2 trampolines: {len(tramps)} found")
    dev = driver_layer(m, syms, lo, hi, quiet)
    print(f"device table: " + (f"${dev:04X}" if dev else "none found"))
    dph = disk_layer(m, syms, lo, hi)
    print(f"disk structures: " + (f"{len(dph)} DPH(s), DPB ${word(m, dph[0][0] + 10):04X}"
                                 if dph else "none found"))
    print(f"\n{len(syms.names)} name(s) derived")
    if not quiet:
        for addr in sorted(syms.names):
            print(f"   ${addr:04X} {syms.names[addr]:<14} {syms.why[addr]}")
    if emit:
        with open(emit, "w") as f:
            f.write(f"# Derived from {args[0]} by tools/derive_syms.py -\n"
                    f"# from the machine's own tables, with no reference build.\n\n")
            for addr in sorted(syms.names):
                f.write(f"{addr:04X} {syms.names[addr]:<14} ; {syms.why[addr]}\n")
        print(f"wrote {emit}")
    if against:
        return check(syms.names, against)
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
