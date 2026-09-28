#!/usr/bin/env python3
"""
Build a tiny CP/M .COM that prints one string, for probing screen behaviour.

The P2500's control codes can only be settled by making the machine execute
them, and no surviving program exercises most of them. This emits the
smallest thing that can: a nine-byte BDOS "print string" call followed by
whatever bytes you asked for. Pair it with tools/cpm_build.py to get a
bootable disk, run it, and read the result out of --dump-vram-attr.

    ORG $0100
    ld de,msg
    ld c,9          ; BDOS 9, print string
    call $0005
    ret             ; back to the CCP
    msg: ...,'$'

usage:
    tools/mk_cpm_probe.py OUT.COM 'text with \\e and \\xHH escapes'

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


def build(message: bytes) -> bytes:
    # BDOS 9 stops at '$', so the message may not contain one.
    if b"$" in message:
        raise SystemExit("BDOS function 9 terminates on '$' - the message cannot contain one")
    msg_addr = ORG + 9
    return bytes([
        0x11, msg_addr & 0xFF, msg_addr >> 8,   # ld de,msg
        0x0E, 0x09,                             # ld c,9
        0xCD, BDOS & 0xFF, BDOS >> 8,           # call $0005
        0xC9,                                   # ret
    ]) + message + b"$"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    out, text = Path(sys.argv[1]), sys.argv[2]
    com = build(unescape(text))
    out.write_bytes(com)
    print(f"{out}: {len(com)} bytes, message {len(com) - 10} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
