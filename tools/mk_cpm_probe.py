#!/usr/bin/env python3
"""
Build a tiny CP/M .COM that prints one string, for probing screen behaviour.

The P2500's control codes can only be settled by making the machine execute
them, and no surviving program exercises most of them. This emits the
smallest thing that can: a byte-at-a-time BDOS console-output loop followed
by whatever bytes you asked for. Pair it with tools/cpm_build.py to get a
bootable disk, run it, and read the result out of --dump-vram-attr.

Function 2 one byte at a time rather than function 9's "print string",
because function 9 stops at a '$' - and a graphics set-point command carries
raw coordinates, one of which is $24. A probe that cannot say "x = 36" is
not much of a probe.

    ORG $0100
    ld hl,msg
    ld bc,len
    loop: ld e,(hl)
          push hl / push bc
          ld c,2          ; BDOS 2, console output
          call $0005
          pop bc / pop hl
          inc hl / dec bc
          ld a,b / or c
          jr nz,loop
    ret                   ; back to the CCP
    msg: ...

usage:
    tools/mk_cpm_probe.py OUT.COM 'text with \\e and \\xHH escapes'
                          [--pad-records N]

example (does ESC 0 P really mean reverse video?):
    tools/mk_cpm_probe.py /tmp/ATTR.COM '\\e0PREVERSED\\e0@ plain'
"""

import sys
from pathlib import Path

BDOS = 0x0005
ORG = 0x0100


def unescape(s: str) -> bytes:
    out = bytearray()
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            c = s[i + 1]
            if c == "e":
                out.append(0x1B); i += 2; continue
            if c == "r":
                out.append(0x0D); i += 2; continue
            if c == "n":
                out.append(0x0A); i += 2; continue
            if c == "t":
                out.append(0x09); i += 2; continue
            if c == "\\":
                out.append(0x5C); i += 2; continue
            if c == "x" and i + 3 < len(s):
                out.append(int(s[i + 2:i + 4], 16)); i += 4; continue
        out.append(ord(s[i]))
        i += 1
    return bytes(out)


PREFIX_LEN = 0x17   # msg starts here; see the listing above


def build(message: bytes) -> bytes:
    if not message:
        raise SystemExit("nothing to print")
    if len(message) > 0xFFFF:
        raise SystemExit("message too long")
    msg = ORG + PREFIX_LEN
    n = len(message)
    return bytes([
        0x21, msg & 0xFF, msg >> 8,             # ld hl,msg
        0x01, n & 0xFF, n >> 8,                 # ld bc,len
        0x5E,                                   # loop: ld e,(hl)
        0xE5,                                   # push hl
        0xC5,                                   # push bc
        0x0E, 0x02,                             # ld c,2   (console output)
        0xCD, BDOS & 0xFF, BDOS >> 8,           # call $0005
        0xC1,                                   # pop bc
        0xE1,                                   # pop hl
        0x23,                                   # inc hl
        0x0B,                                   # dec bc
        0x78,                                   # ld a,b
        0xB1,                                   # or c
        0x20, 0xF0,                             # jr nz,loop
        0xC9,                                   # ret
    ]) + message


def main():
    if len(sys.argv) not in (3, 5):
        print(__doc__)
        return 2
    out, text = Path(sys.argv[1]), sys.argv[2]
    com = build(unescape(text))
    if len(sys.argv) == 5 and sys.argv[3] == "--pad-records":
        # Pad with $00 to an exact record count. The CCP decides whether a
        # .COM fits from its record count alone, so this is how the TPA's
        # real size gets measured rather than computed.
        want = int(sys.argv[4]) * 128
        if want < len(com):
            raise SystemExit(f"--pad-records {sys.argv[4]} is smaller than the program")
        com += b"\x00" * (want - len(com))
    out.write_bytes(com)
    print(f"{out}: {len(com)} bytes, {len(com) // 128} records")
    return 0


if __name__ == "__main__":
    sys.exit(main())
