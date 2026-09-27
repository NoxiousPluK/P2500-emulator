#!/bin/sh
# Disassemble a region of a 64K RAM image produced by `p2500-emu --dump-ram`.
#
# The CP/M-era code this project cares about ($E200-$FFFF: CBIOS, SYSPBI's
# resident part) exists on disk only as sector-interleaved .phi files, so the
# ../Disk Images/disassembly/*.asm listings are all based at org 0 and their
# addresses do not correspond to anything. A RAM dump taken at a breakpoint is
# the ground truth, and z80dasm can be pointed straight at it.
#
# usage: tools/disasm_ram.sh RAM.bin START END [> out.asm]
#        addresses in hex, with or without a 0x prefix; END is inclusive
set -e
[ $# -eq 3 ] || { echo "usage: $0 RAM.bin START END (hex)" >&2; exit 1; }
img=$1
start=$(printf '%d' "0x${2#0x}")
end=$(printf '%d' "0x${3#0x}")
len=$((end - start + 1))
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
dd if="$img" of="$tmp" bs=1 skip="$start" count="$len" status=none
z80dasm -a -l -t -g "$start" "$tmp"
