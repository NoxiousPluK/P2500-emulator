#!/bin/sh
# Builds the emulator: libp2500.a, p2500-emu, p2500-gui if SDL3 is present,
# and the demos/ disk. Thin wrapper around `make` - see Makefile for what
# actually runs.
#
# It also cross-builds the Windows binaries whenever the mingw-w64
# toolchain is on PATH, for the reason recorded in
# ../P2500-emulator-LLM-docs/windows-cross-build.md: the cross build used
# to run only inside publish.sh, so every Windows breakage so far was
# found while cutting a release rather than when the code was written.
# Objects are segregated per toolchain (build/native, build/mingw), so
# this costs a build rather than the full clean it used to need.
set -e
cd "$(dirname "$0")"

make
make demos

# The Windows GUI additionally needs a mingw-w64 SDL3; the Makefile probes
# for it with x86_64-w64-mingw32-pkg-config and skips p2500-gui.exe if it
# is absent, exactly as the native build does.
#
# demos/ is deliberately not rebuilt for Windows: it is Z80 machine code,
# identical whichever host toolchain assembled it, and tools/z80asm.py's
# --verify would need p2500-emu.exe under Wine.
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    make MINGW=1
fi

echo
echo "Built:"
[ -f build/native/libp2500.a ] && echo "  libp2500.a"
[ -f p2500-emu ]        && echo "  p2500-emu"
[ -f p2500-gui ]        && echo "  p2500-gui" || echo "  p2500-gui skipped (SDL3 not found by pkg-config)"
[ -f demos/P2500DEMO.raw ] && echo "  demos/P2500DEMO.raw"
if [ -f p2500-emu.exe ]; then
    echo "  p2500-emu.exe"
    [ -f p2500-gui.exe ] && echo "  p2500-gui.exe" \
        || echo "  p2500-gui.exe skipped (no mingw-w64 SDL3 found by pkg-config)"
else
    echo "  Windows cross build skipped (x86_64-w64-mingw32-gcc not on PATH)"
fi
