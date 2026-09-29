#!/bin/sh
# Builds the emulator: libp2500.a, p2500-emu, p2500-gui if SDL3 is present,
# and the demos/ disk. Thin wrapper around `make` - see Makefile for what
# actually runs.
set -e
cd "$(dirname "$0")"

make
make demos

echo
echo "Built:"
[ -f libp2500.a ]       && echo "  libp2500.a"
[ -f p2500-emu ]        && echo "  p2500-emu"
[ -f p2500-gui ]        && echo "  p2500-gui" || echo "  p2500-gui skipped (SDL3 not found by pkg-config)"
[ -f demos/P2500DEMO.raw ] && echo "  demos/P2500DEMO.raw"
