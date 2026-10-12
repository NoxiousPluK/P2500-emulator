#!/usr/bin/env bash
#
# Run p2500-gui with no window on your desktop and no focus steal, and get a
# PNG back. For agent-driven and scripted use.
#
# P2500-gui has a real headless mode built in: 
# SDL_VIDEODRIVER=dummy renders into an offscreen surface, and
# --frames N runs a fixed number of video fields and exits on its own. The
# whole GUI test suite runs this way. So there is no process to supervise, no
# display to allocate, and nothing to stop afterwards - this script is a
# convenience wrapper, not a daemon.
#
# Usage:
#   tools/p2500-headless.sh shot [-o out.png] [-f FIELDS] [p2500-gui args...]
#       The composited window, menu bar and panels included.
#   tools/p2500-headless.sh screen [-o out.png] [-f FIELDS] [args...]
#       The emulated screen only, at its native 640x288.
#   tools/p2500-headless.sh text [-s STEPS] [args...]
#       The screen as text, read straight out of VRAM by p2500-emu - no
#       renderer in the path at all. The closest thing here to M2000's
#       SCREEN command.
#   tools/p2500-headless.sh run [args...]
#       p2500-gui headless with whatever arguments you pass; output on stdout.
#
# Defaults: 400 fields for the GUI (enough for CP/M to reach its prompt from
# a cold boot at --speed unlimited), 20,000,000 steps for `text`, and
# disks/P25K_B.raw if you name no disk.
#
#   tools/p2500-headless.sh shot -o /tmp/a.png --panels devices,memory
#   tools/p2500-headless.sh shot --aspect square --snap on --window 1000x700
#   tools/p2500-headless.sh text --type 'dir\r'
#
# PNG conversion needs ImageMagick (`magick`/`convert`) or ffmpeg; with
# neither, the raw PPM is kept and named.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

GUI=./p2500-gui
EMU=./p2500-emu
FIELDS=400
STEPS=20000000
OUT=""

die() { echo "p2500-headless: $*" >&2; exit 1; }

to_png() { # to_png <in.ppm> <out>
    local in="$1" out="$2"
    case "$out" in
        *.ppm) mv -f "$in" "$out"; echo "$out"; return 0 ;;
    esac
    if command -v magick >/dev/null;  then magick "$in" "$out"
    elif command -v convert >/dev/null; then convert "$in" "$out"
    elif command -v ffmpeg >/dev/null;  then ffmpeg -loglevel error -y -i "$in" "$out"
    else
        mv -f "$in" "${out%.*}.ppm"
        echo "${out%.*}.ppm  (no ImageMagick or ffmpeg; kept as PPM)"
        return 0
    fi
    rm -f "$in"
    echo "$out"
}

# Pull our own options off the front, leave the rest for p2500-gui. Anything
# unrecognised is passed through, so --aspect/--snap/--panels and friends all
# work without this script knowing about them.
parse_opts() {
    REST=()
    while [ $# -gt 0 ]; do
        case "$1" in
            -o|--out)    OUT="$2"; shift 2 ;;
            -f|--fields) FIELDS="$2"; shift 2 ;;
            -s|--steps)  STEPS="$2"; shift 2 ;;
            *)           REST+=("$1"); shift ;;
        esac
    done
}

# A disk, unless the caller named one. Every --disk* spelling counts.
default_disk() {
    local a
    for a in "${REST[@]-}"; do
        case "$a" in --disk|--disk-b|--disk-c|--disk-d|--disks) return 0 ;; esac
    done
    [ -f disks/P25K_B.raw ] && REST+=(--disk disks/P25K_B.raw)
}

cmd_capture() { # cmd_capture <shot-window|screenshot> <default-name>
    local flag="$1" name="$2"; shift 2
    parse_opts "$@"
    [ -x "$GUI" ] || die "no $GUI (build it: make)"
    default_disk
    local ppm; ppm="$(mktemp -t p2500-XXXXXX.ppm)"
    SDL_VIDEODRIVER=dummy "$GUI" --speed unlimited --frames "$FIELDS" \
        "--$flag" "$ppm" "${REST[@]-}" >&2
    to_png "$ppm" "${OUT:-$name}"
}

cmd_text() {
    parse_opts "$@"
    [ -x "$EMU" ] || die "no $EMU (build it: make)"
    default_disk
    # p2500-emu has no text-screen flag: it dumps VRAM, and the 80x24 of
    # character codes at the front of it decode to ASCII. Same decode as
    # run_tests.sh's own screen() helper, deliberately - if the two ever
    # disagree about what is on screen, that is worth noticing.
    local vram; vram="$(mktemp -t p2500-XXXXXX.vram)"
    "$EMU" --max-steps "$STEPS" --dump-vram "$vram" "${REST[@]-}" >&2
    od -An -v -tu1 -w1 "$vram" | awk 'NR<=1920{c=$1;
        printf "%s", (c>=32 && c<127) ? sprintf("%c",c) : " ";
        if (NR%80==0) printf "\n"}'
    rm -f "$vram"
}

cmd_run() {
    parse_opts "$@"
    [ -x "$GUI" ] || die "no $GUI (build it: make)"
    SDL_VIDEODRIVER=dummy "$GUI" --frames "$FIELDS" "${REST[@]-}"
}

case "${1:-}" in
    shot)   shift; cmd_capture shot-window p2500-window.png "$@" ;;
    screen) shift; cmd_capture screenshot  p2500-screen.png "$@" ;;
    text)   shift; cmd_text "$@" ;;
    run)    shift; cmd_run "$@" ;;
    *)      awk 'NR>2 && /^#/ { sub(/^# ?/, ""); print; next } NR>2 { exit }' "${BASH_SOURCE[0]}"; exit 1 ;;
esac
