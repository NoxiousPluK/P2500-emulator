#!/bin/sh
# Assembles self-contained distributions in publish/<target>/.
#
#   publish/linux-x64/   always built.
#   publish/windows-x64/ built too if x86_64-w64-mingw32-gcc is on PATH -
#                        needs the mingw-w64 toolchain (mingw-w64-gcc) and
#                        a mingw-w64 SDL3 build (mingw-w64-sdl3) for the 
#                        GUI.
#
# Both binaries are built native + Windows PE .exe from the same source,
# by make's MINGW=1 toggle (see Makefile). Object files are not
# segregated per toolchain, so each target gets its own full clean and
# rebuild - this assembles a release, it does not optimise for rebuild
# speed.
#
# Neither p2500-emu link needs anything beyond its own OS's C runtime.
# p2500-gui additionally needs SDL3, which most machines won't have
# installed, so that library (and whatever it in turn pulls in - e.g.
# libssp-0.dll from the mingw build) is vendored alongside it. The two
# targets need different handling:
#
#   Windows  the DLLs sit next to p2500-gui.exe, because the directory the
#            .exe was loaded from is the FIRST entry in Windows' DLL search
#            order - ahead of the system directories and PATH.
#   Linux    the vendored libSDL3 goes in lib/, which p2500-gui finds
#            through the RUNPATH of $ORIGIN/lib the Makefile links it with
#            - $ORIGIN being the binary's own directory, so the tree stays
#            relocatable.
#
# Not vendored: libc/libstdc++/libgcc - those are linked statically into
# the Windows build already (see Makefile), and assumed present on any
# Linux host.
set -eu
cd "$(dirname "$0")"

# Windows system/CRT-forwarder DLLs a .exe may declare NEEDED that are
# never ours to bundle - anything else in the dependency list came from
# the mingw sysroot and needs vendoring alongside the binary.
WIN_SYSTEM_DLLS='^(KERNEL32|USER32|GDI32|ADVAPI32|SHELL32|SHLWAPI|OLE32|OLEAUT32|SETUPAPI|VERSION|WINMM|IMM32|COMDLG32|WS2_32|WININET|CRYPT32|DNSAPI|IPHLPAPI|NTDLL|MSVCRT|UXTHEME|DWMAPI)\.dll$|^api-ms-win-'
MINGW_BIN=/usr/x86_64-w64-mingw32/bin

# Single source of version truth is src/core/version.h
VERSION=$(sed -n 's/^#define P2500_VERSION_STR "\(.*\)"/\1/p' src/core/version.h)
if [ -z "$VERSION" ]; then
    echo "publish.sh: could not read P2500_VERSION_STR from src/core/version.h" >&2
    exit 1
fi

report() {
    echo
    echo "Published to $1/:"
    du -sh "$1" | cut -f1 | xargs -I{} echo "  {} total"
    find "$1" -maxdepth 1 -mindepth 1 -printf '  %f\n' | sort
}

publish_linux() {
    make clean >/dev/null
    ./build.sh >/dev/null

    OUT=publish/linux-x64
    rm -rf "$OUT"
    mkdir -p "$OUT/lib"

    cp p2500-emu p2500-gui libp2500.a "$OUT/"
    chmod +x "$OUT/p2500-gui" "$OUT/p2500-emu"

    sdl3=$(ldd p2500-gui | awk '/libSDL3\.so/ {print $3; exit}')
    if [ -z "${sdl3:-}" ] || [ ! -f "$sdl3" ]; then
        echo "publish.sh: could not resolve libSDL3.so via ldd - p2500-gui" >&2
        echo "will not run on a system without SDL3 installed." >&2
    else
        cp -L "$sdl3" "$OUT/lib/libSDL3.so.0"
    fi

    cp README.md "$OUT/"
    cp -r demos disks docs roms tools "$OUT/"
    rm -f "$OUT"/demos/*.COM
    find "$OUT/tools" -name '__pycache__' -exec rm -rf {} +

    tar czf "publish/linux-x64-v$VERSION.tar.gz" -C publish linux-x64

    report "$OUT"
}

publish_windows() {
    if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        echo "publish.sh: x86_64-w64-mingw32-gcc not found - skipping publish/windows-x64/"
        echo "  (needs: mingw-w64-gcc and mingw-w64-sdl3 or similar packages)"
        return 0
    fi

    # demos/ is Z80 machine code, identical regardless of host toolchain -
    # rebuilding it here would need ./p2500-emu.exe under Wine, which
    # tools/z80asm.py's --verify does not know how to run. Reuse the
    # native build's output instead of rebuilding.
    hold=$(mktemp -d)
    cp -r demos "$hold/"

    make clean >/dev/null
    make MINGW=1 >/dev/null

    OUT=publish/windows-x64
    rm -rf "$OUT"
    mkdir -p "$OUT"

    cp p2500-emu.exe p2500-gui.exe libp2500.a "$OUT/"

    # Transitive: a bundled DLL (SDL3.dll) can itself need one from the
    # mingw sysroot (libssp-0.dll) that p2500-gui.exe never mentions
    # directly, so each newly-vendored DLL goes back on the queue too.
    seen=""
    set -- p2500-gui.exe
    while [ "$#" -gt 0 ]; do
        this=$1
        shift
        case " $seen " in *" $this "*) continue ;; esac
        seen="$seen $this"
        path="$this"
        [ -f "$path" ] || path="$MINGW_BIN/$this"
        [ -f "$path" ] || continue
        for dll in $(x86_64-w64-mingw32-objdump -p "$path" | awk '/DLL Name:/ {print $3}'); do
            if printf '%s\n' "$dll" | grep -qiE "$WIN_SYSTEM_DLLS"; then continue; fi
            case " $seen " in *" $dll "*) continue ;; esac
            if [ -f "$MINGW_BIN/$dll" ]; then
                cp "$MINGW_BIN/$dll" "$OUT/"
                set -- "$@" "$dll"
            else
                echo "publish.sh: could not find $dll under $MINGW_BIN - p2500-gui.exe" >&2
                echo "will be missing a dependency on a machine without it." >&2
                seen="$seen $dll"
            fi
        done
    done

    cp README.md "$OUT/"
    cp -r "$hold/demos" disks docs roms tools "$OUT/"
    rm -f "$OUT"/demos/*.COM
    find "$OUT/tools" -name '__pycache__' -exec rm -rf {} +
    rm -rf "$hold"

    if command -v zip >/dev/null 2>&1; then
        (cd publish && zip -rq "windows-x64-v$VERSION.zip" windows-x64)
    else
        echo "publish.sh: zip not found - skipping windows-x64-v$VERSION.zip" >&2
    fi

    report "$OUT"
}

rm -rf publish
publish_linux
publish_windows

# Leave the working tree in its normal native-built state - not clean.sh,
# which would also delete the publish/ output just assembled.
make clean >/dev/null
./build.sh >/dev/null
