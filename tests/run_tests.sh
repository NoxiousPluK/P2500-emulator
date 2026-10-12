#!/bin/sh
# Regression suite for the P2500 emulator. Every check is a
# falsifiable claim about observable state, not "it didn't crash":
#
#   1. SESAM  - the banner VRAM must be byte-identical to the Python-era
#               reference dump. Guards the CPU core, the SESAM port and the
#               video write path.
#   1b.Banner - with no disk and no cartridge the IPL must come back out of
#               the drive-ready path, print its banner and halt at $014D -
#               what a real P2500 does.
#   1c.Reset  - the FDC's post-reset attention budget: who refills it, who
#               spends it, and how often. Both BIOS generations, because
#               they take different paths through the same driver.
#   2. IPL    - Phase 1: a real disk boot must reach $4A00, plus the four
#               sector-0 landmarks.
#   3. CP/M   - Phase 2: page zero must hold CP/M's real vectors, CBIOS's
#               $E200 jump table must be JP instructions, and the screen
#               must read "58K CP/M Ver. 2.2" with an A> prompt.
#   4. Console- typing DIR at the prompt must list exactly the six files
#               the disk image actually contains. This exercises the whole
#               chain at once: CTC channel 3 + port $06 in, the interrupt
#               daisy chain, the FDC/DMA read path, and CONOUT.
#   4c.Game   - and the same keystroke path must satisfy a program that
#               polls BDOS function 6 in a loop instead of reading a line,
#               all the way to a game that is actually playing.
#   15.Rewind - going back N instructions and forward again must reproduce
#               the machine byte for byte, and a rewind must emit nothing.
#   16.Symbols- --help must work on both front-ends, the shipped
#               symbols/*.sym addresses must agree with the running
#               machine (each checked at an instruction that uses it), and
#               --symbols auto must identify the build from its own bytes.
#   17.Drives - four drive slots (the hardware's select lines), --disks
#               placing images in order, and the guest BIOS refusing a
#               letter it does not map.
#   14.EOT    - a multi-sector read must stop at the last sector of the
#               track. Unreachable from any guest program this project
#               has, so it runs against the core directly.
#   18.Derived- the hand-curated symbols/*.sym must not contradict an
#               independent, shape-only derivation of the same machine
#               (tests/derive_syms.py --check), and that derivation must
#               find enough to make the comparison mean something.
#   19.Images - a `# match-disk` rule must name an attached image before
#               anything has booted, say nothing about an image nobody
#               shipped a rule for, and never be mistaken for a vote on
#               whether that build's symbols are resident.
#   20.Sysinfo- --sysinfo must report what it can prove in order of
#               certainty and STOP where the proof stops: the full map on
#               a CP/M machine, the Machine section alone on one that is
#               not running CP/M. The GUI panel is the same core call.
#   21.Lamps  - four drive lamps (the hardware's select lines), reading
#               mapped / attached-but-unmapped / empty as three distinct
#               brightnesses.
#   22.Cart   - the cartridge slot's stream and traffic in the report.
#   25.Ini    - the configuration syntax: what the TOML subset accepts, what
#               it refuses rather than guesses at, and the preservation the
#               profiles and media metadata will depend on. Runs against the
#               core directly.
#   26.Prefs  - the preferences file: the four-layer precedence, the
#               poison-file isolation that keeps a developer's settings out
#               of this suite, writing on change, and surviving a hand-edit.
#   27.Geometry- the remembered window size: bounded at both ends (a floor
#               the file, the env and a drag all answer to, and the
#               display's usable bounds as the ceiling), the flags that
#               still beat it, and the settle timer that keeps one drag
#               from being hundreds of writes.
#   28.Build  - the build system's own invariant: objects and the library
#               segregated per toolchain, so a Windows cross build can sit
#               beside a native one, and -Werror=format on the project's
#               own code but not on the vendored ImGui. Read out of make's
#               real recipes, without compiling anything.
#   24.Cartpath- and the whole path end to end: the manufacturer's own
#               P2490 BASIC probe must print back the exact key fed in
#               through --cartridge, say NO SESAM for an empty slot, and
#               leave on/reset/3 bytes/off on the port in that order (T93).
#
# usage: tests/run_tests.sh [SECTION...]   (or: make test, make test T="7 21")
#
# With no argument every section runs. A SECTION is one of the ids above or
# its short name - tests/run_tests.sh --list prints both. An unknown name
# exits 2, and a run that ends up with no checks at all exits 1: a filtered
# run that quietly proves nothing must not read as a pass.
set -u
# This script reads itself - --list takes section titles from the sections'
# own echo lines - so pin its path before the cd moves the ground under a
# relative $0.
SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
cd "$(dirname "$SELF")/.."

# Pin the GUI's display geometry for the whole suite. The front-end now
# defaults to a 4:3 viewport with smooth scaling, which is what the machine
# looked like; square pixels snapped to whole dots is the geometry every
# check below was written against, and the one that leaves a capture's
# glyph edges hard rather than filtered. Set once here rather than threaded
# through 54 p2500-gui invocations - and it is the same override a config
# file would need, so a developer's saved settings cannot reach the suite.
#
# Consequence worth knowing: this means the *default* display path is
# covered only by section 7's own aspect checks, which therefore have to be
# real comparisons rather than smoke tests.
export P2500_DISPLAY=aspect=square,snap=on

# And switch the preferences file off for the whole suite. P2500_DISPLAY
# above pins the settings it names; this pins the ones it does not, which is
# the rest of them - speed, capitals lock, which debugger panels are open.
# A developer's saved settings must not be able to reach a check, and a
# check must not be able to write to a developer's file either. Section 26
# tests the preferences themselves and names its own file every time.
export P2500_CONFIG=none

EMU=./p2500-emu
DISK="disks/P25K_B.raw"
REF="tests/fixtures/vram_after_banner.bin"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fails=0
checks=0

pass() { checks=$((checks + 1)); printf '  PASS  %s\n' "$1"; }
fail() { checks=$((checks + 1)); fails=$((fails + 1)); printf '  FAIL  %s\n' "$1"; }

# The front-end's own `lamps:` and `layout:` report, which several sections
# drive the mouse by rather than pinning a pixel column. Produced on demand
# and cached, so a section selected on its own does not depend on an earlier
# section's capture having been taken. It is --shot-window that makes the
# front-end log its layout, which is why this takes one.
gui_report() {
    [ -s "$TMP/gui_report.log" ] && return 0
    [ -x ./p2500-gui ] && [ -f "$DISK" ] || return 1
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 20 \
        --shot-window "$TMP/gui_report.ppm" >"$TMP/gui_report.log" 2>&1
}

# --- Sections, and selecting them ------------------------------------------
#
# Each section is a sec_<id> function below, and this table is the run order.
# The ID IS THE NAME: ids are quoted in TODO.md, they are never reused, and
# they are deliberately not positions - the suite's own header comment lists
# them in a different order from the one they run in. The second field is a
# short alias for typing. Both work as selectors:
#
#   tests/run_tests.sh            every section (what `make test` does)
#   tests/run_tests.sh 7 21       the SDL3 front-end and the drive lamps
#   tests/run_tests.sh gui lamps  the same two, by name
#   tests/run_tests.sh --list     what the names are
#
# No section reads a file another section wrote: where one used to (the speed
# cell's column, a plain CLI run), the consumer now produces it itself, via
# gui_report() or its own run. So any subset is a valid run.
SECTIONS='1:sesam 1b:banner 1c:reset 2:ipl 3:cpm 4:dir 4a:driveb 4b:swap 4c:game
5:banktrip 6:render 7:gui 8:debug 9:log 10:cpmbuild 11:graphics 12:demos
13:speed 14:eot 15:rewind 16:symbols 17:drives 18:derived 19:images
20:sysinfo 21:lamps 22:cart 23:write 24:cartpath 25:ini 26:prefs
27:geometry 28:build'

usage() {
    cat <<USAGE
usage: tests/run_tests.sh [SECTION...]

With no argument the whole suite runs, which is what \`make test\` does.
A SECTION is a section id (1b, 4c, 20) or its short name (banner, game,
sysinfo); --list names them all. An unknown name is an error rather than
an empty pass.
USAGE
}

# Titles come out of the sections' own echo lines, so this listing cannot
# drift from what actually runs.
list_sections() {
    printf '  %-4s %-9s %s\n' id name title
    for e in $SECTIONS; do
        printf '  %-4s %-9s %s\n' "${e%%:*}" "${e#*:}" \
            "$(sed -n "s/^echo \"== ${e%%:*}\\. //p" "$SELF" | head -1 |
               sed -e 's/"$//' -e 's/\\//g')"
    done
}

known() {
    for e in $SECTIONS; do
        if [ "$1" = "${e%%:*}" ] || [ "$1" = "${e#*:}" ]; then return 0; fi
    done
    return 1
}

SELECTED=
for a in "$@"; do
    case $a in
        --list) list_sections; exit 0 ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "run_tests.sh: unknown option: $a" >&2; usage >&2; exit 2 ;;
    esac
    if known "$a"; then SELECTED="$SELECTED $a"
    else
        echo "run_tests.sh: no such section: $a" >&2
        echo "run_tests.sh: try --list" >&2
        exit 2
    fi
done

want() {
    [ -z "$SELECTED" ] && return 0
    for s in $SELECTED; do
        if [ "$s" = "$1" ] || [ "$s" = "$2" ]; then return 0; fi
    done
    return 1
}

# A section present in the file but missing from the table would never run,
# and a table entry with no section would never match. Both are the same
# failure as a selector that matches nothing, so this is checked on every
# run rather than only on a filtered one.
for e in $SECTIONS; do echo "${e%%:*}"; done | sort >"$TMP/sec-table"
sed -n 's/^sec_\([0-9][0-9a-z]*\)() {$/\1/p' "$SELF" | sort >"$TMP/sec-file"
if ! cmp -s "$TMP/sec-table" "$TMP/sec-file"; then
    echo "run_tests.sh: the section table disagrees with the sections in this file" >&2
    echo "  table: $(tr '\n' ' ' <"$TMP/sec-table")" >&2
    echo "  file:  $(tr '\n' ' ' <"$TMP/sec-file")" >&2
    exit 2
fi

# Section bodies below are NOT indented inside their functions, on purpose:
# they are full of heredocs whose terminators must sit at column 0 and whose
# bodies are Python, where indentation is meaning.

# Render the 80x24 text screen out of a VRAM dump.
screen() { od -An -v -tu1 -w1 "$1" | awk 'NR<=1920{c=$1;
    printf "%s", (c>=32 && c<127) ? sprintf("%c",c) : " ";
    if (NR%80==0) printf "\n"}'; }

sec_1() {
echo "== 1. SESAM bootable-cartridge banner (byte-exact VRAM)"
if [ ! -f "$REF" ]; then
    fail "reference dump missing: $REF"
else
    $EMU --rom roms/ipl.bin --cartridge tests/fixtures/sesam_banner_test.bin \
         --max-steps 2000000 --dump-vram "$TMP/sesam.bin" >"$TMP/sesam.log" 2>&1
    if cmp -s "$TMP/sesam.bin" "$REF"; then pass "VRAM identical to $REF"
    else fail "VRAM differs from $REF"; fi
fi
}

sec_1b() {
echo "== 1b. No disk, no cartridge: the IPL banner and the halt at \$014D"
# What a real P2500 does when it finds nothing to boot from, confirmed
# against real hardware. The FDD card is present in this configuration,
# the FDC presence probe at $0498 passes, and the IPL still has to come
# back out of the drive-ready path and print the banner.
$EMU --rom roms/ipl.bin --max-steps 2000000 --break 0x014D \
     --dump-vram "$TMP/nodisk.bin" --dump-screen "$TMP/nodisk.ppm" \
     >"$TMP/nodisk.log" 2>&1
if grep -q 'Final: PC=\$014D' "$TMP/nodisk.log"; then
    pass "diskless boot reaches the halt at \$014D"
else fail "diskless boot never reached \$014D - the IPL is stuck again"; fi

# Byte-exact, because the banner is a table of (address, string) records in
# the ROM: if the row pitch is not 80 these offsets are not row starts.
python3 - "$TMP/nodisk.bin" <<'PY' && pass "banner is at rows 6/9/12, and nothing else is on screen" || fail "banner VRAM is wrong"
import sys
v = open(sys.argv[1], 'rb').read()
ok = True
def check(cond, why):
    global ok
    if not cond:
        print("    banner check failed: " + why); ok = False
for off, want in ((0, b'\x9f'),                      # row 0 col 0
                  (512, b'P H I L I P S'),           # row 6 col 32
                  (752, b'MICROCOMPUTER'),           # row 9 col 32
                  (995, b'P2000/B')):                # row 12 col 35
    got = v[off:off + len(want)]
    check(got == want, "at $%04X wanted %r, got %r" % (0x8000 + off, want, got))
# 1 + 13 + 13 + 7 = 34 cells written; the rest of the visible page is clear.
check(sum(1 for c in v[:1920] if c) == 34,
      "%d non-zero cells on the visible page, expected 34" % sum(1 for c in v[:1920] if c))
sys.exit(0 if ok else 1)
PY

# The $9F at row 0 col 0 is how the IPL extinguishes the hardware cursor:
# the CRTC's non-blinking full-cell cursor sits on that cell and inverts
# whatever is under it, so a blank reads as a solid block and the solid
# block reads as blank. Both halves are asserted - checking only the final
# frame would pass just as well if the cursor were not drawn at all.
$EMU --rom roms/ipl.bin --max-steps 2000000 --break 0x013C \
     --dump-screen "$TMP/precursor.ppm" >>"$TMP/nodisk.log" 2>&1
python3 - "$TMP/precursor.ppm" "$TMP/nodisk.ppm" <<'PY' && pass "the \$9F block cancels the cursor at row 0 col 0" || fail "cursor/\$9F inversion is wrong"
import sys
def cell00(path):
    d = open(path, 'rb').read()
    hdr = d.split(b'\n', 3)
    w, h = (int(v) for v in hdr[1].split())
    px = d[len(b'\n'.join(hdr[:3])) + 1:]
    # Any lit dot in the top-left character cell (8 x 12).
    return any(px[((y * w + x) * 3) + 1] > 0x80
               for y in range(12) for x in range(8))
ok = True
if not cell00(sys.argv[1]):
    print("    before the banner: cell (0,0) is dark - the cursor is not drawn"); ok = False
if cell00(sys.argv[2]):
    print("    after the banner: cell (0,0) is lit - the $9F did not cancel it"); ok = False
sys.exit(0 if ok else 1)
PY
}

sec_1c() {
echo "== 1c. The post-reset attention budget, and who spends it"
# The FDC model hands out a fixed number of unsolicited post-reset
# attention interrupts per release of the uPD765's RESET line, standing in
# for the drive-polling sweep a real controller runs after every reset
# (fdc.h). Section 1b already brackets the budget's SIZE - at 1 the
# diskless IPL hangs at $06C9, at 3 it derails to $0AC7, and only 2 reaches
# the banner. What is asserted here is the SHAPE: who refills it, who
# spends it, and how often. It drifted once with nothing watching it.
#
# The spender is the disk driver in PBIOS, not CBIOS - neither bootable
# generation's CBIOS contains a port access - and the two generations take
# different paths through it, so both are checked.
budget() { grep -c 'post-reset unsolicited interrupt' "$1"; }
refills() { grep -c 'RESET released' "$1"; }

# The IPL alone, with no disk and no driver: two refills back-to-back at
# $0AE5, both slots spent, then a third refill when it re-pulses RESET on
# its way out of the drive-ready path, and one more spent.
$EMU --rom roms/ipl.bin --max-steps 2000000 --break 0x014D --verbose-io \
     >"$TMP/rst-nodisk.log" 2>&1
if [ "$(refills "$TMP/rst-nodisk.log")" = 3 ] && [ "$(budget "$TMP/rst-nodisk.log")" = 3 ]; then
    pass "diskless: the IPL refills the budget 3x and spends 3 attentions"
else
    fail "diskless: $(refills "$TMP/rst-nodisk.log") refills / $(budget "$TMP/rst-nodisk.log") attentions, expected 3/3"
fi

# P25K_G, the older PHILIPS P2000B generation: its driver at $FB20 has no
# PB7 escape, so it reaches the decision purely on its own post-reset flag
# ($FD24, cleared after each RESET release and set at the tail of the
# asynchronous completion). It therefore arms one operation and returns
# exactly once per controller reset - $FB2D, the "port B = $42" on that
# path - and drives the handler inline for the other 7 calls.
if [ -f disks/P25K_G.raw ]; then
    $EMU --disk disks/P25K_G.raw --max-steps 60000000 --verbose-io \
         --count 0xFB20 --count 0xFB2D >"$TMP/rst-g.log" 2>&1
    if grep -q -- '--count \$FB20: 8 execution(s)' "$TMP/rst-g.log" &&
       grep -q -- '--count \$FB2D: 1 execution(s)' "$TMP/rst-g.log"; then
        pass "P25K_G: the PBIOS driver runs 8x and arms the interrupt exactly once"
    else
        fail "P25K_G: the arm-and-return path no longer runs once per boot"
    fi
    # One more refill than the IPL's own (its $FA1A reset) and one more
    # attention spent (the one that gets that armed operation going).
    if [ "$(refills "$TMP/rst-g.log")" = 3 ] && [ "$(budget "$TMP/rst-g.log")" = 3 ]; then
        pass "P25K_G: 3 refills, 3 attentions - the driver spends the IPL's leftover"
    else
        fail "P25K_G: $(refills "$TMP/rst-g.log") refills / $(budget "$TMP/rst-g.log") attentions, expected 3/3"
    fi
else
    fail "disk image missing: disks/P25K_G.raw"
fi

# P25K_B, the newer generation: its driver at $FAB7 asks PB7 first
# ("IN A,($11) / AND $80" at $FABA, read as a card-capability strap and
# modelled as 0 until someone measures it on real hardware), so it never
# reaches the arm-and-return path at $FAC8 at all. All 8 calls run the
# handler inline, and the budget its own resets refill is never touched -
# which is exactly why this generation booted while P25K_G hung, and hid
# the missing refill for so long.
$EMU --disk "$DISK" --max-steps 60000000 --verbose-io \
     --count 0xFAB7 --count 0xFAC8 >"$TMP/rst-b.log" 2>&1
if grep -q -- '--count \$FAB7: 8 execution(s)' "$TMP/rst-b.log" &&
   grep -q -- '--count \$FAC8: never executed' "$TMP/rst-b.log"; then
    pass "P25K_B: all 8 driver calls take the inline path (PB7 reads 0)"
else
    fail "P25K_B: the PB7 escape no longer sends every call inline"
fi
if [ "$(refills "$TMP/rst-b.log")" = 4 ] && [ "$(budget "$TMP/rst-b.log")" = 2 ]; then
    pass "P25K_B: 4 refills, 2 attentions - its driver refills and spends none"
else
    fail "P25K_B: $(refills "$TMP/rst-b.log") refills / $(budget "$TMP/rst-b.log") attentions, expected 4/2"
fi
}

sec_2() {
echo "== 2. IPL disk boot reaches Phase 1's landmarks"
if [ ! -f "$DISK" ]; then
    fail "disk image missing: $DISK"
else
    $EMU --disk "$DISK" --max-steps 2000000 >"$TMP/ipl.log" 2>&1
    for lm in 1000 1002 1003 1006 4A00; do
        if grep -q "landmark \$$lm .* hit #1" "$TMP/ipl.log"; then pass "landmark \$$lm hit"
        else fail "landmark \$$lm never hit"; fi
    done
fi
}

sec_3() {
echo "== 3. CP/M 2.2 is up"
if [ ! -f "$DISK" ]; then
    fail "disk image missing: $DISK"
else
    $EMU --disk "$DISK" --max-steps 3000000 \
         --peek 0000:8 --peek E200:16 --dump-vram "$TMP/cpm.bin" >"$TMP/cpm.log" 2>&1
    # page zero: JP $E203 (CBIOS warm boot) at $0000, JP $D406 (BDOS) at $0005
    if grep -q '\$0000: C3 03 E2 00 00 C3 06 D4' "$TMP/cpm.log"; then
        pass "page zero = JP \$E203 / JP \$D406"
    else fail "page zero is not CP/M's"; fi
    # CBIOS jump table: the first 5 entries must all be JP (C3)
    if grep -qE '\$E200: C3 .. .. C3 .. .. C3 .. .. C3 .. .. C3' "$TMP/cpm.log"; then
        pass "\$E200 CBIOS jump table is JP instructions"
    else fail "\$E200 is not a jump table"; fi
    screen "$TMP/cpm.bin" >"$TMP/cpm.screen"
    if grep -q '58K CP/M Ver. 2.2' "$TMP/cpm.screen"; then pass "banner: 58K CP/M Ver. 2.2"
    else fail "CP/M banner not on screen"; fi
    if grep -q '^A>' "$TMP/cpm.screen"; then pass "A> prompt present"
    else fail "no A> prompt"; fi
fi
}

sec_4() {
echo "== 4. Typing DIR at the prompt lists the disk's real contents"
if [ ! -f "$DISK" ]; then
    fail "disk image missing: $DISK"
else
    $EMU --disk "$DISK" --max-steps 20000000 --type 'dir\r' \
         --dump-vram "$TMP/dir.bin" >"$TMP/dir.log" 2>&1
    screen "$TMP/dir.bin" >"$TMP/dir.screen"
    if grep -q 'A>DIR' "$TMP/dir.screen"; then pass "DIR echoed at the prompt"
    else fail "DIR was not echoed - keyboard path broken"; fi
    for f in PIP SYSGEN SYSCBI SYSLOAD SYSPBI SYSCPM; do
        if grep -q "$f" "$TMP/dir.screen"; then pass "DIR lists $f"
        else fail "DIR does not list $f"; fi
    done
fi
}

sec_4a() {
echo "== 4a. Drive B:"
GAMES="disks/P2500GAM.raw"
if [ ! -f "$GAMES" ]; then
    echo "  SKIP  $GAMES not present"
else
    $EMU --disk "$DISK" --disk-b "$GAMES" --max-steps 60000000 \
         --push-at '4000:dir b:\r' --dump-vram "$TMP/b.bin" >"$TMP/b.log" 2>&1
    screen "$TMP/b.bin" >"$TMP/b.screen"
    # The unit-select bits must actually route the read: B: has to list the
    # games disk, and A: must still hold the CP/M system disk.
    if grep -q 'B: BLACKJAK BAS' "$TMP/b.screen"; then
        pass "DIR B: lists the disk in drive B"
    else fail "DIR B: did not read drive B"; fi
    if grep -q 'PIP' "$TMP/b.screen"; then
        fail "drive B's listing leaked drive A's files"
    else pass "drive B's listing is not drive A's"; fi
fi
}

sec_4b() {
echo "== 4b. Swapping a disk mid-run"
# What a swap does and does not cost, both asserted.
#
# Reads refresh by themselves: CP/M re-reads the directory on every search,
# so a DIR straight after a swap lists the new disk with no warm boot. What
# the swap does cost is WRITE access - these drives carry CKS=16 in their
# DPB, i.e. removable media, so BDOS compares the directory checksum, finds
# it changed, and sets the drive's bit in its read-only vector at $E1AD.
# Ctrl-C clears it through the BDOS reset at $E086. That flag has no visible
# consequence until writing to disk is implemented, which is exactly why the
# claim needs a test rather than a sentence in a tooltip.
SYSDISK="disks/P25K_S.raw"
if [ ! -f "$SYSDISK" ]; then
    echo "  SKIP  $SYSDISK not present"
else
    $EMU --disk "$DISK" --max-steps 6000000 --swap-at "6000:$SYSDISK" \
         --type-at '4000:dir\r' --type-at '8000:dir\r' --type-at '11000:\x03' \
         --watch E1AD:2 --dump-vram "$TMP/swap.bin" \
         >"$TMP/swap.log" 2>"$TMP/swap.err"
    screen "$TMP/swap.bin" >"$TMP/swap.screen"
    # PIP is only on the first disk, SC2 only on the second, and no warm
    # boot happened between the two listings.
    if grep -q 'PIP' "$TMP/swap.screen" && grep -q 'SC2      COM' "$TMP/swap.screen"; then
        pass "DIR after a swap lists the new disk with no warm boot"
    else fail "the swapped-in disk was not listed"; fi
    if grep -q 'watch \$E1AD\] \$00 -> \$01' "$TMP/swap.err"; then
        pass "BDOS marks the swapped drive read-only (\$E1AD bit 0)"
    else fail "BDOS did not mark the swapped drive read-only"; fi
    if grep -q 'watch \$E1AD\] \$01 -> \$00.*written by \$E086' "$TMP/swap.err"; then
        pass "Ctrl-C clears the read-only flag"
    else fail "Ctrl-C did not clear the read-only flag"; fi
fi
}

sec_4c() {
echo "== 4c. A scripted session drives an interactive game (P25K_G, PAC.COM)"
# The widest thing --type-at has to answer for: a program that polls BDOS
# function 6 with E=$FF - direct console I/O, non-blocking - in a tight
# loop, rather than reading a line through the CCP. PAC does that from
# $0A25, so every poll either gets the byte or gets a zero, and a queue
# that delivered bytes only on a read the program never makes would show
# up here and nowhere else in this suite.
#
# Three claims, because "the screen changed" is not one: the setup screen
# is reached, a scripted TAB lands in the program's own field index, and
# two carriage returns - PAC has TWO setup screens, which is the whole
# reason this once read as a harness bug - leave a game actually playing.
GAMEDISK="disks/P25K_G.raw"
if [ ! -f "$GAMEDISK" ]; then
    echo "  SKIP  $GAMEDISK not present"
else
    $EMU --disk "$GAMEDISK" --max-steps 20000000 \
         --type-at '4000:PAC\r' --type-at '12000:\t' --watch 45C5:1 \
         --dump-vram "$TMP/pac.bin" >"$TMP/pac.log" 2>"$TMP/pac.err"
    screen "$TMP/pac.bin" >"$TMP/pac.screen"
    if grep -q '<TAB> next field' "$TMP/pac.screen"; then
        pass "PAC.COM loads and draws its setup screen"
    else fail "PAC.COM never drew its setup screen"; fi
    # The field index itself, not a redraw: $0833 is PAC's own store.
    if grep -q 'watch \$45C5\] \$00 -> \$01, written by \$0833' "$TMP/pac.err"; then
        pass "a scripted TAB advances PAC's field index"
    else fail "the scripted TAB never reached PAC's BDOS-6 poll"; fi

    $EMU --disk "$GAMEDISK" --max-steps 35000000 \
         --type-at '4000:PAC\r' --type-at '12000:\r' --type-at '14000:\r' \
         --dump-vram "$TMP/pacrun.bin" >"$TMP/pacrun.log" 2>&1
    screen "$TMP/pacrun.bin" >"$TMP/pacrun.screen"
    # The title line survives both setup screens and is gone once the maze
    # owns the display, so its absence is the state change, and a non-zero
    # score is the game having actually run rather than merely redrawn.
    if grep -q 'PACMAN' "$TMP/pacrun.screen"; then
        fail "still on PAC's setup screen after two carriage returns"
    else pass "two carriage returns leave PAC's setup screens"; fi
    if grep -qE 'PLAYER 1: *[1-9]' "$TMP/pacrun.screen"; then
        pass "the game is playing (PLAYER 1 has scored)"
    else fail "the maze is up but nothing is moving"; fi
fi
}

sec_5() {
echo "== 5. The undecoded-video-bank tripwire still fires"
# tests/fixtures/sesam_bank_probe.bin is a SESAM cartridge whose payload is
# LD A,$01 / OUT ($05),A / HALT - i.e. it selects one of port $05's six
# undecoded $8000-$BFFF windows, the most likely home of the video card's
# attribute plane. Nothing in any disk image does this, so without a
# deliberate probe the diagnostic would be untested code that quietly rots.
$EMU --cartridge tests/fixtures/sesam_bank_probe.bin --max-steps 3000000 \
     >"$TMP/bank.log" 2>&1
if grep -q 'selects an unknown \$8000-\$BFFF window' "$TMP/bank.log"; then
    pass "unknown bank select is logged"
else fail "unknown bank select was NOT logged - the tripwire is dead"; fi
if grep -q 'selected an undecoded \$8000-\$BFFF window' "$TMP/bank.log"; then
    pass "unknown bank select is counted in the exit report"
else fail "unknown bank select missing from the exit report"; fi
}

sec_6() {
echo "== 6. The core renderer and the live keyboard ring"
if [ ! -f "$DISK" ]; then
    fail "disk image missing: $DISK"
else
    # --push-at drives p2500_keyboard_push, which is the exact call a GUI
    # keypress makes, so this covers the front-end input path with no display.
    $EMU --disk "$DISK" --max-steps 30000000 --push-at '4000:dir\r' \
         --dump-screen "$TMP/screen.ppm" >"$TMP/screen.log" 2>&1
    if [ -s "$TMP/screen.ppm" ] && head -c 15 "$TMP/screen.ppm" | grep -q '640 288'; then
        pass "rendered 640x288 from the CRTC registers"
    else fail "no 640x288 render produced"; fi
    python3 - "$TMP/screen.ppm" <<'PY' && pass "render checks passed" || fail "render checks failed"
import sys
d = open(sys.argv[1], 'rb').read()
hdr = d.split(b'\n', 3)
w, h = (int(v) for v in hdr[1].split())
px = d[len(b'\n'.join(hdr[:3])) + 1:]
def lit(x, y):
    o = (y * w + x) * 3
    return px[o + 1] > 0x80           # green channel: foreground is bright
ok = True
def check(cond, why):
    global ok
    if not cond:
        print("    render check failed: " + why)
        ok = False
# Typing DIR through the live ring must have produced text on screen.
check(sum(1 for y in range(h) for x in range(w) if lit(x, y)) > 1000,
      "screen is essentially blank - the live keyboard ring delivered nothing")
# T27a: the 'p' of "Philips" is row 0 col 5, and its descender lives on
# cell rows 8-9. An 8-row glyph read would leave these blank.
check(any(lit(5 * 8 + x, y) for x in range(8) for y in (8, 9)),
      "no descender under the 'p' of Philips - glyph truncated to 8 rows")
# The cursor is a solid block; CP/M parks it at row 6 col 2 after DIR.
check(all(lit(2 * 8 + x, 6 * 12 + y) for x in range(8) for y in range(12)),
      "no solid cursor block at row 6 col 2 (R14/R15 = $01E2)")
sys.exit(0 if ok else 1)
PY

    # The attribute latch (port $0A, T27) must stay silent on a normal boot:
    # CP/M prints its banner and DIR with no attributes, so any cell carrying
    # one means the latch is painting when it should not be.
    if grep -q 'carrying an attribute' "$TMP/screen.log"; then
        fail "attributes appeared during a plain CP/M boot"
    else pass "attribute latch silent on a plain boot"; fi
fi
}

sec_7() {
echo "== 7. The SDL3 front-end boots headless"
if [ ! -x ./p2500-gui ]; then
    echo "  (skipped: run 'make gui' to build it)"
else
    # A stale p2500-gui once passed this whole section while the library
    # under it had moved on by hours, so check the binary is actually newer
    # than what it links against. `make test` now depends on the GUI too, but
    # this script can be run on its own.
    # build/native/, not the top level: objects and the library are
    # segregated per toolchain, so the native library is what a
    # native p2500-gui was linked against.
    if [ ! -f build/native/libp2500.a ]; then
        fail "build/native/libp2500.a is missing - cannot tell if p2500-gui is stale"
    elif [ ./p2500-gui -nt build/native/libp2500.a ]; then
        pass "p2500-gui is newer than libp2500.a"
    else fail "p2500-gui is STALE - rebuild it (make gui)"; fi
    if SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 400 \
           --screenshot "$TMP/gui.ppm" >"$TMP/gui.log" 2>&1 &&
       [ -s "$TMP/gui.ppm" ]; then
        pass "p2500-gui ran 400 fields and rendered"
    else fail "p2500-gui failed headless"; fi
    # Same pixel assertions the CLI render gets, so the front-end cannot
    # drift away from the core renderer unnoticed.
    python3 - "$TMP/gui.ppm" <<'PY' && pass "GUI pixel checks passed" || fail "GUI pixel checks failed"
import sys
d = open(sys.argv[1], 'rb').read()
hdr = d.split(b'\n', 3)
w, h = (int(v) for v in hdr[1].split())
px = d[len(b'\n'.join(hdr[:3])) + 1:]
def lit(x, y):
    o = (y * w + x) * 3
    return px[o + 1] > 0x80
ok = True
def check(cond, why):
    global ok
    if not cond:
        print("    GUI check failed: " + why); ok = False
check((w, h) == (640, 288), "wrong geometry %dx%d" % (w, h))
check(sum(1 for y in range(h) for x in range(w) if lit(x, y)) > 400,
      "screen essentially blank - the front-end rendered nothing")
check(any(lit(5 * 8 + x, y) for x in range(8) for y in (8, 9)),
      "no descender under the 'p' of Philips")
sys.exit(0 if ok else 1)
PY

    # The ImGui menu bar renders even under the dummy video driver, so it can
    # be checked with no display: capture the whole composited window and
    # confirm there is UI in the top band and that the emulated screen has
    # been pushed below it.
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 400 \
        --shot-window "$TMP/win.ppm" >>"$TMP/gui.log" 2>&1
    python3 - "$TMP/win.ppm" "$TMP/gui.ppm" <<'PY' && pass "menu bar renders" || fail "menu bar did not draw"
import sys
d = open(sys.argv[1], 'rb').read()
hdr = d.split(b'\n', 3)
w, h = (int(v) for v in hdr[1].split())
px = d[len(b'\n'.join(hdr[:3])) + 1:]
def lit(x, y):
    o = (y * w + x) * 3
    return px[o + 1] > 0x60
ok = True
def check(cond, why):
    global ok
    if not cond:
        print("    menu check failed: " + why); ok = False
bar = sum(1 for y in range(0, 18) for x in range(w) if lit(x, y))
check(bar > 20, "no lit pixels in the menu bar band - the UI did not draw")
# The capitals-lock indicator is pinned to the far right of the bar and is
# always drawn, so there must be ink in the last 24 px of the band.
check(any(lit(x, y) for x in range(w - 24, w) for y in range(2, 18)),
      "no capitals-lock indicator at the right of the menu bar")
# The emulated screen must start below the bar. Probe a strip that falls in a
# gap between two menu labels but which the "Philips P2500" banner would
# occupy if the screen were drawn at y=0.
#
# The probe's own discriminating power is asserted rather than assumed: the
# same region is looked up in the screen-only capture (half the scale, since
# the composite is drawn at 2x), and if the banner has no ink there then the
# probe could never have failed and is reported as broken. That is the guard
# a previous version of this check lacked - adding a third menu moved the
# labels under the old strip and it went on passing. It has now earned its
# keep twice: adding the View menu moved Machine under the old 118..144 strip
# and this failed loudly rather than quietly. The strip now sits in the gap
# between Machine and Debug, which is the widest gap with banner ink under it.
PROBE_X, PROBE_Y = range(172, 195), range(2, 22)
check(not any(lit(x, y) for x in PROBE_X for y in PROBE_Y),
      "banner ink inside the menu band - screen was not offset")
sd = open(sys.argv[2], 'rb').read()
shdr = sd.split(b'\n', 3)
sw, sh = (int(v) for v in shdr[1].split())
spx = sd[len(b'\n'.join(shdr[:3])) + 1:]
check(any(spx[((y // 2) * sw + x // 2) * 3 + 1] > 0x60 for x in PROBE_X for y in PROBE_Y),
      "the probe strip is blank on the emulated screen too - this check "
      "cannot fail, move it over the banner")
sys.exit(0 if ok else 1)
PY
    # The offset itself comes from the front-end's own layout report. In the
    # default window the capture cannot second-guess it: the viewport fills
    # the window exactly, so there is no letterbox anywhere to measure.
    if grep -qE 'layout: menu [0-9]+ px, screen at y=[1-9]' "$TMP/gui.log"; then
        pass "screen is offset below the menu bar"
    else fail "screen not offset below the menu bar"; fi

    # ...which is what --window is for: a window the 640x288 screen does not
    # divide evenly letterboxes on all four sides, and because the letterbox
    # is a *different* black from the screen's own background (T66.3), the
    # viewport rectangle can be measured straight out of the capture and held
    # against the line the front-end logged. Before that colour split the two
    # blacks were identical and this check was impossible to write.
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 60 \
        --window 1000x700 --shot-window "$TMP/boxed.ppm" \
        >"$TMP/boxed.log" 2>&1
    python3 - "$TMP/boxed.ppm" "$TMP/boxed.log" <<'VIEWPORT' && pass "viewport rectangle in the capture matches the logged layout" || fail "viewport rectangle does not match the logged layout"
import re, sys
d = open(sys.argv[1], 'rb').read()
hdr = d.split(b'\n', 3)
w, h = (int(v) for v in hdr[1].split())
px = d[len(b'\n'.join(hdr[:3])) + 1:]
log = open(sys.argv[2]).read()
m = re.search(r'layout: menu (\d+) px, screen at y=(\d+) x=(\d+), (\d+)x(\d+)', log)
if not m:
    print("    no layout line in the GUI log"); sys.exit(1)
menu, ly, lx, lw, lh = (int(g) for g in m.groups())

# Both blacks come from the front-end's own `display:` line rather than
# being written here: they are settings now, and a literal would quietly go
# stale the first time a theme or phosphor changed one of them.
d = re.search(r'display: .*letterbox (\d+),(\d+),(\d+), screen-bg (\d+),(\d+),(\d+)', log)
if not d:
    print("    no display line in the GUI log"); sys.exit(1)
LETTERBOX = tuple(int(g) for g in d.groups()[:3])
SCREEN_BG = tuple(int(g) for g in d.groups()[3:])
def at(x, y):
    o = (y * w + x) * 3
    return (px[o], px[o + 1], px[o + 2])

ok = True
def check(cond, why):
    global ok
    if not cond:
        print("    viewport check failed: " + why); ok = False

# Discriminating power first: both blacks have to be present and distinct, or
# the measurement below is measuring nothing. This is the check that fails
# the day someone sets the clear colour back to PALETTE.bg.
have = {at(x, y) for y in range(menu + 1, h, 7) for x in range(0, w, 7)}
check(LETTERBOX != SCREEN_BG, "the letterbox and the screen share a colour")
check(LETTERBOX in have, "no letterbox pixels below the menu bar - nothing to measure")
check(SCREEN_BG in have, "no screen-background pixels - the screen did not draw")

# The viewport is then everything below the menu band that is not letterbox:
# scan one row through its middle and one column through it.
xs = [x for x in range(w) if at(x, ly + lh // 2) != LETTERBOX]
ys = [y for y in range(menu + 1, h) if at(lx + lw // 2, y) != LETTERBOX]
if not xs or not ys:
    print("    found no viewport at all below the menu bar"); sys.exit(1)
# One pixel of slack: the layout centres on a half pixel and the log rounds.
check(abs(xs[0] - lx) <= 1 and abs(xs[-1] - (lx + lw - 1)) <= 1,
      "horizontal extent %d..%d, logged x=%d w=%d" % (xs[0], xs[-1], lx, lw))
check(abs(ys[0] - ly) <= 1 and abs(ys[-1] - (ly + lh - 1)) <= 1,
      "vertical extent %d..%d, logged y=%d h=%d" % (ys[0], ys[-1], ly, lh))
check(ys[0] > menu, "viewport starts inside the menu band")
sys.exit(0 if ok else 1)
VIEWPORT

    # --- the default display geometry ----------------------------------
    # Everything above runs under P2500_DISPLAY=aspect=square,snap=on, which
    # is the pre-2026-10 geometry. That pins the suite, but it also means the
    # *shipped default* - 4:3, smooth - is covered by nothing unless it is
    # checked here on purpose. Both runs use the same awkward window, so the
    # only difference between them is the two settings.
    for mode in "43:--aspect 4:3 --snap off" "sq:--aspect square --snap on"; do
        tag=${mode%%:*}
        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 300 \
            --speed unlimited --window 1000x700 ${mode#*:} \
            --screenshot "$TMP/asp-$tag.native.ppm" \
            --shot-window "$TMP/asp-$tag.ppm" >"$TMP/asp-$tag.log" 2>&1
    done
    # And that 4:3 really is what ships. Both runs above name their mode
    # explicitly, so neither would notice the default being changed; this one
    # passes no display flags at all and drops the suite's own P2500_DISPLAY,
    # which is the only run in the suite that sees what a user sees.
    env -u P2500_DISPLAY SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" \
        --frames 10 --shot-window "$TMP/asp-def.ppm" >"$TMP/asp-def.log" 2>&1
    if grep -q 'display: aspect 4:3, snap off,' "$TMP/asp-def.log"; then
        pass "with no flags and no P2500_DISPLAY the screen is 4:3, unsnapped"
    else fail "the shipped default is not 4:3 with snapping off"; fi

    python3 - "$TMP/asp-43.ppm" "$TMP/asp-43.native.ppm" "$TMP/asp-43.log" \
              "$TMP/asp-sq.ppm" "$TMP/asp-sq.native.ppm" "$TMP/asp-sq.log" \
        <<'ASPECT' && pass "the 4:3 default fills the window and filters; square+snap neither" || fail "the default display geometry is not 4:3, or is not filtered"
import re, sys

def ppm(path):
    d = open(path, 'rb').read()
    hdr = d.split(b'\n', 3)
    w, h = (int(v) for v in hdr[1].split())
    return w, h, d[len(b'\n'.join(hdr[:3])) + 1:]

def layout(path):
    log = open(path).read()
    m = re.search(r'layout: menu (\d+) px, screen at y=(\d+) x=(\d+), (\d+)x(\d+)', log)
    # Matched field-by-field rather than as one fixed sequence: the display
    # line gains fields as settings are added, and a positional regex breaks
    # every time one does (it already did once, when `snapped` arrived).
    f = re.search(r'display: aspect (\S+?), .*\bsnap (\w+), .*\bfilter (\S+)', log)
    if not m or not f:
        print("    missing layout/display line in " + path); sys.exit(1)
    return [int(g) for g in m.groups()], f.groups()

ok = True
def check(cond, why):
    global ok
    if not cond:
        print("    " + why); ok = False

(w43, h43, p43), (ws, hs, ps) = ppm(sys.argv[1]), ppm(sys.argv[4])
(m43, l43, x43, cw43, ch43), f43 = layout(sys.argv[3])
(ms, ls, xs_, cws, chs), fs = layout(sys.argv[6])

# 1. The shapes. 4:3 means the rect really is 4:3 and really does use all the
#    height available below the menu bar; square+snap is the native 2.22:1 at
#    a whole multiple, which at this window size is 1x.
check(abs(cw43 / ch43 - 4 / 3) < 0.02, "default rect %dx%d is not 4:3" % (cw43, ch43))
check(l43 + ch43 >= h43 - 1, "default rect leaves %d px unused below it" % (h43 - l43 - ch43))
check((cws, chs) == (640, 288), "square+snap gave %dx%d, not the native 640x288" % (cws, chs))
check((cw43, ch43) != (cws, chs), "both modes drew the same rectangle")

# 2. The settings actually reached the renderer.
check(f43[0] == "4:3" and f43[1] == "off", "default reported aspect/snap %s/%s" % f43[:2])
check(fs[0] == "square" and fs[1] == "on", "square run reported aspect/snap %s/%s" % fs[:2])
check(f43[2] != "nearest", "the 4:3 default reported filter '%s'" % f43[2])
check(fs[2] == "nearest", "square+snap reported filter '%s', expected nearest" % fs[2])

# 3. The filter does something, and the unfiltered path does not - checked
#    against the machine's own output rather than against colours written
#    here. A nearest blit can only ever emit colours that are already in the
#    640x288 render; a resampling one must introduce intermediate shades. So
#    --screenshot (the framebuffer) is the ground truth for both.
def screen_colours(win, native, rect):
    w, h, px = win
    nw, nh, npx = native
    palette = {npx[i:i+3] for i in range(0, nw * nh * 3, 3)}
    # Densely, and over the top of the rect where CP/M's banner and prompt
    # are: a sparse grid over a screen that is mostly background samples no
    # glyph edges at all, and glyph edges are the entire point.
    x0, y0, cw, ch = rect
    seen = set()
    for y in range(y0 + 1, y0 + int(ch * 0.45)):
        row = y * w
        for x in range(x0 + 1, x0 + cw - 1):
            o = (row + x) * 3
            seen.add(px[o:o+3])
    return palette, seen

pal43, saw43 = screen_colours((w43, h43, p43), ppm(sys.argv[2]), (x43, l43, cw43, ch43))
pals, saws = screen_colours((ws, hs, ps), ppm(sys.argv[5]), (xs_, ls, cws, chs))
# Discriminating power: with a blank screen there are no glyph edges to
# filter and the comparison below would pass vacuously either way.
check(len(pal43) > 1, "the emulated screen rendered a single colour - nothing to filter")
check(sum(1 for c in saw43 if c not in pal43) >= 0 and len(saws) > 1,
      "the square+snap capture holds one colour - the screen did not draw")
check(saws <= pals,
      "square+snap introduced %d colour(s) the framebuffer never had" % len(saws - pals))
check(not (saw43 <= pal43),
      "the 4:3 default introduced no intermediate shades - it did not filter")
sys.exit(0 if ok else 1)
ASPECT

    # --- snapping must not abandon the aspect ---------------------------
    # Flooring each axis to a whole number of dots independently does not
    # give 4:3, and the failure is not subtle: a 1600x900 window floored to
    # 1x3, a rect of aspect 0.74 - a portrait picture in 4:3 mode. The sizes
    # below are the ones that used to break it, so this is a regression test
    # with a known-failing input rather than a hopeful sweep.
    : > "$TMP/snap.log"
    for win in 1280x984 1000x700 1600x900 800x600 1920x1464 900x700; do
        SDL_VIDEODRIVER=dummy ./p2500-gui --frames 3 --aspect 4:3 --snap on \
            --window "$win" --shot-window "$TMP/snap.ppm" 2>&1 \
            | grep -E '^(layout|display):' | sed "s/^/$win /" >>"$TMP/snap.log"
    done
    python3 - "$TMP/snap.log" <<'SNAP' && pass "whole-pixel snapping never abandons the 4:3 it is snapping within" || fail "snapping produced a rect that is not 4:3"
import re, sys
lines = open(sys.argv[1]).read().splitlines()
geo = {}
for l in lines:
    m = re.match(r'(\S+) layout: menu \d+ px, screen at y=\d+ x=\d+, (\d+)x(\d+)', l)
    if m: geo.setdefault(m.group(1), {})['rect'] = (int(m.group(2)), int(m.group(3)))
    m = re.match(r'(\S+) display: .*snapped (\S+?),', l)
    if m: geo.setdefault(m.group(1), {})['snapped'] = m.group(2)
ok = True
if len(geo) < 6:
    print("    only %d of 6 window sizes reported" % len(geo)); ok = False
for win, g in sorted(geo.items()):
    w, h = g['rect']
    err = abs((w / h) / (4 / 3) - 1)
    # 1/9 is the 2x3 pair, the best whole-dot fit most windows can offer.
    if err > 0.12:
        print("    %s: snapped to %dx%d, aspect %.2f (%.0f%% off 4:3), snapped=%s"
              % (win, w, h, w / h, err * 100, g['snapped'])); ok = False
    # And when no pair fits, the exact rect must be kept - not a floored one.
    if g['snapped'] == 'nofit' and err > 0.01:
        print("    %s: reported no snap fit but the rect is %.0f%% off 4:3"
              % (win, err * 100)); ok = False
# Discriminating power: at least one size must actually snap, or this only
# proves that falling back to the exact rect works.
if not any(g['snapped'] not in ('nofit', 'off') for g in geo.values()):
    print("    no window size snapped at all - the check proves nothing"); ok = False
sys.exit(0 if ok else 1)
SNAP

    # --- phosphor and interface theme ----------------------------------
    # Two settings that change colour, and the thing worth checking about
    # each is that what the front-end *reports* is what it actually drew:
    # the report line is what the viewport check above now trusts, so a
    # report that drifted from the pixels would quietly disarm it.
    for v in "green dark" "white dark" "green paper"; do
        set -- $v
        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 300 \
            --speed unlimited --window 1000x700 --phosphor "$1" --theme "$2" \
            --screenshot "$TMP/col-$1-$2.native.ppm" \
            --shot-window "$TMP/col-$1-$2.ppm" >"$TMP/col-$1-$2.log" 2>&1
    done
    python3 - "$TMP" <<'COLOURS' && pass "phosphor and theme change the pixels the front-end says they do" || fail "a reported phosphor or theme does not match what was drawn"
import re, sys
T = sys.argv[1]

def ppm(path):
    d = open(path, 'rb').read()
    hdr = d.split(b'\n', 3)
    w, h = (int(v) for v in hdr[1].split())
    return w, h, d[len(b'\n'.join(hdr[:3])) + 1:]

def report(path):
    m = re.search(r'display: .*phosphor (\w+), theme (\w+), '
                  r'letterbox (\d+),(\d+),(\d+), screen-bg (\d+),(\d+),(\d+)',
                  open(path).read())
    if not m:
        print("    no display line in " + path); sys.exit(1)
    g = m.groups()
    return g[0], g[1], bytes(int(x) for x in g[2:5]), bytes(int(x) for x in g[5:8])

def colours(path):
    w, h, px = ppm(path)
    return {px[i:i+3] for i in range(0, w * h * 3, 3)}

ok = True
def check(cond, why):
    global ok
    if not cond:
        print("    " + why); ok = False

runs = {}
for name in ("green-dark", "white-dark", "green-paper"):
    ph, th, lb, bg = report("%s/col-%s.log" % (T, name))
    runs[name] = (ph, th, lb, bg, colours("%s/col-%s.ppm" % (T, name)),
                  colours("%s/col-%s.native.ppm" % (T, name)))

# 1. Each run reports the settings it was given.
for name in runs:
    ph, th = name.split("-")
    check(runs[name][0] == ph and runs[name][1] == th,
          "%s reported phosphor/theme %s/%s" % (name, runs[name][0], runs[name][1]))

# 2. Green is the machine's own and must not move: these are the three values
#    every shipped screenshot and every other check in this file is in.
check(runs["green-dark"][3] == bytes((8, 12, 8)),
      "the green phosphor's background is now %s, not 8,12,8" % list(runs["green-dark"][3]))

# 3. The reported background is really on the emulated screen, for both
#    phosphors, and the two are not the same colour.
for name in ("green-dark", "white-dark"):
    check(runs[name][3] in runs[name][5],
          "%s reports screen-bg %s but the render has no such pixel"
          % (name, list(runs[name][3])))
check(runs["green-dark"][3] != runs["white-dark"][3],
      "both phosphors render the same background")
check(runs["green-dark"][5] != runs["white-dark"][5],
      "both phosphors produced the same set of screen colours")

# 4. The letterbox is the dead space around a picture tube, not chrome, so it
#    stays dark whatever the phosphor and whatever the theme - and is really
#    in each capture.
check(runs["green-dark"][2] == runs["white-dark"][2] == runs["green-paper"][2],
      "the letterbox moved with the phosphor or the theme; it should do neither")
check(max(runs["green-dark"][2]) < 64,
      "the letterbox is not dark: %s" % list(runs["green-dark"][2]))
for name in ("green-dark", "green-paper"):
    check(runs[name][2] in runs[name][4],
          "%s reports letterbox %s but the window has no such pixel"
          % (name, list(runs[name][2])))
    # The invariant the viewport check depends on, asserted per theme rather
    # than assumed once: a letterbox the colour of the screen is unmeasurable.
    check(runs[name][2] != runs[name][3],
          "%s: the letterbox and the screen background are the same colour" % name)

# 5. With the letterbox no longer following the theme, the theme needs its own
#    evidence: the menu bar is chrome, so Dark and Paper must paint it
#    differently, and Paper must be the light one.
def menu_band(path):
    w, h, px = ppm(path)
    return [px[(y * w + x) * 3:(y * w + x) * 3 + 3]
            for y in range(2, 18) for x in range(0, w, 3)]
dark_bar, paper_bar = menu_band("%s/col-green-dark.ppm" % T), menu_band("%s/col-green-paper.ppm" % T)
check(set(dark_bar) != set(paper_bar), "both themes painted the same menu bar")
def mean(b): return sum(sum(p) for p in b) / (3.0 * len(b))
check(mean(paper_bar) > mean(dark_bar) + 64,
      "the Paper menu bar (%.0f) is not lighter than the Dark one (%.0f)"
      % (mean(paper_bar), mean(dark_bar)))
sys.exit(0 if ok else 1)
COLOURS

    # --- the debugger panels -------------------------------------------
    # Drawn under the dummy driver like the menu bar, so what the panels put
    # on screen can be asserted with no display.
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 200 \
        --panels devices,memory,disasm,log --shot-window "$TMP/panels.ppm" \
        >"$TMP/panels.log" 2>&1
    python3 - "$TMP/win.ppm" "$TMP/panels.ppm" <<'PY' && pass "debugger panels draw" || fail "debugger panels did not draw"
import sys
def load(p):
    d = open(p, 'rb').read()
    hdr = d.split(b'\n', 3)
    w, h = (int(v) for v in hdr[1].split())
    return w, h, d[len(b'\n'.join(hdr[:3])) + 1:]
bw, bh, bare = load(sys.argv[1])
pw, ph, panels = load(sys.argv[2])
if (bw, bh) != (pw, ph):
    print("    captures differ in size"); sys.exit(1)
# Below the menu bar the bare window holds the emulated screen alone. Opening
# four panels has to put substantially more ink there; were --panels ignored,
# the two images would be identical.
def ink(px):
    return sum(1 for i in range(bw * 3 * 30, len(px), 3) if px[i + 1] > 0x40)
a, b = ink(bare), ink(panels)
if b <= a * 1.2:
    print("    panels added no ink: %d -> %d lit pixels" % (a, b)); sys.exit(1)
sys.exit(0)
PY

    # A breakpoint must actually stop the machine, and the run/pause lamp in
    # the bar must show it. Two runs identical but for --break.
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 400 --break E46F \
        --shot-window "$TMP/paused.ppm" >"$TMP/guibreak.log" 2>&1
    if grep -q 'breakpoint: stopped at \$E46F' "$TMP/guibreak.log"; then
        pass "--break stops the GUI at the requested PC"
    else fail "--break did not stop the GUI"; fi
    python3 - "$TMP/win.ppm" "$TMP/paused.ppm" <<'PY' && pass "run/pause lamp follows the run state" || fail "run/pause lamp did not change"
import sys
def load(p):
    d = open(p, 'rb').read()
    hdr = d.split(b'\n', 3)
    w, h = (int(v) for v in hdr[1].split())
    return w, h, d[len(b'\n'.join(hdr[:3])) + 1:]
w, h, run = load(sys.argv[1])
_, _, paused = load(sys.argv[2])
# The lamp sits between the two hairlines left of the capitals-lock keycap.
def ink(px):
    return sum(1 for y in range(3, 21) for x in range(w - 48, w - 26)
               if px[((y * w) + x) * 3 + 1] > 0x60)
r, p = ink(run), ink(paused)
# Paused is the bright state, and two bars carry more ink than a dim
# triangle, so this is a real inequality rather than a coin flip.
if p <= r:
    print("    lamp unchanged: running %d lit px, paused %d" % (r, p)); sys.exit(1)
sys.exit(0)
PY

    # The drive lamps: three mounted disks must light more of the bar than one
    # does. Nothing else differs between the two runs.
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --disk-b "$DISK" --disk-c "$DISK" \
        --frames 200 --shot-window "$TMP/threedisks.ppm" >>"$TMP/gui.log" 2>&1
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" \
        --frames 200 --shot-window "$TMP/onedisk.ppm" >>"$TMP/gui.log" 2>&1
    python3 - "$TMP/onedisk.ppm" "$TMP/threedisks.ppm" <<'PY' && pass "drive lamps follow mounted media" || fail "drive lamps did not follow mounted media"
import sys
def load(p):
    d = open(p, 'rb').read()
    hdr = d.split(b'\n', 3)
    w, h = (int(v) for v in hdr[1].split())
    return w, h, d[len(b'\n'.join(hdr[:3])) + 1:]
w, h, one = load(sys.argv[1])
_, _, three = load(sys.argv[2])
def ink(px):
    return sum(1 for y in range(3, 21) for x in range(w - 100, w - 55)
               if px[((y * w) + x) * 3 + 1] > 0x80)
a, b = ink(one), ink(three)
if b <= a:
    print("    lamps unchanged: one disk %d lit px, three %d" % (a, b)); sys.exit(1)
sys.exit(0)
PY

    # --- the lamps are controls (clickable, inverted on hover) -------------
    # The guest must run at the machine's own speed, not the host's. SDL calls
    # SDL_AppIterate as fast as it can, and the core manages about 950 fields
    # a second unthrottled - nineteen times too fast, which is what made the
    # demos unwatchable. --frames runs deliberately unpaced so tests do not
    # wait; --paced puts the throttle back, which is the only way to check it.
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 60 --paced \
        >"$TMP/paced.log" 2>&1
    python3 - "$TMP/paced.log" <<'PY' && pass "the guest is paced to 50 fields a second" || fail "pacing is wrong"
import re, sys
t = open(sys.argv[1]).read()
m = re.search(r'presented (\d+) frames in ([0-9.]+) s \(([0-9.]+) frames/s', t)
if not m:
    print("    no pacing report in the log"); sys.exit(1)
rate = float(m.group(3))
# Generous, because a loaded host can only ever be slower: what this has to
# catch is the unthrottled case, which was twenty times faster.
if not 40.0 <= rate <= 56.0:
    print("    %s fields/s, wanted about 50" % m.group(3)); sys.exit(1)
sys.exit(0)
PY
    # The lamp columns come out of the front-end's own report rather than
    # being hardcoded: a test that pins these pixel columns stops testing
    # anything the moment a menu is added or the font changes.
    LAMPROW=$(sed -n 's/.*lamps: caps \([0-9]*\) run \([0-9]*\) drives \([0-9]*\) [0-9]* [0-9]* [0-9]* rows \([0-9]*\)-\([0-9]*\).*/\1 \2 \3 \4 \5/p' "$TMP/gui.log" | head -1)
    if [ -z "$LAMPROW" ]; then
        # Not a skip: this line is how the suite finds the lamps at all, so
        # a layout change that stops it parsing must fail rather than
        # quietly retire the three checks below.
        fail "could not parse the lamp layout line - the bar's report changed shape"
    fi
    if [ -n "$LAMPROW" ]; then
        # shellcheck disable=SC2086
        set -- $LAMPROW
        L_CAPS=$1; L_RUN=$2; L_DRA=$3; L_Y0=$4; L_Y1=$5
        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 30 \
            --mouse "$L_CAPS,11" --shot-window "$TMP/hover.ppm" >"$TMP/hover.log" 2>&1
        python3 - "$TMP/win.ppm" "$TMP/hover.ppm" "$L_CAPS" "$L_Y0" "$L_Y1" <<'PY' && pass "hovering a lamp inverts it" || fail "hovering a lamp did nothing"
import sys
def load(p):
    d = open(p, 'rb').read()
    hdr = d.split(b'\n', 3)
    w, h = (int(v) for v in hdr[1].split())
    return w, h, d[len(b'\n'.join(hdr[:3])) + 1:]
w, h, plain = load(sys.argv[1])
_, _, hover = load(sys.argv[2])
cx, y0, y1 = (int(v) for v in sys.argv[3:6])
# "Inverted" means the ground fills and the glyph goes dark, so state the
# check that way rather than as a ratio: most of the cell must be lit when
# hovered and a minority of it when not. A hover that merely tinted the
# glyph would leave both fractions where they started.
cells = [(x, y) for y in range(y0, y1 + 1) for x in range(cx - 8, cx + 9)]
def frac(px):
    return sum(1 for x, y in cells if px[((y * w) + x) * 3 + 1] > 0x80) / float(len(cells))
a, b = frac(plain), frac(hover)
if not (a < 0.55 and b > 0.65):
    print("    not inverted: %.2f of the cell lit plain, %.2f hovered" % (a, b))
    sys.exit(1)
sys.exit(0)
PY
        # Hovering must not activate anything - a lamp that fires on hover
        # would pause the machine just by the pointer resting on it.
        if grep -q 'run state:' "$TMP/hover.log"; then
            fail "hovering a lamp activated it"
        else pass "hovering a lamp does not activate it"; fi

        # Left click on the run lamp, right click to eject drive A, left
        # click on the capitals-lock keycap.
        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 30 \
            --mouse "$L_RUN,11,left" >"$TMP/click-run.log" 2>&1
        if grep -q 'run state: paused' "$TMP/click-run.log"; then
            pass "clicking the run lamp pauses the machine"
        else fail "clicking the run lamp did nothing"; fi

        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 30 \
            --mouse "$L_DRA,11,right" >"$TMP/click-eject.log" 2>&1
        if grep -q 'drive A: ejected' "$TMP/click-eject.log"; then
            pass "right-clicking a drive lamp ejects the disk"
        else fail "right-clicking a drive lamp did not eject"; fi

        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 30 \
            --mouse "$L_CAPS,11,left" >"$TMP/click-caps.log" 2>&1
        if grep -q 'capitals lock: off' "$TMP/click-caps.log"; then
            pass "clicking the capitals-lock lamp toggles it"
        else fail "clicking the capitals-lock lamp did nothing"; fi
    else
        fail "the front-end did not report its lamp geometry"
    fi

    # --- the guest keeps its keyboard after the UI has used a text field ---
    # ImGui's SDL3 backend calls SDL_StopTextInput() when one of its fields
    # loses focus, which turns SDL_EVENT_TEXT_INPUT off for the whole window -
    # that is where every printable key the guest receives comes from.
    # Asserted on SDL's own text-input state, not on WantCaptureKeyboard,
    # which reads true throughout regardless.
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 8 --panels memory \
        --shot-window "$TMP/mem.ppm" >"$TMP/mem.log" 2>&1
    FIELD=$(sed -n 's/.*goto field \([0-9]*\),\([0-9]*\).*/\1,\2/p' "$TMP/mem.log" | head -1)
    if [ -z "$FIELD" ]; then
        fail "the memory panel did not report its field position"
    else
        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 60 --panels memory \
            --mouse "10:$FIELD,left" --mouse '30:640,450,left' \
            >"$TMP/focus.log" 2>&1
        # The field must actually have taken focus, or the test proves nothing.
        if grep -q 'guest text input: off' "$TMP/focus.log"; then
            pass "clicking a panel field takes the keyboard"
        else fail "the scripted click never reached the field"; fi
        if [ "$(grep -c 'guest text input: on' "$TMP/focus.log")" -ge 1 ]; then
            pass "the guest gets its keyboard back when the field is done"
        else fail "text input never came back - the P2500 is untypeable"; fi
        # An address control commits on its label, which is a button rather
        # than a caption - clicking the words next to a box is what people
        # try first. Driven end to end: focus the box, type into it, click
        # the label. The control run types the same thing and never clicks,
        # so this cannot pass on the typing alone.
        GEOM=$(sed -n 's/.*watch field \([0-9]*\),\([0-9]*\) button \([0-9]*\),\([0-9]*\).*/\1,\2 \3,\4/p' "$TMP/mem.log" | head -1)
        WFIELD=$(echo "$GEOM" | cut -d' ' -f1)
        WBUTTON=$(echo "$GEOM" | cut -d' ' -f2)
        if [ -z "$WBUTTON" ]; then
            fail "the memory panel did not report its add-watch control"
        else
            SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 60 --panels memory \
                --mouse "10:$WFIELD,left" --ui-type '20:E200' --mouse "30:$WBUTTON,left" \
                >"$TMP/btn.log" 2>&1
            SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 60 --panels memory \
                --mouse "10:$WFIELD,left" --ui-type '20:E200' \
                >"$TMP/btn-control.log" 2>&1
            if grep -q 'watches: 1' "$TMP/btn.log"; then
                pass "clicking an address control's label commits it"
            else fail "the label button did not commit the address"; fi
            if grep -q 'watches: 1' "$TMP/btn-control.log"; then
                fail "a watch was added without clicking the label"
            else pass "typing alone adds nothing - the click is what commits"; fi
        fi
    fi
fi
}

sec_8() {
echo "== 8. The shared debug core"
# --disasm and --state are the headless half of the GUI's panels: they print
# the same lines core/debug.c hands ImGui, so a panel's claims can be checked
# without a display - and so the two cannot silently drift apart.
./p2500-emu --max-steps 0 --disasm 0:3 >"$TMP/disasm.log" 2>/dev/null
if grep -q 'C3 00 01     jp \$0100' "$TMP/disasm.log" &&
   grep -q 'C3 DA 02     jp \$02DA' "$TMP/disasm.log"; then
    pass "--disasm decodes the ROM's reset vectors"
else fail "--disasm output wrong"; fi

./p2500-emu --disk "$DISK" --max-steps 900000 --state >"$TMP/state.log" 2>/dev/null
missing=""
for topic in CPU Interrupts CTC PIO DMA FDC CRTC Video Misc; do
    grep -q "^\[$topic\]" "$TMP/state.log" || missing="$missing $topic"
done
if [ -z "$missing" ]; then pass "--state reports every device topic"
else fail "--state missing topic(s):$missing"; fi
# A boot that got as far as CP/M must show the daisy chain having done work.
if grep -qE '^  0\. DMA +vec \$[0-9A-F]{2} +[1-9][0-9]* req' "$TMP/state.log"; then
    pass "--state shows live interrupt counts"
else fail "--state interrupt counts look dead"; fi

# The disassembler is cross-checked against z80dasm over the ROM, every
# opcode page and random byte streams. Both walk the same bytes, so a length
# disagreement desynchronises them and shows up loudly.
if command -v z80dasm >/dev/null 2>&1; then
    if python3 tests/disasm_crosscheck.py --rounds 2 >"$TMP/crosscheck.log" 2>&1; then
        pass "disassembler agrees with z80dasm ($(tail -1 "$TMP/crosscheck.log"))"
    else
        fail "disassembler disagrees with z80dasm"
        head -12 "$TMP/crosscheck.log" | sed 's/^/    /'
    fi
else
    echo "   (z80dasm not installed - disassembler cross-check skipped)"
fi

# A watch can stop the run, not just report it. $E1AD is written during the
# boot copy at a fixed step, so this is deterministic.
$EMU --disk "$DISK" --max-steps 6000000 --watch-break E1AD \
    >"$TMP/wp.out" 2>"$TMP/wp.err"
if grep -q -- '--watch-break: a watched byte changed' "$TMP/wp.out"; then
    pass "--watch-break stops the run when a watched byte changes"
else fail "--watch-break did not stop the run"; fi
# The control: the same watch without --watch-break must run to the end.
$EMU --disk "$DISK" --max-steps 6000000 --watch E1AD >"$TMP/wc.out" 2>/dev/null
if grep -q 'hit max-steps' "$TMP/wc.out"; then
    pass "a plain --watch does not stop the run"
else fail "a plain --watch stopped the run"; fi

# The watch report must name the instruction that WROTE the byte, not the one
# after it - the poll happens between instructions, so the naive answer is
# one instruction late.
$EMU --disk "$DISK" --max-steps 6000000 --watch-break E1AD >/dev/null 2>"$TMP/wp2.err"
WROTE=$(sed -n 's/.*written by \$\([0-9A-F]*\).*/\1/p' "$TMP/wp2.err" | head -1)
NOWPC=$(sed -n 's/.*now PC=\$\([0-9A-F]*\).*/\1/p' "$TMP/wp2.err" | head -1)
if [ -n "$WROTE" ] && [ -n "$NOWPC" ]; then
    # The write is an LDIR block copy, which repeats at one address - so here
    # the two agree. What must hold everywhere is that the named writer is an
    # instruction that actually stores: check the opcode at it.
    OPC=$($EMU --disk "$DISK" --max-steps 6000000 --break "$WROTE" \
              --disasm "$WROTE:1" 2>/dev/null | sed -n 's/^  \$[0-9A-F]*: [0-9A-F ]* \(.*\)$/\1/p' | head -1)
    case "$OPC" in
        ld*|ldi*|ldd*|push*|ex*|in*|out*|rst*|call*)
            pass "the watch names a storing instruction ($WROTE: $OPC)" ;;
        *)
            fail "the watch named \$$WROTE, which is '$OPC' - not a write" ;;
    esac
else
    fail "the watch report did not name a writing instruction"
fi

# Breakpoints survived being lifted out of the CLI into core/debug.c.
./p2500-emu --disk "$DISK" --max-steps 900000 --break 0333 --count 0333 \
    >"$TMP/break.out" 2>/dev/null
if grep -q -- '--break reached' "$TMP/break.out" &&
   grep -qE 'Final: PC=\$0333' "$TMP/break.out"; then
    pass "--break stops with PC on the breakpoint"
else fail "--break did not stop at \$0333"; fi

# Symbols, and the two things built on the one-shot breakpoint. All three
# are the headless half of debugger conveniences the GUI grew, so they are
# tested through the CLI for the same reason the panels are: one
# implementation, one answer.
GAMEDISK="disks/P25K_G.raw"
if [ ! -f "$GAMEDISK" ]; then
    echo "  SKIP  symbols/step-over: $GAMEDISK not present"
else
    # Nothing here is a constant of the machine: the table is read out of
    # page zero and whatever address THIS disk's CBIOS was linked at.
    $EMU --disk "$GAMEDISK" --max-steps 3000000 --disasm E200:4 \
        >"$TMP/sym.log" 2>/dev/null
    if grep -q '^  CONST:$' "$TMP/sym.log" &&
       grep -q 'C3 86 E3     jp CONST.body' "$TMP/sym.log"; then
        pass "the CBIOS jump table names itself in a listing"
    else fail "symbols did not reach the disassembly"; fi
    # A name the user gave must survive the scan, not be overwritten by it.
    $EMU --disk "$GAMEDISK" --max-steps 3000000 --symbol E206:POLLKEY \
        --disasm E206:1 >"$TMP/sym2.log" 2>/dev/null
    if grep -q '^  POLLKEY:$' "$TMP/sym2.log"; then
        pass "--symbol wins over the derived name"
    else fail "--symbol was overwritten by the scan"; fi
fi

# Step over a CALL: the one-shot must fire on the instruction AFTER it, with
# the stack back where it started - $1006 is the boot sector's CALL $0003,
# which goes all the way into the IPL's FDC read and back, so a one-shot
# without the SP guard would be no test at all.
./p2500-emu --disk "$DISK" --max-steps 3000000 --step-over 1006 \
    >"$TMP/over.log" 2>/dev/null
if grep -q -- '--step-over \$1006: call \$0003' "$TMP/over.log" &&
   grep -q -- '--step-over returned' "$TMP/over.log" &&
   grep -qE 'Final: PC=\$1009 SP=\$FE6F' "$TMP/over.log"; then
    pass "--step-over runs a CALL to completion and stops after it"
else fail "--step-over did not return to the instruction after the CALL"; fi

# And on something that is not a CALL it must say so and single-step, not
# set a breakpoint on the following address - a JP never arrives there.
./p2500-emu --disk "$DISK" --max-steps 3000000 --step-over 1002 \
    >"$TMP/over2.log" 2>/dev/null
if grep -q -- '--step-over: not a CALL' "$TMP/over2.log" &&
   grep -q -- 'one instruction executed' "$TMP/over2.log" &&
   grep -qE 'Final: PC=\$1003' "$TMP/over2.log"; then
    pass "--step-over single-steps anything that is not a CALL"
else fail "--step-over mishandled a non-CALL"; fi
}

sec_9() {
echo "== 9. Core diagnostics go through the log callback"
# The core never touches stderr directly; the CLI installs a sink that adds
# the "[cat] " prefix. Check every device category still reaches it.
./p2500-emu --disk "$DISK" --max-steps 4000000 --verbose-io --type 'DIR\r' \
    >/dev/null 2>"$TMP/verbose.err"
missing=""
for cat in ctc dma fdc io int pio cartridge; do
    grep -q "^\[$cat\] " "$TMP/verbose.err" || missing="$missing $cat"
done
if [ -z "$missing" ]; then
    pass "every device category still reaches the log ($(wc -l <"$TMP/verbose.err") lines)"
else fail "no output from category:$missing"; fi

if [ -x ./p2500-gui ]; then
    # The GUI's log panel is fed by the same sink. With device logging off it
    # says so; with --verbose-io it fills, so the panel must carry more ink.
    for v in off on; do
        [ "$v" = on ] && V=--verbose-io || V=
        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 300 --panels log \
            $V --shot-window "$TMP/log-$v.ppm" >>"$TMP/gui.log" 2>&1
    done
    python3 - "$TMP/log-off.ppm" "$TMP/log-on.ppm" <<'PY' && pass "the log panel is fed by the core" || fail "the log panel got nothing from the core"
import sys
def load(p):
    d = open(p, 'rb').read()
    hdr = d.split(b'\n', 3)
    w, h = (int(v) for v in hdr[1].split())
    return w, h, d[len(b'\n'.join(hdr[:3])) + 1:]
w, h, off = load(sys.argv[1])
_, _, on = load(sys.argv[2])
def ink(px):
    return sum(1 for i in range(0, len(px), 3) if px[i + 1] > 0x40)
a, b = ink(off), ink(on)
if b <= a * 1.2:
    print("    log panel unchanged: quiet %d lit px, verbose %d" % (a, b)); sys.exit(1)
sys.exit(0)
PY
fi
}

sec_10() {
echo "== 10. Building a disk image (tools/cpm_build.py)"
# Build a bootable disk from scratch, boot it, and run a program off it -
# which exercises the sector skew, the directory encoding, the block map
# and the system-file copy in one go. A wrong skew fails here
# and not subtly.
if python3 tests/mk_cpm_probe.py "$TMP/PROBE.COM" 'BEFORE\eY\x20\x20\ek\e0PREVERSED\e0@ plain' >/dev/null 2>&1 &&
   python3 tools/cpm_build.py "$TMP/probe.raw" "$TMP/PROBE.COM" \
        --boot-from "$DISK" >"$TMP/build.log" 2>&1; then
    pass "cpm_build.py built a bootable image ($(sed -n 's/^Verified *: .*, \([0-9]*\) file.*/\1/p' "$TMP/build.log") files verified)"
else
    fail "cpm_build.py failed"
    sed 's/^/    /' "$TMP/build.log"
fi

if [ -s "$TMP/probe.raw" ]; then
    $EMU --disk "$TMP/probe.raw" --max-steps 8000000 --type-at '4000:dir\r' \
         --dump-vram "$TMP/built.bin" >"$TMP/built.log" 2>&1
    screen "$TMP/built.bin" >"$TMP/built.screen"
    if grep -q '58K CP/M Ver. 2.2' "$TMP/built.screen"; then
        pass "a generated disk boots CP/M"
    else fail "a generated disk did not boot"; fi
    if grep -q 'PROBE    COM' "$TMP/built.screen"; then
        pass "DIR lists the file that was written into it"
    else fail "the written file is not in the directory"; fi

    # Running it proves the data blocks landed where the directory says, not
    # merely that the directory parses.
    $EMU --disk "$TMP/probe.raw" --max-steps 20000000 --type-at '4000:probe\r' \
         --dump-vram "$TMP/pr.bin" --dump-vram-attr "$TMP/pr.attr" \
         >"$TMP/probe.log" 2>&1
    screen "$TMP/pr.bin" >"$TMP/pr.screen"
    # The probe clears the screen first, so the CP/M banner must be gone.
    if grep -q 'REVERSED plain' "$TMP/pr.screen" &&
       ! grep -q '58K CP/M' "$TMP/pr.screen"; then
        pass "a program written by cpm_build.py loads and runs"
    else fail "the written program did not run"; fi
    # ESC 0 P must set the reverse bit on exactly the eight characters of
    # "REVERSED" and on nothing after ESC 0 @.
    python3 - "$TMP/pr.bin" "$TMP/pr.attr" <<'PY' && pass "ESC 0 P sets the reverse attribute, ESC 0 @ clears it" || fail "the attribute plane is wrong"
import sys
ch = open(sys.argv[1], 'rb').read()
at = open(sys.argv[2], 'rb').read()
row = ch[:80].decode('latin-1')
col = row.find('REVERSED')
if col < 0:
    print("    'REVERSED' not on the top row: %r" % row.rstrip()); sys.exit(1)
rev = [at[col + i] & 0x0F for i in range(8)]
after = [at[col + 8 + i] & 0x0F for i in range(6)]   # " plain"
if any(a != 0x04 for a in rev):
    print("    reverse run is %r, wanted eight 4s" % rev); sys.exit(1)
if any(a != 0x00 for a in after):
    print("    attribute leaked past ESC 0 @: %r" % after); sys.exit(1)
sys.exit(0)
PY
fi
}

sec_11() {
echo "== 11. High-resolution graphics mode"
# Driven through CBIOS's own set-point call, so what is being checked is the
# whole path: ESC 3 reprograms the CRTC, the firmware computes an address and
# a bit, and the renderer turns that back into the pixel the program asked
# for. The pattern is an L plus one far dot - asymmetric in both axes, so a
# transpose or a flip cannot pass it.
python3 - >"$TMP/gfx.seq" <<'PY'
seq = '\\e3'
pts  = [(x, 0) for x in range(0, 9)] + [(x, 0) for x in range(10, 21)]
pts += [(0, y) for y in range(1, 9)] + [(0, y) for y in range(10, 16)]
pts += [(100, 100)]
# $09 in a coordinate is eaten as a TAB by the console path - measured, and
# the reason the runs above skip 9.
for x, y in pts:
    seq += '\\x01\\x%02x\\x%02x\\x%02x' % (x & 0xFF, x >> 8, y)
print(seq)
PY
python3 tests/mk_cpm_probe.py "$TMP/GFX.COM" "$(cat "$TMP/gfx.seq")" >/dev/null
python3 tools/cpm_build.py "$TMP/gfx.raw" "$TMP/GFX.COM:G.COM" --boot-from "$DISK" >/dev/null 2>&1
$EMU --disk "$TMP/gfx.raw" --max-steps 25000000 --type-at '4000:g\r' \
     --dump-screen "$TMP/gfx.ppm" --state >"$TMP/gfx.log" 2>&1
if grep -q 'HIGH-RESOLUTION GRAPHICS' "$TMP/gfx.log"; then
    pass "ESC 3 puts port \$0A in graphics mode"
else fail "ESC 3 did not select graphics mode"; fi
# The resolution is not asserted as a constant anywhere - it falls out of the
# CRTC registers CBIOS programs on the way in.
if grep -q '512 x 256 px' "$TMP/gfx.log"; then
    pass "CBIOS reprograms the CRTC to 512 x 256"
else fail "graphics geometry is not 512 x 256"; fi
python3 - "$TMP/gfx.ppm" <<'PY' && pass "set points render where the program put them" || fail "graphics pixels are in the wrong place"
import sys
d = open(sys.argv[1], 'rb').read()
hdr = d.split(b'\n', 3)
w, h = (int(v) for v in hdr[1].split())
px = d[len(b'\n'.join(hdr[:3])) + 1:]
def lit(x, y):
    return px[((y * w) + x) * 3 + 1] > 0x80
ok = (w, h) == (512, 256)
if not ok:
    print("    wrong geometry %dx%d" % (w, h))
for (x, y), want in [((4, 0), True), ((18, 0), True), ((0, 4), True), ((0, 14), True),
                     ((100, 100), True),
                     ((4, 4), False), ((50, 50), False), ((30, 0), False), ((0, 30), False)]:
    if lit(x, y) != want:
        print("    (%d,%d) is %s, wanted %s" % (x, y, lit(x, y), want)); ok = False
sys.exit(0 if ok else 1)
PY
}

sec_12() {
echo "== 12. The graphics demos (demos/)"
# The whole chain: assemble with tools/z80asm.py, which verifies its own
# encodings against the disassembler; package onto a bootable image with
# cpm_build.py; boot it and run one. A demo drawing the right thing is a
# stronger statement about the graphics layout than any probe, because it
# writes video memory directly rather than through CBIOS - so the emulator's
# renderer and the layout in demos/p2500.inc have to agree independently.
demo_ok=yes
for src in demos/logo.asm demos/stars.asm demos/spiro.asm demos/bench.asm; do
    if ! python3 tools/z80asm.py "$src" -o "$TMP/$(basename "$src" .asm).COM" --verify \
            >"$TMP/asm.log" 2>&1; then
        fail "$src did not assemble and verify"
        sed 's/^/    /' "$TMP/asm.log" | head -6
        demo_ok=no
    fi
done
[ "$demo_ok" = yes ] && pass "all three demos assemble and round-trip through the disassembler"

if [ "$demo_ok" = yes ]; then
    python3 tools/cpm_build.py "$TMP/demo.raw" "$TMP/logo.COM:LOGO.COM" \
        "$TMP/stars.COM:STARS.COM" "$TMP/spiro.COM:SPIRO.COM" \
        "$TMP/bench.COM:BENCH.COM" \
        --boot-from "$DISK" >"$TMP/demobuild.log" 2>&1
    $EMU --disk "$TMP/demo.raw" --max-steps 8000000 --type-at '4000:dir\r' \
         --dump-vram "$TMP/demodir.bin" >/dev/null 2>&1
    screen "$TMP/demodir.bin" >"$TMP/demodir.screen"
    if grep -q 'LOGO     COM' "$TMP/demodir.screen" &&
       grep -q 'SPIRO    COM' "$TMP/demodir.screen"; then
        pass "the demo disk boots and lists its programs"
    else fail "the demo disk did not boot"; fi

    # STARS draws exactly one pixel per star and erases the previous one, so
    # a correct run shows close to 48 lit pixels - many fewer means it is not
    # drawing, many more means the erase is missing and it is leaving trails.
    $EMU --disk "$TMP/demo.raw" --max-steps 45000000 --type-at '4000:stars\r' \
         --no-stuck-detect --dump-screen "$TMP/stars.ppm" >/dev/null 2>&1
    python3 - "$TMP/stars.ppm" <<'PY' && pass "STARS draws a starfield and erases behind itself" || fail "STARS did not draw correctly"
import sys
d = open(sys.argv[1], 'rb').read()
hdr = d.split(b'\n', 3)
w, h = (int(v) for v in hdr[1].split())
px = d[len(b'\n'.join(hdr[:3])) + 1:]
lit = [(x, y) for y in range(h) for x in range(w)
       if px[((y * w) + x) * 3 + 1] > 0x80]
if (w, h) != (512, 256):
    print("    wrong geometry %dx%d" % (w, h)); sys.exit(1)
if not 30 <= len(lit) <= 60:
    print("    %d lit pixels, wanted about 48 - trails or nothing drawn" % len(lit))
    sys.exit(1)
# They should be spread out, not clustered in one corner.
if max(x for x, _ in lit) - min(x for x, _ in lit) < 200:
    print("    stars are not spread across the screen"); sys.exit(1)
sys.exit(0)
PY

    # BENCH times each category against the 50 Hz tick counter, so the same
    # binary gives comparable numbers here and on real hardware. What is
    # asserted is the emulator's own position: it charges a write to the video
    # window exactly what it charges main RAM, because it models no CRTC
    # contention at all. If someone gives it a wait-state model, this check
    # fails and makes them say so on purpose rather than by accident.
    $EMU --disk "$TMP/demo.raw" --max-steps 120000000 --type-at '4000:bench\r' \
         --no-stuck-detect --dump-vram "$TMP/bench.bin" >/dev/null 2>&1
    screen "$TMP/bench.bin" >"$TMP/bench.screen"
    python3 - "$TMP/bench.screen" <<'PY' && pass "BENCH reports every category, and VRAM costs what RAM costs" || fail "BENCH did not report as expected"
import re, sys
text = open(sys.argv[1]).read()
rows = {}
for line in text.splitlines():
    m = re.match(r'\s*(CPU regs|RAM write|RAM read|RAM ldir|VID write T|VID ldir  T'
                 r'|VID write G|VID ldir  G|Firmware  \.|Console   \.|Disk read \.)'
                 r'\s+(\d+)\s*$', line)
    if m:
        rows[m.group(1).strip()] = int(m.group(2))
want = ['CPU regs', 'RAM write', 'RAM read', 'RAM ldir', 'VID write T',
        'VID ldir  T', 'VID write G', 'VID ldir  G', 'Firmware  .',
        'Console   .', 'Disk read .']
missing = [w for w in want if w not in rows]
if missing:
    print("    missing rows: %s" % missing); sys.exit(1)
if any(v == 0 for k, v in rows.items() if k != 'Disk read .'):
    print("    a test measured zero ticks: %s" % rows); sys.exit(1)
# The emulator models no video contention, so these must match exactly.
for vid, ram in (('VID write T', 'RAM write'), ('VID write G', 'RAM write'),
                 ('VID ldir  T', 'RAM ldir'), ('VID ldir  G', 'RAM ldir')):
    if rows[vid] != rows[ram]:
        print("    %s (%d) != %s (%d) - the emulator has grown a VRAM timing "
              "model; update this check deliberately"
              % (vid, rows[vid], ram, rows[ram]))
        sys.exit(1)
# Firmware plotting must be vastly dearer than direct writes, or the demos'
# whole reason for bypassing CBIOS has gone away.
if rows['Firmware  .'] * 100 < rows['RAM write']:
    print("    firmware plotting looks implausibly cheap: %s" % rows); sys.exit(1)
sys.exit(0)
PY
    # LOGO is the only demo that walks down the screen a row at a time, so it
    # is the only one that exercises next_row - the raster-bank wrap that
    # makes row y+1 sometimes +$1000 and sometimes -12224.
    #
    # Three instants, several seconds apart and deterministic. Each one that
    # catches the sprite between blits must show exactly one clean 144 x 45
    # logo - which is a trail check as much as a blit check, since with no
    # erase pass a mis-stepped bounce leaves a second copy behind.
    #
    # And the positions must differ vertically. That is not fussiness: a
    # sign-test bug once pinned the logo to y=0 so it only ever travelled
    # horizontally, and every position-independent assertion here passed
    # while it did, because 45 contiguous rows at the top of the screen look
    # exactly like 45 contiguous rows anywhere else.
    : >"$TMP/logo.rows"
    for steps in 8000000 16000000 30000000; do
        $EMU --disk "$TMP/demo.raw" --max-steps $steps --type-at '4000:logo\r' \
             --no-stuck-detect --dump-screen "$TMP/logo.ppm" >/dev/null 2>&1
        python3 - "$TMP/logo.ppm" >>"$TMP/logo.rows" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
hdr = d.split(b'\n', 3)
w, h = (int(v) for v in hdr[1].split())
px = d[len(b'\n'.join(hdr[:3])) + 1:]
def lit(x, y):
    return px[((y * w) + x) * 3 + 1] > 0x80
rows = [y for y in range(h) if any(lit(x, y) for x in range(w))]
cols = [x for x in range(w) if any(lit(x, y) for y in range(h))]
span = (cols[-1] - cols[0] + 1) if cols else 0
print("%d %d %d" % (len(rows), span, rows[0] if rows else -1))
PY
    done
    python3 - "$TMP/logo.rows" <<'PY' && pass "LOGO blits one clean sprite and bounces in both axes" || fail "LOGO blit or movement is wrong"
import sys
samples = [tuple(int(v) for v in l.split()) for l in open(sys.argv[1]) if l.strip()]
if len(samples) != 3:
    print("    expected three samples, got %d" % len(samples)); sys.exit(1)
# At least one instant must land between blits and show the sprite whole.
clean = [s for s in samples if s[0] == 45 and 144 <= s[1] <= 145]
if not clean:
    print("    never showed one clean 144x45 logo: %s" % (samples,)); sys.exit(1)
# Nothing may ever show MORE than the sprite: that is a trail, not a
# part-drawn frame. 49 is the blit height, so a mid-blit union can reach it.
if any(s[0] > 49 or s[1] > 145 for s in samples):
    print("    a sample is larger than the sprite - trail left behind: %s"
          % (samples,)); sys.exit(1)
tops = {s[2] for s in samples}
if len(tops) < 2:
    print("    the logo never moved vertically (top row always %s) - it is "
          "travelling horizontally" % tops); sys.exit(1)
sys.exit(0)
PY
fi
}

sec_13() {
echo "== 13. The speed control"
# Every claim here is about the RATIO of emulated time to wall time, taken
# from the front-end's own report - which reads the CPU's T-state counter, so
# it cannot agree with the setting by construction the way a count of
# iterations would.
#
# Tolerances are one-sided in spirit: a loaded host can only ever come out
# slow, so the floor is what catches a broken throttle and the ceiling is
# what catches a setting that did nothing.
ratio() {
    sed -n 's/.*of wall clock (\([0-9.]*\)x).*/\1/p' "$1" | tail -1
}
if [ -x ./p2500-gui ] && [ -f "$DISK" ]; then
    for spec in "2 1.6 2.4" "0.5 0.40 0.60" "0.25 0.20 0.30"; do
        # shellcheck disable=SC2086
        set -- $spec
        SPEED=$1; LO=$2; HI=$3
        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 60 --paced \
            --speed "$SPEED" >"$TMP/speed-$SPEED.log" 2>&1
        R=$(ratio "$TMP/speed-$SPEED.log")
        F=$(sed -n 's/.*(\([0-9.]*\) frames\/s.*/\1/p' "$TMP/speed-$SPEED.log" | tail -1)
        if [ -z "$R" ]; then
            fail "no speed report at ${SPEED}x"
        elif awk "BEGIN{exit !($R >= $LO && $R <= $HI)}"; then
            # The other half of the claim, and the one that is easy to get
            # wrong: changing the speed must not change how often the screen
            # is drawn. Below 1x the budget shrinks; above it, one iteration
            # covers several fields.
            if awk "BEGIN{exit !($F >= 40 && $F <= 56)}"; then
                pass "${SPEED}x runs at ${R}x with the display still at ${F} fps"
            else fail "${SPEED}x changed the presented frame rate to $F"; fi
        else fail "--speed $SPEED gave ${R}x, wanted $LO-$HI"; fi
    done

    # Unlimited: pacing is off, so the only claim is that it is much faster
    # than real time. Deliberately loose - it measures the host, not the
    # emulator.
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 40 \
        --speed unlimited >"$TMP/speed-max.log" 2>&1
    R=$(ratio "$TMP/speed-max.log")
    if [ -n "$R" ] && awk "BEGIN{exit !($R > 3)}"; then
        pass "unlimited runs flat out (${R}x real time here)"
    else fail "unlimited only managed ${R:-no}x"; fi

    # The status-bar cell is a control: click toggles, right-click opens the
    # list. The list's rows come out of the front-end's own report, so adding
    # a speed to it cannot silently stop this from testing anything.
    gui_report
    L_SPEED=$(sed -n 's/.*lamps: .*speed \([0-9]*\).*/\1/p' \
              "$TMP/gui_report.log" | head -1)
    if [ -z "$L_SPEED" ]; then
        fail "the front-end did not report the speed cell's column"
    else
        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 30 \
            --mouse "$L_SPEED,11,left" >"$TMP/speed-click.log" 2>&1
        if grep -q 'speed: unlimited' "$TMP/speed-click.log"; then
            pass "clicking the speed cell toggles unlimited"
        else fail "clicking the speed cell did nothing"; fi

        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 20 \
            --mouse "6:$L_SPEED,11,right" >"$TMP/speed-list.log" 2>&1
        ROW=$(sed -n 's/.*speed menu:.* 0\.5x \([0-9]*\),\([0-9]*\).*/\1 \2/p' \
              "$TMP/speed-list.log" | head -1)
        if [ -z "$ROW" ]; then
            fail "right-clicking the speed cell did not open the list"
        else
            # shellcheck disable=SC2086
            set -- $ROW
            SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 40 \
                --mouse "6:$L_SPEED,11,right" --mouse "14:$1,$2,left" \
                >"$TMP/speed-pick.log" 2>&1
            if grep -q 'speed: 0.5x' "$TMP/speed-pick.log"; then
                pass "picking a speed from the list sets it"
            else fail "picking 0.5x from the list did nothing"; fi
        fi
    fi
else
    echo "  (skipped: no p2500-gui or no disk image)"
fi

# The CLI's throttle. Default is unlimited - every other check in this file
# depends on that - so the flag is what gets tested, not the default.
if [ -f "$DISK" ]; then
    $EMU --disk "$DISK" --max-steps 400000 --speed 1 >"$TMP/cli-speed.log" 2>&1
    R=$(sed -n 's/^Host speed: \([0-9.]*\)x.*/\1/p' "$TMP/cli-speed.log")
    if [ -n "$R" ] && awk "BEGIN{exit !($R >= 0.85 && $R <= 1.15)}"; then
        pass "p2500-emu --speed 1 holds the run to real time (${R}x)"
    else fail "p2500-emu --speed 1 gave ${R:-no}x"; fi
    $EMU --disk "$DISK" --max-steps 400000 >"$TMP/cli-flat.log" 2>&1
    R=$(sed -n 's/^Host speed: \([0-9.]*\)x.*/\1/p' "$TMP/cli-flat.log")
    if [ -n "$R" ] && awk "BEGIN{exit !($R > 2)}"; then
        pass "p2500-emu runs flat out by default (${R}x real time here)"
    else fail "the default CLI run is throttled (${R:-no}x)"; fi
fi
}

sec_14() {
echo "== 14. A multi-sector read is bounded by EOT, not by the end of the image"
# tests/fdc_eot.c links libp2500.a directly and programs the DMA for more
# than a track holds, which no guest program in this project's corpus does -
# every traced READ DATA asks for exactly one track or exactly one sector,
# so the bound is unreachable from here and this check has to reach past
# the harness to see it.
#
# Its output is already one line per claim, so those lines are replayed
# through pass/fail rather than forwarded verbatim: they then count towards
# this script's total instead of merely appearing in it. Read from files
# rather than a pipe, because a `while read` on the end of a pipe runs in a
# subshell and the count would not survive it.
if [ -x ./tests/fdc_eot ]; then
    ./tests/fdc_eot >"$TMP/fdc-eot.log" 2>&1
    eot=$?
    sed -n 's/^  PASS  //p' "$TMP/fdc-eot.log" >"$TMP/fdc-eot.pass"
    sed -n 's/^  FAIL  //p' "$TMP/fdc-eot.log" >"$TMP/fdc-eot.fail"
    while IFS= read -r c; do pass "$c"; done <"$TMP/fdc-eot.pass"
    while IFS= read -r c; do fail "$c"; done <"$TMP/fdc-eot.fail"
    # Neither silence nor an unexplained exit code may read as success.
    if [ ! -s "$TMP/fdc-eot.pass" ] && [ ! -s "$TMP/fdc-eot.fail" ]; then
        fail "tests/fdc_eot named no checks at all"
    elif [ "$eot" -ne 0 ] && [ ! -s "$TMP/fdc-eot.fail" ]; then
        fail "tests/fdc_eot exited $eot without naming a failed check"
    fi
else
    fail "tests/fdc_eot is not built (make tests/fdc_eot)"
fi
}

sec_15() {
echo "== 15. Rewind: seek to an instruction that has already run"
# The claim the whole feature rests on is that a replay from a snapshot is
# bit-identical, so that is checked first and directly - go back, come
# forward, compare a digest of the entire P2500Machine - rather than
# inferred from a PC landing where it was expected.
if [ -f "$DISK" ]; then
    $EMU --disk "$DISK" --break 4A00 --verify-rewind 1 >"$TMP/rw1.log" 2>&1
    if grep -q 'rewind: verified' "$TMP/rw1.log"; then
        pass "one instruction back and forward again is bit-identical"
    else fail "a one-instruction rewind did not come back to the same state"; fi

    # Across many snapshot boundaries, which is where a cadence or ring bug
    # would show: 250k instructions is ~12 of the 64 slots.
    $EMU --disk "$DISK" --break 4A00 --verify-rewind 250000 >"$TMP/rw2.log" 2>&1
    if grep -q 'rewind: verified' "$TMP/rw2.log"; then
        pass "250k instructions back and forward again is bit-identical"
    else fail "a long rewind did not reproduce the state"; fi

    # The negative control for both: asked for more history than is held,
    # the check must REFUSE and exit non-zero. Without this, "verified"
    # could mean "nothing was tried".
    if $EMU --disk "$DISK" --break 4A00 --verify-rewind 900000 \
            >"$TMP/rw3.log" 2>&1; then
        fail "--verify-rewind returned success with no history to rewind"
    elif grep -q 'rewind: cannot step back 900000' "$TMP/rw3.log"; then
        pass "a rewind past the oldest snapshot is refused, not faked"
    else fail "--verify-rewind failed for the wrong reason"; fi

    # One instruction back from the boot sector's entry is $1014, the IPL's
    # JP $4A00 - which is only the right answer if the replay stopped at
    # exactly the right boundary.
    $EMU --disk "$DISK" --break 4A00 --step-back 1 --disasm 1014:1 \
        >"$TMP/rw4.log" 2>&1
    if grep -q 'rewind: stepped back 1 to instruction 801122, PC=\$1014' "$TMP/rw4.log" &&
       grep -qE 'Final: PC=\$1014 ' "$TMP/rw4.log" &&
       grep -q 'C3 00 4A     jp \$4A00' "$TMP/rw4.log"; then
        pass "--step-back 1 lands on the instruction that jumped here"
    else fail "--step-back 1 did not land on \$1014"; fi
    # The pair that makes the one above mean something: the same run whose
    # rewind was a round trip still ends where it stopped.
    if grep -qE 'Final: PC=\$4A00 ' "$TMP/rw1.log"; then
        pass "a round-trip rewind leaves the run ending at \$4A00"
    else fail "the round-trip run did not end at \$4A00"; fi

    # --seek takes an absolute instruction number, which is the primitive
    # the rest are built on; 801000 is 123 instructions before the stop.
    $EMU --disk "$DISK" --break 4A00 --seek 801000 >"$TMP/rw4b.log" 2>&1
    if grep -q 'rewind: sought to instruction 801000' "$TMP/rw4b.log"; then
        pass "--seek goes to an absolute instruction number"
    else fail "--seek did not land on instruction 801000"; fi

    # Run backwards to the last time a breakpoint was standing at the PC.
    # --step-over passes THROUGH its breakpoint at $1006 and stops 1161
    # instructions later, so there is a real earlier hit to find.
    $EMU --disk "$DISK" --max-steps 3000000 --step-over 1006 --back-to-break \
        >"$TMP/rw5.log" 2>&1
    if grep -q 'rewind: back to breakpoint \$1006 at instruction 791894' "$TMP/rw5.log" &&
       grep -qE 'Final: PC=\$1006 ' "$TMP/rw5.log"; then
        pass "--back-to-break runs backwards to the CALL it stepped over"
    else fail "--back-to-break did not reach the earlier breakpoint hit"; fi
    # Its negative control: a run that stopped ON its only breakpoint hit
    # has nothing earlier to go back to, and must say so.
    $EMU --disk "$DISK" --break 4A00 --back-to-break >"$TMP/rw6.log" 2>&1
    if grep -q 'rewind: no earlier breakpoint' "$TMP/rw6.log"; then
        pass "with no earlier hit, --back-to-break refuses"
    else fail "--back-to-break claimed a hit that never happened"; fi

    # --back-to ADDR, which is the sound version of a reverse step-over
    # (see core/history.h): $06C6 is the IPL's busy-wait, executed 8000
    # instructions before the boot sector runs.
    $EMU --disk "$DISK" --break 4A00 --back-to 06C6 >"$TMP/rw7.log" 2>&1
    if grep -q 'rewind: back to \$06C6' "$TMP/rw7.log" &&
       grep -qE 'Final: PC=\$06C6 ' "$TMP/rw7.log"; then
        pass "--back-to finds the last visit to an address, across snapshots"
    else fail "--back-to did not reach \$06C6"; fi
    # A failed search must leave the machine exactly where it was, not
    # wherever the scan happened to end. $0BFD is never executed.
    $EMU --disk "$DISK" --break 4A00 --state --back-to 0BFD \
        >"$TMP/rw8.log" 2>&1
    $EMU --disk "$DISK" --break 4A00 --state >"$TMP/rw8-ctl.log" 2>&1
    grep -v 'rewind:\|Host speed\|Emulated time' "$TMP/rw8.log" >"$TMP/rw8.cmp"
    grep -v 'Host speed\|Emulated time' "$TMP/rw8-ctl.log" >"$TMP/rw8-ctl.cmp"
    if grep -q 'rewind: \$0BFD is not in the recorded history' "$TMP/rw8.log" &&
       cmp -s "$TMP/rw8.cmp" "$TMP/rw8-ctl.cmp"; then
        pass "a failed search leaves the machine bit-identical"
    else fail "a failed --back-to moved the machine"; fi

    # The replay must be SILENT: it runs bare p2500_step() with the log
    # detached, so a 250k-instruction rewind must not re-emit one line of
    # the landmark and watch reporting it passes back through. 27 of the 29
    # lines this run produces are inside that window, so a non-silent
    # replay would be unmissable.
    $EMU --disk "$DISK" --break 4A00 2>"$TMP/rw9-ctl.err" >/dev/null
    $EMU --disk "$DISK" --break 4A00 --step-back 250000 2>"$TMP/rw9.err" >/dev/null
    A=$(wc -l <"$TMP/rw9-ctl.err"); B=$(wc -l <"$TMP/rw9.err")
    if [ "$A" = "$B" ]; then
        pass "rewinding 250k instructions emits no diagnostics ($A lines either way)"
    else fail "the replay was not silent ($A lines without it, $B with)"; fi

    # Snapshots are opt-in: nothing records history unless asked, so a
    # plain run must not mention it at all.
    $EMU --disk "$DISK" --max-steps 400000 >"$TMP/rw-plain.log" 2>&1
    if grep -q 'rewind:' "$TMP/rw9-ctl.err" ||
       grep -q 'rewind:' "$TMP/rw-plain.log"; then
        fail "history was recorded without being asked for"
    else pass "history is opt-in - a plain run says nothing about rewind"; fi
fi

# The GUI's Step back button. Driven by its reported position, like the
# memory panel's controls, so the suite never pins a pixel column that any
# layout change would invalidate. The button is disabled while the machine
# is running, which is why this needs a breakpoint stop first.
if [ -x ./p2500-gui ] && [ -f "$DISK" ]; then
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 8 --panels disasm \
        --shot-window "$TMP/rwgeom.ppm" >"$TMP/rwgeom.log" 2>&1
    BACK=$(sed -n 's/.*step-back button \([0-9]*\),\([0-9]*\).*/\1,\2/p' "$TMP/rwgeom.log" | head -1)
    BRK=$(sed -n 's/.*back-to-break button \([0-9]*\),\([0-9]*\).*/\1,\2/p' "$TMP/rwgeom.log" | head -1)
    if [ -z "$BACK" ] || [ -z "$BRK" ]; then
        fail "the disassembly panel did not report its rewind controls"
    else
        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 140 \
            --panels disasm --break 4A00 --mouse "90:$BACK,left" \
            >"$TMP/rwgui.log" 2>&1
        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 140 \
            --panels disasm --break 4A00 >"$TMP/rwgui-ctl.log" 2>&1
        if grep -q 'rewind: 1 instruction(s) back to \$1014' "$TMP/rwgui.log"; then
            pass "clicking Step back rewinds the machine one instruction"
        else fail "the Step back button did nothing"; fi
        if grep -q 'rewind:' "$TMP/rwgui-ctl.log"; then
            fail "the GUI rewound without the button being clicked"
        else pass "no click, no rewind - the button is what does it"; fi
        # And the refusal is reported too, rather than being a silent no-op:
        # $4A00 is reached once, so there is no earlier hit to go back to.
        SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 140 \
            --panels disasm --break 4A00 --mouse "90:$BRK,left" \
            >"$TMP/rwgui2.log" 2>&1
        if grep -q 'rewind: nothing to rewind to' "$TMP/rwgui2.log"; then
            pass "Back to brk says so when there is nothing to go back to"
        else fail "Back to brk failed silently"; fi
    fi
fi
}

sec_16() {
echo "== 16. Symbol files, and --help"
# Two claims. The small one: both front-ends answer --help on stdout with
# status 0, and an unknown argument is still an error - a usage text that
# only appears on failure is a usage text nobody can ask for.
# The real one: the addresses in symbols/*.sym agree with the machine. A
# name is a claim about where something lives, so each file is checked by
# disassembling at an address that USES it and reading the operand back.
for prog in "$EMU" ./p2500-gui; do
    if [ ! -x "$prog" ]; then echo "  SKIP  $prog not built"; continue; fi
    if "$prog" --help >"$TMP/help.out" 2>"$TMP/help.err" &&
       grep -q 'usage:' "$TMP/help.out"; then
        pass "$prog --help prints usage on stdout, exit 0"
    else fail "$prog --help did not print usage with status 0"; fi
    if "$prog" --no-such-flag >"$TMP/bad.out" 2>"$TMP/bad.err"; then
        fail "$prog accepted --no-such-flag"
    else pass "$prog rejects an unknown argument"; fi
done

# $02E3 is `ld hl,$FE0A`, the IPL loading the address of its own device
# dispatch table - so ipl.sym's $FE0A entry has to show up as the operand.
$EMU --symbols symbols/ipl.sym --max-steps 1 --disasm 02E3:1 \
     >"$TMP/sym_ipl.log" 2>&1
if grep -q 'ld hl,DEV.DISPATCH' "$TMP/sym_ipl.log"; then
    pass "symbols/ipl.sym names \$FE0A where the IPL loads it"
else fail "ipl.sym did not name \$FE0A at \$02E3"; fi

if [ -f "$DISK" ]; then
    # Same for the CP/M-era file, which is the build-specific one: $E4B7 is
    # CONIN reading the capitals-lock mask at $E34C. Both names come out of
    # cpm.sym, and CONIN - which does NOT - comes from the live scan, so
    # one listing proves the two sources coexist.
    $EMU --disk "$DISK" --symbols symbols/cpm.sym --max-steps 3000000 \
         --disasm E4B7:1 --disasm E209:1 >"$TMP/sym_cpm.log" 2>&1
    if grep -q 'CONIN.XOR:' "$TMP/sym_cpm.log" &&
       grep -q 'ld a,(CAPS.MASK)' "$TMP/sym_cpm.log"; then
        pass "symbols/cpm.sym names \$E34C where CONIN reads it"
    else fail "cpm.sym did not name \$E34C at \$E4B7"; fi
    if grep -q 'jp CONIN.body' "$TMP/sym_cpm.log"; then
        pass "the live CBIOS scan still runs with a symbol file loaded"
    else fail "a loaded symbol file suppressed the CBIOS scan"; fi

    # A name the user wrote down wins over the one the scan would derive.
    printf 'E206 MYCONST\n' >"$TMP/mine.sym"
    $EMU --disk "$DISK" --symbols "$TMP/mine.sym" --max-steps 3000000 \
         --disasm E206:1 >"$TMP/sym_win.log" 2>&1
    if grep -q 'MYCONST:' "$TMP/sym_win.log" &&
       ! grep -q '^ *CONST:' "$TMP/sym_win.log"; then
        pass "a --symbols name overrides the scan's own"
    else fail "the scan overwrote a name from --symbols"; fi
fi

# cpm.sym claims to hold for every disk of P25K_B's BIOS generation and
# NOT for P25K_G's, which is a falsifiable claim about three disks.
for d in P25K_S P25TEST; do
    if [ ! -f "disks/$d.raw" ]; then echo "  SKIP  disks/$d.raw not present"; continue; fi
    $EMU --disk "disks/$d.raw" --symbols symbols/cpm.sym --max-steps 3000000 \
         --disasm E4B7:1 >"$TMP/sym_$d.log" 2>&1
    if grep -q 'ld a,(CAPS.MASK)' "$TMP/sym_$d.log"; then
        pass "symbols/cpm.sym holds on $d too"
    else fail "cpm.sym's \$E34C does not hold on $d - the file's claim is wrong"; fi
done

if [ -f disks/P25K_G.raw ]; then
    # The older PHILIPS P2000B generation. cpm.sym must NOT fit it - if this
    # ever passes, that file has been generalised past what was measured.
    $EMU --disk disks/P25K_G.raw --max-steps 3000000 --disasm E4B7:1 \
         >"$TMP/sym_g.log" 2>&1
    if grep -q 'ld a,(\$E34C)' "$TMP/sym_g.log"; then
        fail "P25K_G now looks like P25K_B at \$E4B7 - re-measure cpm.sym"
    else pass "P25K_G is a different build, as symbols/cpm.sym says"; fi
    # And what p25k_g.sym does claim about it is checked the same way as
    # the other files: at addresses whose contents say so. Four of them,
    # one per way those names were derived - the IM2 trampolines, the
    # device table, G's own SELDSK, and lockstep disassembly against
    # P25K_B.
    $EMU --disk disks/P25K_G.raw --symbols symbols/p25k_g.sym \
         --max-steps 3000000 --disasm EC9D:2 --disasm E46B:1 \
         --disasm EE8C:1 --disasm E3A8:2 >"$TMP/sym_g2.log" 2>&1
    if grep -q 'EI.RETI:' "$TMP/sym_g2.log" &&
       grep -q 'reti' "$TMP/sym_g2.log"; then
        pass "symbols/p25k_g.sym names P25K_G's EI/RETI correctly"
    else fail "p25k_g.sym's \$EC9D is not an EI/RETI"; fi
    if grep -q 'ld hl,(DRIVEMAP.PTR)' "$TMP/sym_g2.log"; then
        pass "p25k_g.sym's drive map pointer is where SELDSK reads it"
    else fail "p25k_g.sym's \$E2A7 is not what SELDSK reads"; fi
    if grep -q 'ld hl,SCREEN.FNTAB' "$TMP/sym_g2.log"; then
        pass "p25k_g.sym's screen function table is the one its driver loads"
    else fail "p25k_g.sym's \$EEAB is not the screen driver's table"; fi
    if grep -q 'ld a,(CAPS.MASK)' "$TMP/sym_g2.log" &&
       grep -q 'and b' "$TMP/sym_g2.log"; then
        pass "p25k_g.sym's capitals mask is ANDed, as that BIOS does it"
    else fail "p25k_g.sym's \$E3C3 is not CONIN's mask"; fi

    # The two BIOS generations hold capitals lock differently, and it is
    # visible from the prompt: P25K_B XORs $20, so typing DIR echoes dir,
    # while P25K_G ANDs $DF, which clamps to upper case. Asserted because
    # it is the one difference between them a user actually meets.
    $EMU --disk "$DISK" --max-steps 40000000 --push-at '6000:DIR\r' \
         --dump-vram "$TMP/caps_b.bin" >"$TMP/caps_b.log" 2>&1
    $EMU --disk disks/P25K_G.raw --max-steps 40000000 --push-at '6000:DIR\r' \
         --dump-vram "$TMP/caps_g.bin" >"$TMP/caps_g.log" 2>&1
    screen "$TMP/caps_b.bin" >"$TMP/caps_b.screen"
    screen "$TMP/caps_g.bin" >"$TMP/caps_g.screen"
    if grep -q 'A>dir' "$TMP/caps_b.screen"; then
        pass "P25K_B's capitals lock swaps case: DIR echoes dir"
    else fail "P25K_B did not swap the case of DIR"; fi
    if grep -q 'A>DIR' "$TMP/caps_g.screen"; then
        pass "P25K_G's capitals lock clamps: DIR stays DIR"
    else fail "P25K_G did not leave DIR alone"; fi
fi

# --symbols auto identifies the build from its own bytes. The claim is
# that it follows the machine rather than the command line: the same
# invocation must name the IPL early in the boot and the right CBIOS once
# that CBIOS is resident, and must pick a different file for P25K_G.
$EMU --symbols auto --max-steps 2000 >"$TMP/auto_ipl.log" 2>&1
if grep -q 'from symbols/ipl.sym' "$TMP/auto_ipl.log"; then
    pass "--symbols auto recognises the IPL with no disk attached"
else fail "--symbols auto did not match symbols/ipl.sym at reset"; fi

if [ -f "$DISK" ]; then
    $EMU --disk "$DISK" --symbols auto --max-steps 3000000 \
         >"$TMP/auto_b.log" 2>&1
    if grep -q 'from symbols/cpm.sym' "$TMP/auto_b.log"; then
        pass "--symbols auto recognises P25K_B's CBIOS once it is resident"
    else fail "--symbols auto did not match symbols/cpm.sym"; fi
    # The IPL's names are stale by then - its fingerprint is the EPROM seen
    # through $0000, which CBIOS has banked away - so they must NOT load.
    if grep -q 'from symbols/ipl.sym' "$TMP/auto_b.log"; then
        fail "ipl.sym still matched after CBIOS banked the EPROM out"
    else pass "the IPL's fingerprint expires when its names stop being true"; fi
fi
if [ -f disks/P25K_G.raw ]; then
    $EMU --disk disks/P25K_G.raw --symbols auto --max-steps 3000000 \
         >"$TMP/auto_g.log" 2>&1
    if grep -q 'from symbols/p25k_g.sym' "$TMP/auto_g.log"; then
        pass "--symbols auto tells the two BIOS generations apart"
    else fail "--symbols auto did not match symbols/p25k_g.sym on P25K_G"; fi
fi
# And the GUI follows the machine as it boots, without being asked.
if [ -x ./p2500-gui ] && [ -f "$DISK" ]; then
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 400 \
        >"$TMP/auto_gui.log" 2>&1
    if grep -q 'symbols: .* symbols/ipl.sym' "$TMP/auto_gui.log" &&
       grep -q 'symbols: .* symbols/cpm.sym' "$TMP/auto_gui.log"; then
        pass "the GUI re-identifies the build as the machine boots"
    else fail "the GUI did not follow the build change during boot"; fi
fi
}

sec_17() {
echo "== 17. Four drives, and --disks"
# Four is the hardware's number: PIO port A bits 4-7 are one select line
# per drive (T14). How many a guest BIOS maps is its own business.
if [ ! -f disks/P2500GAM.raw ]; then
    echo "  SKIP  disks/P2500GAM.raw not present"
else
    $EMU --disks "$DISK,,,disks/P2500GAM.raw" --max-steps 1000 \
         >"$TMP/disks.log" 2>&1
    if grep -q 'Loaded A: from' "$TMP/disks.log" &&
       grep -q 'Loaded D: from' "$TMP/disks.log"; then
        pass "--disks fills drives in order and an empty field skips one"
    else fail "--disks did not place the images in A: and D:"; fi
    # The fourth slot is the hardware's fourth select line, NOT a claim
    # that anything maps it: P25K_B's CBIOS maps three drive letters and
    # P25K_G's maps two. Both halves asserted, because the interesting
    # failure would be a drive letter that silently reads the wrong unit.
    $EMU --disks "$DISK,,disks/P2500GAM.raw" --max-steps 60000000 \
         --push-at '4000:dir c:\r' --dump-vram "$TMP/c.bin" \
         >"$TMP/dirc.log" 2>&1
    screen "$TMP/c.bin" >"$TMP/dirc.screen"
    if grep -q 'BLACKJAK' "$TMP/dirc.screen"; then
        pass "DIR C: reads the third drive"
    else fail "DIR C: did not read drive C"; fi
    $EMU --disks "$DISK,,,disks/P2500GAM.raw" --max-steps 60000000 \
         --push-at '4000:dir d:\r' --dump-vram "$TMP/d.bin" \
         >"$TMP/dird.log" 2>&1
    screen "$TMP/d.bin" >"$TMP/dird.screen"
    if grep -q 'Bdos Err On D: Select' "$TMP/dird.screen"; then
        pass "D: is attachable, and this CBIOS refuses it - it maps three"
    else fail "DIR D: did not give the Select error P25K_B's CBIOS owes"; fi
fi

# And a bad file is refused with the line that caused it, not ignored.
printf '# fine\nE200SELDSK\n' >"$TMP/broken.sym"
if $EMU --symbols "$TMP/broken.sym" --max-steps 1 >"$TMP/sym_bad.log" 2>&1; then
    fail "a malformed symbol file was accepted"
else
    if grep -q 'broken.sym:2:' "$TMP/sym_bad.log"; then
        pass "a malformed symbol file names the offending line"
    else fail "the symbol-file error did not name line 2"; fi
fi
}

sec_18() {
echo "== 18. The shipped symbol files against an independent derivation"
# The symbols/*.sym files are hand-curated. tests/derive_syms.py finds the
# same machine's structures from scratch - page zero, the CBIOS jump table,
# the IM2 trampolines, the device table and the DPH/DPB chain, all by shape
# - and where the two overlap they must not contradict each other. That
# agreement is the closest thing to a proof either set is right, and until
# now nobody was running it.
#
# A curated *semantic* name over this tool's purely structural one
# (SCREEN.WRITE for SCREEN.F05) is an elaboration, not a disagreement, and
# is reported as such; two different semantic names at one address, or one
# name sitting at two addresses, fails. Both arms were negative-tested by
# mutating a symbol file, which is why this is a check and not decoration.
if ! command -v python3 >/dev/null 2>&1; then
    echo "  SKIP  python3 not installed"
else
    # The three IM2 vectors come out of the machine rather than being baked
    # in here: given them, derive_syms reads the interrupt table instead of
    # searching for it, and the device behind each trampoline is certain.
    vectors() {
        sed -n 's/.*CTC ch2 *vec \$\([0-9A-F]*\).*/\1/p;
                s/.*PIO port A *vec \$\([0-9A-F]*\).*/\1/p;
                s/.*DMA *vec \$\([0-9A-F]*\).*/\1/p' "$1" |
        tr '\n' ':' | sed 's/:$//'
    }
    for pair in "P25K_B:symbols/cpm.sym" "P25K_G:symbols/p25k_g.sym"; do
        img="disks/$(echo "$pair" | cut -d: -f1).raw"
        sym=$(echo "$pair" | cut -d: -f2)
        name=$(basename "$img" .raw)
        if [ ! -f "$img" ]; then
            echo "  SKIP  $img not present"
            continue
        fi
        $EMU --disk "$img" --max-steps 3000000 --state \
             --dump-ram "$TMP/$name.ram" >"$TMP/$name.state" 2>&1
        vec=$(vectors "$TMP/$name.state")
        # No vectors means the boot did not get as far as installing them,
        # which would make everything below vacuous.
        case "$vec" in
            ??:??:??) ;;
            *) fail "$name: no IM2 vectors in --state, nothing to derive from"
               continue ;;
        esac
        if python3 tests/derive_syms.py "$TMP/$name.ram" --vectors "$vec" \
                   --quiet --check "$sym" >"$TMP/$name.derive" 2>&1; then
            pass "$sym agrees with an independent derivation of $name"
        else
            sed 's/^/        /' "$TMP/$name.derive"
            fail "$sym contradicts what $name says about itself"
        fi
        # The derivation must actually have found something: a tool that
        # derives nothing agrees with every symbol file ever written.
        if grep -q 'CBIOS jump table at \$E200' "$TMP/$name.derive" &&
           grep -q 'IM2 trampolines: 6 found' "$TMP/$name.derive" &&
           grep -qE '[0-9]+ address\(es\) in both' "$TMP/$name.derive" &&
           [ "$(sed -n 's/.*, \([0-9]*\) address(es) in both/\1/p' \
                "$TMP/$name.derive")" -ge 40 ]; then
            pass "the derivation of $name has something to compare (40+ addresses)"
        else fail "$name: the derivation found too little to be a real check"; fi
    done
    # The device table is where "how many drive letters does this BIOS map"
    # stops being a front-end guess: B's CBIOS publishes three DPHs and
    # G's two, which is exactly the Select error section 17 asserts.
    if [ -f "$TMP/P25K_B.derive" ] && [ -f "$TMP/P25K_G.derive" ]; then
        if grep -q 'disk structures: 3 DPH' "$TMP/P25K_B.derive" &&
           grep -q 'disk structures: 2 DPH' "$TMP/P25K_G.derive"; then
            pass "the DPH chains confirm three drive letters on B, two on G"
        else fail "the derived DPH counts no longer match the mapped drives"; fi
    fi
    # Negative control: the check must fail when a symbol file lies. Pick
    # an address both sides name and rename it; if that still passes, the
    # whole section is decoration.
    if [ -f "$TMP/P25K_B.ram" ]; then
        vec=$(vectors "$TMP/P25K_B.state")
        sed 's/^E84D .*/E84D SOMETHING.ELSE/' symbols/cpm.sym >"$TMP/lying.sym"
        if python3 tests/derive_syms.py "$TMP/P25K_B.ram" --vectors "$vec" \
                   --quiet --check "$TMP/lying.sym" >"$TMP/lying.log" 2>&1; then
            fail "the derivation check passed a symbol file that renames \$E84D"
        else
            pass "the check fails when a symbol file disputes a derived address"
        fi
    fi
fi
}

sec_19() {
echo "== 19. Identifying a disk image before it boots (# match-disk)"
# `# match` can only recognise a build once it is RESIDENT, which is
# useless for "what is this image?" before booting it. A `# match-disk`
# rule is a window of the file, so it answers at attach time - which is
# what a drive lamp's tooltip wants and what an unknown third-party image
# needs. The window has to be the CP/M directory: all four 320K images
# carry a byte-identical boot track, P25K_G's included.
$EMU --disks "$DISK,disks/P2500GAM.raw,disks/P25K_G.raw" --max-steps 100 \
     >"$TMP/ident.log" 2>&1
if grep -q 'A: is P25K_B .* (recognised by symbols/cpm.sym)' "$TMP/ident.log"; then
    pass "A: is identified from the image, before anything has executed"
else fail "the image in A: was not identified at attach time"; fi
# Two BIOS generations told apart by their disks rather than by their RAM,
# and from two different symbol files.
if grep -q 'C: is P25K_G .* (recognised by symbols/p25k_g.sym)' "$TMP/ident.log"; then
    pass "match-disk tells the two generations apart before either boots"
else fail "P25K_G was not identified from its own image"; fi
# Silence is the honest answer for an image nobody ships a rule for, and
# P2500GAM is that image - so the unrecognised path has a real test and
# not just an argument.
if grep -q 'Loaded B: from disks/P2500GAM.raw' "$TMP/ident.log" &&
   ! grep -q 'B: is ' "$TMP/ident.log"; then
    pass "an image with no rule is attached and claims nothing"
else fail "P2500GAM was either refused or given a name nobody shipped"; fi
# The GUI must agree with the CLI - same core call, and the suite should
# notice if one front-end grows its own idea of what is in the drive.
if [ -x ./p2500-gui ]; then
    SDL_VIDEODRIVER=dummy ./p2500-gui --disks "$DISK,disks/P25K_G.raw" --frames 20 \
        >"$TMP/ident_gui.log" 2>&1
    if grep -q 'attached A: .* - P25K_B' "$TMP/ident_gui.log" &&
       grep -q 'attached B: .* - P25K_G' "$TMP/ident_gui.log"; then
        pass "the GUI names both disks the same way the CLI does"
    else fail "the GUI did not identify the attached images"; fi
fi
# A disk rule must NOT be a vote on whether a file's symbols apply: a disk
# can sit in a drive for an hour without its BIOS ever being resident. So
# with P25K_G in the drive and nothing booted, --symbols auto must still
# decline to load p25k_g.sym's names.
$EMU --disk disks/P25K_G.raw --symbols auto --max-steps 2000 \
     >"$TMP/ident_vote.log" 2>&1
if grep -q 'from symbols/p25k_g.sym' "$TMP/ident_vote.log"; then
    fail "a match-disk rule loaded symbols for a BIOS that is not resident"
else pass "a recognised disk does not by itself load that build's symbols"; fi
# And the rules themselves must be rejected when malformed rather than
# passed over as comments - a fingerprint that silently never matches is
# the quiet refusal this project keeps paying for.
printf '# fine\n# match-disk 2000:ZZZZ\nE200 X\n' >"$TMP/badrule.sym"
if $EMU --symbols "$TMP/badrule.sym" --max-steps 1 >"$TMP/badrule.log" 2>&1; then
    fail "a malformed match-disk rule was silently ignored"
else
    if grep -q 'badrule.sym:2:' "$TMP/badrule.log"; then
        pass "a malformed match-disk rule names the offending line"
    else fail "the malformed rule error did not name line 2"; fi
fi
# ...while a comment that merely starts with those letters stays a comment.
printf '# matchless, and not a rule\nE200 X\n' >"$TMP/okcomment.sym"
if $EMU --symbols "$TMP/okcomment.sym" --max-steps 1 >/dev/null 2>&1; then
    pass "a comment beginning \"match\" is still a comment"
else fail "an ordinary comment was mistaken for a malformed rule"; fi
}

sec_20() {
echo "== 20. The system-information report (--sysinfo, and the GUI panel)"
# "What is this machine running" used to be spread across the log, the
# window title and three CLI flags. The report is built in core/sysinfo.c
# so both front-ends print the same lines - and so a headless run can
# contradict the panel, which is why the panel is allowed to exist.
#
# The rule it must obey: say what can be proved, in order of certainty,
# and stop where the proof stops. Each claim below is checked against
# something established elsewhere in this suite or in the research notes.
$EMU --disk "$DISK" --max-steps 3000000 --sysinfo >"$TMP/sys_b.log" 2>&1
# Always-provable: the EPROM is the dump every address in symbols/ipl.sym
# belongs to. The constant in sysinfo.c is falsifiable precisely here.
if grep -q 'boot EPROM .* CRC32 \$4CB66255 .* the dump this project' "$TMP/sys_b.log"; then
    pass "the boot EPROM is recognised as the dump the findings come from"
else fail "the EPROM CRC32 or its verdict changed"; fi
# The CRTC's own arithmetic, which is what T29 turns into a frame rate
# once the dot clock is known: 311 scanlines of 98 character times.
if grep -q 'CRTC frame .* 311 scanlines of 98 character times' "$TMP/sys_b.log"; then
    pass "the CRTC frame geometry is reported from the registers"
else fail "the CRTC frame geometry is not 311x98"; fi
# CP/M's map, against the addresses section 16 checks by disassembly.
if grep -q 'CCP base .*\$CC00' "$TMP/sys_b.log" &&
   grep -q 'BDOS .*base \$D400, entry \$D406' "$TMP/sys_b.log" &&
   grep -q 'CBIOS jump table .*\$E200   17 slots, all JP' "$TMP/sys_b.log"; then
    pass "the CP/M memory map agrees with the symbol files"
else fail "the reported CP/M memory map moved"; fi
# The largest loadable .COM was MEASURED by bisection at 405 records
# (406 gives BAD LOAD). The report must not quietly print the TPA size
# instead, which is one record larger.
if grep -q 'TPA .*51968 bytes (406 records)' "$TMP/sys_b.log" &&
   grep -q 'largest .COM .*405 records' "$TMP/sys_b.log"; then
    pass "the TPA and the largest loadable .COM are reported as different"
else fail "the TPA/load-limit distinction was lost"; fi
# Drive A's DPB, decoded. These are the numbers DSKPRAM.COM dumps on real
# hardware and that SuperCopy's shipped format table independently agrees
# with - three sources, so a change here is a real regression.
if grep -q 'drive A: DPB .*SPT 32, BSH 4, BLM 15, EXM 1, DSM 149, DRM 63, CKS 16, OFF 2' \
        "$TMP/sys_b.log" &&
   grep -q 'decoded .*2048-byte blocks, 64 directory entries, 2 reserved tracks, 300 KB' \
        "$TMP/sys_b.log"; then
    pass "drive A's DPB decodes to the geometry three sources agree on"
else fail "the decoded DPB no longer matches the known disk format"; fi
# And the headline claim: how many drive letters the RUNNING BIOS maps,
# read out of its own DPH chain rather than guessed from the hardware's
# four select lines. Section 17 asserts the consequence (D: gets Select);
# this asserts the machine says so about itself.
if grep -q 'DPH table .*3 drive letters (A:-C:)' "$TMP/sys_b.log"; then
    pass "P25K_B's BIOS reports the three drive letters it maps"
else fail "the DPH chain no longer reports three drive letters on B"; fi
if grep -q 'device table .*8 devices' "$TMP/sys_b.log" &&
   grep -q 'id \$30 .*keyboard' "$TMP/sys_b.log" &&
   grep -q 'id \$31 .*screen' "$TMP/sys_b.log"; then
    pass "the device table is found by shape and its known ids named"
else fail "the device table scan did not find B's eight devices"; fi

# The build size, derived from the CCP base at $400 per K (T59). This is
# the one line in the report that can be checked against a literal string
# in the firmware of a running system, so it is checked that way rather
# than against a constant: both BIOS generations print their own size on
# screen - "58K CP/M Ver. 2.2" on P25K_B, "CP/M 2.2 - 58K" on P25K_G - and
# the panel must agree with the banner on each. Two independent reads of
# one fact, neither of them this suite's own arithmetic.
for d in P25K_B P25K_G; do
    [ -f "disks/$d.raw" ] || continue
    $EMU --disk "disks/$d.raw" --max-steps 3000000 --sysinfo \
         --dump-vram "$TMP/bk_$d.bin" >"$TMP/bk_$d.log" 2>&1
    screen "$TMP/bk_$d.bin" | head -2 >"$TMP/bk_$d.screen"
    said=$(grep -oE '[0-9]+K' "$TMP/bk_$d.screen" | head -1)
    shown=$(sed -n 's/^  build  *CP\/M 2\.2 - \([0-9]*K\) .*/\1/p' "$TMP/bk_$d.log")
    # Both halves must be non-empty, or "they agree" would mean nothing.
    if [ -n "$said" ] && [ -n "$shown" ] && [ "$said" = "$shown" ]; then
        pass "$d: the reported build size ($shown) is the size its own banner prints"
    else fail "$d: banner says '${said:-nothing}', the report says '${shown:-nothing}'"; fi
done
# ...and the derivation is anchored, not a constant: $400 per K from $CC00.
if grep -q 'build  *CP/M 2\.2 - 58K   from the CCP base, \$400 per K' "$TMP/sys_b.log"; then
    pass "the build size names its own derivation from the CCP base"
else fail "the build size row is not derived from the CCP base"; fi

# The other BIOS generation must come out different in both numbers,
# which is what makes these reads and not constants.
if [ -f disks/P25K_G.raw ]; then
    $EMU --disk disks/P25K_G.raw --max-steps 3000000 --sysinfo \
         >"$TMP/sys_g.log" 2>&1
    if grep -q 'DPH table .*2 drive letters (A:-B:)' "$TMP/sys_g.log" &&
       grep -q 'device table .*6 devices' "$TMP/sys_g.log"; then
        pass "P25K_G reports two drive letters and six devices, not B's numbers"
    else fail "P25K_G's report did not differ from P25K_B's"; fi
fi

# The honest-silence rule, and the one that matters most: a machine that
# is NOT running CP/M must produce a SHORT report, not a wrong one. With
# no disk the IPL owns the machine and page zero is the ROM's vectors.
$EMU --max-steps 2000 --sysinfo >"$TMP/sys_ipl.log" 2>&1
if grep -q 'boot EPROM' "$TMP/sys_ipl.log" &&
   grep -q 'No CP/M page zero here' "$TMP/sys_ipl.log" &&
   ! grep -q 'CCP base' "$TMP/sys_ipl.log" &&
   ! grep -q 'DPH table' "$TMP/sys_ipl.log" &&
   ! grep -q 'device table' "$TMP/sys_ipl.log"; then
    pass "a non-CP/M machine gets the Machine section and nothing it cannot prove"
else fail "the report claimed CP/M structures on a machine with no CP/M"; fi
# ...and it must still have said the provable things, or "sparse" would
# just mean "broken".
if grep -q 'fingerprint .*symbols/ipl.sym' "$TMP/sys_ipl.log" &&
   grep -q 'bank latch (port \$05) .*\$0000-\$0FFF EPROM' "$TMP/sys_ipl.log"; then
    pass "the sparse report still proves what it can (ROM banked in, IPL named)"
else fail "the sparse report dropped the facts it does have"; fi

# The panel and the flag must be the same call. The GUI draws the report
# from p2500_sysinfo() exactly as --sysinfo prints it, so the suite checks
# the panel opens and renders rather than pinning pixels.
if [ -x ./p2500-gui ]; then
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --panels system \
        --frames 400 >"$TMP/sys_gui.log" 2>&1
    if [ $? -eq 0 ]; then
        pass "the GUI opens the System information panel headlessly"
    else fail "p2500-gui failed with the system panel open"; fi
fi
}

sec_21() {
echo "== 21. Four drive lamps, and the one the running BIOS will not address"
# The hardware has four drive-select lines (PIO port A bits 4-7, T14), so
# there are four lamps. How many a guest BIOS maps is a different number:
# P25K_B's CBIOS maps three drive letters, so a disk in D: is attached to a
# drive the running CP/M refuses (section 17 asserts the Select error).
# The lamp says so by being mid-bright - attached, but inert.
#
# Three distinct brightnesses, probed at the columns the front-end reports
# rather than at hardcoded pixels. The probe asserts its own discriminating
# power: the inequality is strict, so it cannot pass by finding every lamp
# the same colour.
if [ ! -x ./p2500-gui ]; then
    echo "  SKIP  p2500-gui not built"
else
    # A: mapped+loaded, B: mapped+loaded, C: empty, D: loaded but unmapped.
    SDL_VIDEODRIVER=dummy ./p2500-gui \
        --disks "$DISK,disks/P2500GAM.raw,,disks/P25K_G.raw" \
        --frames 2500 --shot-window "$TMP/lamps.ppm" >"$TMP/lamps.log" 2>&1
    LAMPS=$(sed -n 's/.*lamps: .*drives \([0-9]*\) \([0-9]*\) \([0-9]*\) \([0-9]*\) rows \([0-9]*\)-\([0-9]*\).*/\1 \2 \3 \4 \5 \6/p' \
            "$TMP/lamps.log" | head -1)
    if [ -z "$LAMPS" ]; then
        fail "the bar did not report four drive-lamp columns"
    else
        pass "the menu bar reports four drive-lamp columns"
        # shellcheck disable=SC2086
        set -- $LAMPS
        python3 - "$TMP/lamps.ppm" "$1" "$2" "$3" "$4" "$5" "$6" <<'LAMPPROBE' && pass "the four drive lamps read mapped / unmapped / empty" || fail "the drive lamps did not separate the three states"
import sys
def load(p):
    d = open(p, 'rb').read()
    hdr = d.split(b'\n', 3)
    w, h = (int(v) for v in hdr[1].split())
    return w, h, d[len(b'\n'.join(hdr[:3])) + 1:]
w, h, px = load(sys.argv[1])
cols = [int(v) for v in sys.argv[2:6]]
y0, y1 = (int(v) for v in sys.argv[6:8])
def peak(cx):
    return max(px[((y * w) + x) * 3 + 1]
               for y in range(y0, y1) for x in range(cx - 5, cx + 5))
a, b, c, d = (peak(x) for x in cols)
print("    A=%d B=%d C=%d D=%d" % (a, b, c, d))
# A and B hold media this BIOS maps; D holds media it will not address; C
# is empty. Strict in both places, so "all the same" fails.
if not (a == b and a > d > c):
    print("    expected A == B > D > C (mapped > unmapped > empty)")
    sys.exit(1)
sys.exit(0)
LAMPPROBE
    fi
fi
}

sec_22() {
echo "== 22. The cartridge slot in the system-information report"
# The slot is generic - SESAM dongle, the "Maint" diagnostic plug - so the
# report describes the stream rather than naming a product. Byte 0 is the
# ROM's own presence check, and the read/write counts are the only evidence
# a protection check leaves (T51).
$EMU --max-steps 2000 --sysinfo >"$TMP/cart_empty.log" 2>&1
if grep -q 'slot .*empty - every read returns \$FF' "$TMP/cart_empty.log"; then
    pass "an empty slot is reported as empty"
else fail "the empty cartridge slot was not reported"; fi
# A three-byte stream starting $00 is the bootable-cartridge shape, and the
# IPL must actually take that branch - which is what makes byte 0's verdict
# a claim rather than a label.
printf '\000\022\064' >"$TMP/key.bin"
$EMU --cartridge "$TMP/key.bin" --max-steps 300000 --sysinfo \
     >"$TMP/cart_key.log" 2>&1
if grep -q 'byte 0 .*\$00   the IPL will boot from this cartridge' "$TMP/cart_key.log" &&
   grep -q 'first three bytes .*\$00 \$12 \$34   likely a SESAM key' "$TMP/cart_key.log"; then
    pass "a three-byte stream is reported as a bootable SESAM-shaped key"
else fail "the cartridge stream was not decoded"; fi
# A non-$00 first byte must send the IPL to the floppy instead, and the
# report must say so rather than printing one line whatever the byte is.
printf '\377\022\064' >"$TMP/notboot.bin"
$EMU --cartridge "$TMP/notboot.bin" --max-steps 2000 --sysinfo \
     >"$TMP/cart_nb.log" 2>&1
if grep -q 'byte 0 .*\$FF   not a bootable cartridge' "$TMP/cart_nb.log"; then
    pass "a non-\$00 first byte is reported as not bootable"
else fail "byte 0's verdict is the same whatever the byte is"; fi
# The traffic counters must move, or they are decoration - and the IPL's
# own figure is a documented constant rather than a shrug: left alone with
# an empty slot it does exactly 32 reads and 6 writes, which is the
# baseline the report quotes and the number any excess is measured
# against. Asserted exactly, so a change in the IPL's probing shows up
# here rather than silently shifting what the report means.
$EMU --max-steps 3000000 --sysinfo >"$TMP/cart_base.log" 2>&1
if grep -q 'traffic .*32 reads, 6 writes' "$TMP/cart_base.log"; then
    pass "an untouched slot sees exactly the IPL's 32 reads and 6 writes"
else fail "the IPL's cartridge baseline is no longer 32 reads / 6 writes"; fi
python3 - "$TMP/cart_base.log" "$TMP/cart_key.log" <<'CARTTRAFFIC' && pass "a cartridge the IPL boots from is read far past the baseline" || fail "the cartridge traffic counters did not move"
import re, sys
def reads(p):
    m = re.search(r'traffic\s+(\d+) reads', open(p).read())
    return int(m.group(1)) if m else -1
base, key = reads(sys.argv[1]), reads(sys.argv[2])
print("    baseline %d reads, bootable cartridge %d" % (base, key))
sys.exit(0 if base == 32 and key > base * 10 else 1)
CARTTRAFFIC
}

sec_23() {
echo "== 23. Writing to disk: CP/M's own SAVE and PIP, then read back (T30)"
# The end-to-end claim: a guest program writes a file, the image carries it,
# and a SECOND boot of the saved image finds it. Reading it back on a fresh
# machine is what makes this a test of the data path rather than of the
# directory buffer that is still in RAM.
#
# $DISK is never written: --save-disk writes the in-memory image out to a
# path of our choosing, which is the whole persistence policy - nothing in
# this emulator can damage an original.
cp "$DISK" "$TMP/w.raw"
$EMU --disk "$TMP/w.raw" --max-steps 40000000 \
     --type-at '4000:SAVE 1 ZZ.COM\r' --type-at '20000:DIR\r' \
     --save-disk "$TMP/saved.raw" --dump-vram "$TMP/w.bin" \
     >"$TMP/w.log" 2>&1
screen "$TMP/w.bin" >"$TMP/w.screen"
if grep -q 'ZZ *COM' "$TMP/w.screen"; then
    pass "SAVE writes a file CP/M's own DIR then lists"
else fail "SAVE did not produce a directory entry"; fi
if grep -q 'Wrote A: .*1 sector' "$TMP/w.log" || grep -q 'Wrote A: .*[2-9] sector' "$TMP/w.log"; then
    pass "the FDC counted the sectors the guest wrote"
else fail "no sectors were counted as written"; fi
# The source image must be untouched whatever the guest did.
if cmp -s "$TMP/w.raw" "$DISK"; then
    pass "the file the disk was loaded from is byte-identical afterwards"
else fail "the source image was modified"; fi
if [ -s "$TMP/saved.raw" ] && ! cmp -s "$TMP/saved.raw" "$DISK"; then
    pass "--save-disk wrote an image that differs from the original"
else fail "--save-disk produced nothing, or nothing changed"; fi

# Boot the saved image. This is the discriminating half: a write that only
# updated the in-RAM directory would pass everything above and fail here.
$EMU --disk "$TMP/saved.raw" --max-steps 20000000 --type-at '4000:DIR\r' \
     --dump-vram "$TMP/r.bin" >"$TMP/r.log" 2>&1
screen "$TMP/r.bin" >"$TMP/r.screen"
if grep -q '58K CP/M Ver. 2.2' "$TMP/r.screen"; then
    pass "the written-to image still boots"
else fail "the written-to image no longer boots"; fi
if grep -q 'ZZ *COM' "$TMP/r.screen"; then
    pass "a fresh boot of it finds the file"
else fail "the file did not survive the save/reload"; fi

# PIP: a real utility, copying a real file. Comparing the copy against its
# source byte for byte proves the data blocks landed where the directory
# says they did - the directory alone parses either way.
cp "$DISK" "$TMP/p.raw"
$EMU --disk "$TMP/p.raw" --max-steps 120000000 \
     --type-at '4000:PIP COPY.COM=SYSGEN.COM\r' \
     --save-disk "$TMP/pip.raw" >"$TMP/pip.log" 2>&1
if python3 tools/cpm_extract.py "$TMP/pip.raw" "$TMP/pipex" --quiet >"$TMP/pipex.log" 2>&1 &&
   cmp -s "$TMP/pipex/COPY.COM" "$TMP/pipex/SYSGEN.COM"; then
    pass "PIP's copy is byte-identical to its source on the saved image"
else
    fail "PIP's copy is missing or does not match its source"
    sed 's/^/    /' "$TMP/pipex.log"
fi

# The write-protect tab. CBIOS does not consult ST3's WP bit - it issues the
# write anyway and reports the failure through BDOS - so what this asserts
# is that the refusal reaches the guest at all, and that the image really is
# untouched. A model that ignored the tab would pass neither.
cp "$DISK" "$TMP/wp.raw"
$EMU --disk "$TMP/wp.raw" --write-protect a --max-steps 40000000 \
     --type-at '4000:SAVE 1 ZZ.COM\r' --verbose-io \
     --save-disk "$TMP/wpsaved.raw" --dump-vram "$TMP/wp.bin" \
     >"$TMP/wp.log" 2>&1
screen "$TMP/wp.bin" >"$TMP/wp.screen"
if grep -q 'media is write-protected' "$TMP/wp.log"; then
    pass "a write to write-protected media is refused by the controller"
else fail "the write-protect tab was ignored"; fi
if grep -qi 'Bdos Err On A' "$TMP/wp.screen"; then
    pass "and the guest is told, through BDOS"
else fail "the guest saw no error"; fi
if cmp -s "$TMP/wpsaved.raw" "$DISK"; then
    pass "the write-protected image is byte-identical to the original"
else fail "a write-protected image was modified"; fi

# The GUI's half of the policy: the only notice a user gets that an image
# has been written to is the count beside File > Save Disk As... That is a
# claim about pixels, so it is checked in pixels - the same submenu, with
# and without a guest that wrote, and the two must differ. Checking only
# the "after" capture would pass against a menu that always said something.
if [ -x ./p2500-gui ]; then
    # Spelled out twice rather than looped: the push argument carries
    # spaces, and a variable holding it would word-split back into four
    # arguments. The mouse path is click File (the lamp report gives no
    # menu coordinates), hover "Save Disk As...", then step right into the
    # submenu - so adding a File item above it moves the target and this
    # fails loudly (nothing differs) rather than passing vacuously.
    cp "$DISK" "$TMP/g.raw"
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$TMP/g.raw" --speed unlimited \
        --frames 900 --push-at '4000:SAVE 1 ZZ.COM\r' \
        --mouse '800:20,11,left' --mouse '820:60,119' --mouse '860:100,119' \
        --shot-window "$TMP/menu-wrote.ppm" >"$TMP/menu-wrote.log" 2>&1
    cp "$DISK" "$TMP/g.raw"   # the written image must not seed the next run
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$TMP/g.raw" --speed unlimited \
        --frames 900 \
        --mouse '800:20,11,left' --mouse '820:60,119' --mouse '860:100,119' \
        --shot-window "$TMP/menu-clean.ppm" >"$TMP/menu-clean.log" 2>&1
    python3 - "$TMP/menu-wrote.ppm" "$TMP/menu-clean.ppm" <<'MENUCOUNT' && pass "the File menu shows a written-sector count, and only when there is one" || fail "the Save Disk As... submenu does not report writes"
import sys
def load(p):
    f = open(p, 'rb')
    assert f.readline().strip() == b'P6'
    w, h = map(int, f.readline().split()); f.readline()
    return w, h, f.read()
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
# The submenu sits to the right of the File dropdown, below the bar. The
# count is the only thing in it that a write changes.
diff = sum(a[(y*w+x)*3:(y*w+x)*3+3] != b[(y*w+x)*3:(y*w+x)*3+3]
           for y in range(100, 200) for x in range(210, 560))
print("    %d pixel(s) differ in the submenu" % diff)
sys.exit(0 if diff > 50 else 1)
MENUCOUNT
else
    echo "  (skipped: no ./p2500-gui)"
fi

# tests/fdc_write links libp2500.a directly, for the paths no guest program
# in this corpus takes: FORMAT TRACK, a write that outlives its track, a DMA
# programmed the wrong way round, and the write-protect refusal in
# isolation. Its lines are replayed through pass/fail so they count here
# rather than merely appearing - same arrangement as section 14, and read
# from files because a `while read` off a pipe counts in a subshell.
if [ -x ./tests/fdc_write ]; then
    ./tests/fdc_write >"$TMP/fdc-write.log" 2>&1
    wrc=$?
    sed -n 's/^  PASS  //p' "$TMP/fdc-write.log" >"$TMP/fdc-write.pass"
    sed -n 's/^  FAIL  //p' "$TMP/fdc-write.log" >"$TMP/fdc-write.fail"
    while IFS= read -r c; do pass "$c"; done <"$TMP/fdc-write.pass"
    while IFS= read -r c; do fail "$c"; done <"$TMP/fdc-write.fail"
    if [ ! -s "$TMP/fdc-write.pass" ] && [ ! -s "$TMP/fdc-write.fail" ]; then
        fail "tests/fdc_write named no checks at all"
    elif [ "$wrc" -ne 0 ] && [ ! -s "$TMP/fdc-write.fail" ]; then
        fail "tests/fdc_write exited $wrc without naming a failed check"
    fi
else
    fail "tests/fdc_write is not built (make tests/fdc_write)"
fi
}

sec_24() {
echo "== 24. The cartridge path end to end: a real P2490 probe reads the key back (T93)"
# Sections 1 and 22 drive port $0F from the IPL and from the report. What
# neither does is run a *guest program* that deliberately works the
# protocol - and the IPL's own baseline never issues the final OUT 0, so
# the power-down half of the sequence had no coverage at all.
#
# SESAMTST.BAS is the manufacturer's own BASIC probe for the P2490 module
# (SESAM-module-photos): OUT 15,1 / OUT 15,3 / three INP(15) / OUT 15,0,
# then it prints the three bytes as a serial number. So the test is a
# mirror: feed a known key in through --cartridge and require the guest to
# print those exact bytes back. Nothing here can recover anything - the
# point is that the day a real module's bytes turn up, nobody is debugging
# the emulator and the key at the same time.
#
# The fixture is the archive copy with CP/M line endings: the held file is
# LF-only, and MBASIC's ASCII loader needs CR LF or it reads the whole
# program as one line and stops with "Line buffer overflow in 140".
CARTBAS="tests/fixtures/sesamtst.bas"
if [ ! -f "$DISK" ] || [ ! -f disks/P25TEST.raw ] || [ ! -f "$CARTBAS" ]; then
    echo "  SKIP  needs $DISK, disks/P25TEST.raw and $CARTBAS"
else
    # MBASIC comes off P25TEST rather than being carried here twice.
    mkdir -p "$TMP/cartp"
    python3 tools/cpm_extract.py disks/P25TEST.raw "$TMP/cartp" --quiet \
        >"$TMP/cartp_extract.log" 2>&1
    if [ ! -f "$TMP/cartp/MBASIC.COM" ]; then
        fail "no MBASIC.COM on disks/P25TEST.raw to build the probe disk with"
    elif ! python3 tools/cpm_build.py "$TMP/sesamtst.raw" \
            "$TMP/cartp/MBASIC.COM" "$CARTBAS:SESAMTST.BAS" \
            --boot-from "$DISK" >"$TMP/cartp_build.log" 2>&1; then
        fail "could not build the SESAMTST probe disk"
        sed -n '$p' "$TMP/cartp_build.log"
    else
        # Three keys, one run each. Two real ones differing in every byte -
        # so a hard-coded or stale answer cannot pass both - and the empty
        # slot, which must take the program's other branch.
        printf '\001\030\061' >"$TMP/cart_1831.bin"   # $01 $18 $31
        printf '\001\017\007' >"$TMP/cart_0f07.bin"   # $01 $0F $07
        cartrun() { # NAME [CARTFILE]
            if [ -n "${2-}" ]; then set -- "$1" --cartridge "$2"; else set -- "$1"; fi
            n=$1; shift
            $EMU --disk "$TMP/sesamtst.raw" "$@" --max-steps 20000000 \
                 --speed unlimited --type-at '4000:MBASIC SESAMTST\r' \
                 --sysinfo --dump-vram "$TMP/cart_$n.vram" \
                 >"$TMP/cart_$n.log" 2>&1
            screen "$TMP/cart_$n.vram" | tr -s ' ' >"$TMP/cart_$n.screen"
        }
        cartrun 1831 "$TMP/cart_1831.bin"
        cartrun 0f07 "$TMP/cart_0f07.bin"
        cartrun none

        # The program must have run to completion in all three, or a blank
        # screen would read as whatever the grep below wants it to.
        for n in 1831 0f07 none; do
            if grep -q '\*\*\* END \*\*\*' "$TMP/cart_$n.screen"; then
                pass "SESAMTST ran to completion ($n)"
            else fail "SESAMTST did not finish ($n) - MBASIC or the build is broken"; fi
        done
        # $18/$31 and $0F/$07 also cover both sides of the program's own
        # zero-padding: HEX$ gives one digit for a byte under $10.
        if grep -q 'SESAM P2490 NR.: 1831' "$TMP/cart_1831.screen"; then
            pass "the guest reads \$01 \$18 \$31 back as NR.: 1831"
        else fail "the key did not come back through INP(15)"; fi
        if grep -q 'SESAM P2490 NR.: 0F07' "$TMP/cart_0f07.screen"; then
            pass "a second key reads back as NR.: 0F07, so the answer is the stream"
        else fail "the second key did not come back - a stale or fixed answer"; fi
        if grep -q 'NO SESAM !!!' "$TMP/cart_none.screen" &&
           ! grep -q 'SESAM P2490' "$TMP/cart_none.screen"; then
            pass "an empty slot reads \$FF and the guest says NO SESAM"
        else fail "the empty slot did not take the program's other branch"; fi

        # Traffic, which is the part the counters are for. Booting this disk
        # costs 35 reads / 9 writes - the IPL's 32/6 plus the CBIOS's own
        # three-byte gate - and the program must add exactly its own 3 and 3
        # on top. Asserted exactly: an extra read is a double-fetch bug and a
        # missing write is the power-down half going unissued.
        python3 - "$TMP/cart_1831.log" "$TMP/cart_0f07.log" "$TMP/cart_none.log" <<'CARTPATH' && pass "each run adds exactly SESAMTST's 3 reads and 3 writes to the boot's 35/9" || fail "the \$0F traffic is not boot 35/9 + the program's 3/3"
import re, sys
ok = True
for p in sys.argv[1:]:
    m = re.search(r'traffic\s+(\d+) reads, (\d+) writes', open(p).read())
    if not m:
        print("    %s: no traffic line" % p); ok = False; continue
    r, w = int(m.group(1)), int(m.group(2))
    print("    %s: %d reads, %d writes" % (p.split('/')[-1], r, w))
    if (r, w) != (38, 12): ok = False
sys.exit(0 if ok else 1)
CARTPATH
        # And the sequence itself, in order, from the device's own trace:
        # power on, reset the address counter, three bytes, power off.
        $EMU --disk "$TMP/sesamtst.raw" --cartridge "$TMP/cart_1831.bin" \
             --max-steps 20000000 --speed unlimited --verbose-io \
             --type-at '4000:MBASIC SESAMTST\r' >"$TMP/cart_trace.log" 2>&1
        # The last six accesses, reduced to direction and byte, so the
        # comparison is against the sequence and not against log wording.
        grep '\[cartridge\]' "$TMP/cart_trace.log" | tail -6 |
            sed -e 's/.*OUT (\$0F) <- \(\$..\).*/OUT \1/' \
                -e 's/.*IN (\$0F) -> \(\$..\).*/IN \1/' >"$TMP/cart_tail"
        printf 'OUT $01\nOUT $03\nIN $01\nIN $18\nIN $31\nOUT $00\n' >"$TMP/cart_want"
        if cmp -s "$TMP/cart_tail" "$TMP/cart_want"; then
            pass "the last sequence on the port is on/reset/3 bytes/off, in that order"
        else
            fail "SESAMTST's \$0F sequence is not on/reset/3 bytes/off"
            diff "$TMP/cart_want" "$TMP/cart_tail"
        fi
    fi
fi
}

sec_25() {
echo "== 25. The configuration syntax: the subset, and what it preserves (T97)"
# tests/ini.c links libp2500.a directly, for the same reason tests/fdc_eot.c
# does: the claim is about bytes in a file, not about anything the machine
# can be driven to do. It is the parser three things share - the preferences
# file here, and the named profiles and media metadata still to come - so
# what it refuses and what it keeps are both worth pinning now rather than
# after two more consumers depend on them.
#
# Its output is one line per claim, replayed through pass/fail so those
# claims count towards this script's total instead of merely appearing in it.
if [ -x ./tests/ini ]; then
    ./tests/ini >"$TMP/ini.log" 2>&1
    rc=$?
    sed -n 's/^  PASS  //p' "$TMP/ini.log" >"$TMP/ini.pass"
    sed -n 's/^  FAIL  //p' "$TMP/ini.log" >"$TMP/ini.fail"
    while IFS= read -r c; do pass "$c"; done <"$TMP/ini.pass"
    while IFS= read -r c; do fail "$c"; done <"$TMP/ini.fail"
    if [ ! -s "$TMP/ini.pass" ] && [ ! -s "$TMP/ini.fail" ]; then
        fail "tests/ini named no checks at all"
    elif [ "$rc" -ne 0 ] && [ ! -s "$TMP/ini.fail" ]; then
        fail "tests/ini exited $rc without naming a failed check"
    fi
else
    fail "tests/ini is not built (make tests/ini)"
fi
}

sec_26() {
echo "== 26. The preferences file: precedence, isolation, and writing on change (T97)"
# Four layers decide every presentation setting - defaults, the file,
# P2500_DISPLAY, the flags - and the front-end's own `settings:` line names
# the value AND the layer it came from, which is what makes the precedence
# testable rather than merely documented.
if [ ! -x ./p2500-gui ]; then
    echo "  (skipped: no ./p2500-gui)"
    return
fi
CFG="$TMP/cfg/settings.ini"
# Every run below names its own config file. None of them may touch the
# developer's: that is what the first check is about.
gset() { sed -n "s/^settings: .*$1 \([^ ]*\) (\([a-z]*\)).*/\1 \2/p" "$2" | head -1; }

# --- the isolation, which is the one that must not be a happy path -------
# A POISON file: every value set to the opposite of its default, with
# P2500_CONFIG=none. If the override leaks, every one of these comes back
# instead of the defaults, and the whole suite would quietly be running on
# whatever a developer last clicked.
mkdir -p "$TMP/cfg"
cat >"$TMP/poison.ini" <<'POISON'
[display]
aspect = "square"
snap = true
phosphor = "white"
theme = "paper"

[machine]
speed = 0.25
caps_lock = false

[panels]
system = true
devices = true
memory = true
disasm = true
log = true

[window]
width = 900
height = 650
POISON
P2500_CONFIG=none SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY ./p2500-gui \
    --frames 3 >"$TMP/poison.log" 2>&1
if grep -q '^config: off (P2500_CONFIG=none)' "$TMP/poison.log"; then
    pass "P2500_CONFIG=none reports the file as off"
else fail "P2500_CONFIG=none did not report the file as off"; fi
if grep -q '^settings: aspect 4:3 (default), snap off (default), phosphor green (default), theme dark (default), speed 1x (default), caps-lock on (default), panels - (default), window 1280x984 (default)$' "$TMP/poison.log"; then
    pass "and a file setting every value wrong changes nothing: all eight are defaults"
else
    fail "P2500_CONFIG=none did not hold every setting at its default"
    grep '^settings:' "$TMP/poison.log"
fi
# The poison file must be POISON - i.e. it has to be capable of changing
# things, or the check above proves nothing at all.
P2500_CONFIG="$TMP/poison.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY ./p2500-gui \
    --frames 3 >"$TMP/poison-on.log" 2>&1
if grep -q '^settings: aspect square (file), snap on (file), phosphor white (file), theme paper (file), speed 0.25x (file), caps-lock off (file), panels system,devices,memory,disasm,log (file), window 900x650 (file)$' "$TMP/poison-on.log"; then
    pass "...and that same file DOES change all eight when it is not suppressed"
else
    fail "the poison file changed nothing, so the isolation check proves nothing"
    grep '^settings:' "$TMP/poison-on.log"
fi

# --- precedence: defaults < file < P2500_DISPLAY < flags -----------------
cat >"$CFG" <<'PREFS'
[display]
aspect = "square"
theme = "paper"
PREFS
P2500_CONFIG="$CFG" P2500_DISPLAY=aspect=4:3 SDL_VIDEODRIVER=dummy ./p2500-gui \
    --frames 3 --theme dark >"$TMP/prec.log" 2>&1
if [ "$(gset aspect "$TMP/prec.log")" = "4:3 env" ]; then
    pass "P2500_DISPLAY beats the file"
else fail "P2500_DISPLAY did not beat the file (got '$(gset aspect "$TMP/prec.log")')"; fi
if [ "$(gset theme "$TMP/prec.log")" = "dark flag" ]; then
    pass "a flag beats the file"
else fail "a flag did not beat the file (got '$(gset theme "$TMP/prec.log")')"; fi
if [ "$(gset snap "$TMP/prec.log")" = "off default" ]; then
    pass "and a setting nobody mentioned stays at its default"
else fail "an unmentioned setting did not stay at its default"; fi
# A flag must beat P2500_DISPLAY too - the two top layers, which no single
# run above distinguishes.
P2500_CONFIG=none P2500_DISPLAY=aspect=square SDL_VIDEODRIVER=dummy ./p2500-gui \
    --frames 3 --aspect 4:3 >"$TMP/prec2.log" 2>&1
if [ "$(gset aspect "$TMP/prec2.log")" = "4:3 flag" ]; then
    pass "a flag beats P2500_DISPLAY"
else fail "a flag did not beat P2500_DISPLAY"; fi

# --- surviving a hand-edit ----------------------------------------------
# The property the profiles and the media metadata will depend on: a file
# someone edited by hand, carrying a section this build has never heard of,
# must come back out of a save with everything it arrived with.
cat >"$TMP/hand.ini" <<'HAND'
# My settings. Do not reformat.
[display]
phosphor  = "white"    # easier on my eyes

[from-a-later-version]
cards = ["fdc2", "sio"]
HAND
cp "$TMP/hand.ini" "$TMP/hand-before.ini"
gui_report
L_SPEED=$(sed -n 's/.*lamps: .*speed \([0-9]*\).*/\1/p' "$TMP/gui_report.log" | head -1)
if [ -z "$L_SPEED" ]; then
    fail "the front-end did not report the speed cell's column"
else
    # Clicking the speed cell changes a setting, which is what triggers a
    # write - no separate "save" action exists, by design.
    P2500_CONFIG="$TMP/hand.ini" SDL_VIDEODRIVER=dummy ./p2500-gui \
        --disk "$DISK" --frames 30 --mouse "$L_SPEED,11,left" \
        >"$TMP/hand.log" 2>&1
    if grep -q 'speed = "unlimited"' "$TMP/hand.ini"; then
        pass "changing a setting writes the file, with no save action"
    else fail "changing a setting did not write the file"; fi
    if grep -q '^# My settings. Do not reformat.$' "$TMP/hand.ini" &&
       grep -q '^phosphor  = "white"    # easier on my eyes$' "$TMP/hand.ini"; then
        pass "a hand-edited line this version did not change is byte-identical afterwards"
    else
        fail "a hand-edited line was reformatted or lost"
        diff "$TMP/hand-before.ini" "$TMP/hand.ini"
    fi
    if grep -q '^\[from-a-later-version\]$' "$TMP/hand.ini" &&
       grep -q '^cards = \["fdc2", "sio"\]$' "$TMP/hand.ini"; then
        pass "and a section this version does not understand survives the write"
    else fail "an unknown section was dropped on write"; fi
    # And the file it wrote must load back as itself.
    P2500_CONFIG="$TMP/hand.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY \
        ./p2500-gui --frames 3 >"$TMP/hand2.log" 2>&1
    if [ "$(gset phosphor "$TMP/hand2.log")" = "white file" ] &&
       [ "$(gset speed "$TMP/hand2.log")" = "unlimited file" ]; then
        pass "the written file loads back as itself"
    else
        fail "the written file did not load back"
        grep '^settings:' "$TMP/hand2.log"
    fi
fi

# --- a broken file must not stop the emulator ---------------------------
# A settings file is hand-editable, so it WILL be mis-typed. One bad line
# costs that line; the rest of the file still loads and the machine starts.
cat >"$TMP/bad.ini" <<'BAD'
[display]
phosphor = white
theme = "paper"
BAD
P2500_CONFIG="$TMP/bad.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY ./p2500-gui \
    --frames 3 >"$TMP/bad.log" 2>&1
rc=$?
if [ "$rc" -eq 0 ] && [ "$(gset theme "$TMP/bad.log")" = "paper file" ]; then
    pass "a line that does not parse costs that line and nothing else"
else fail "a bad line in the settings file broke the run (exit $rc)"; fi
if grep -q 'not understood' "$TMP/bad.log"; then
    pass "and the file says so rather than failing silently"
else fail "a bad line was not reported"; fi

# --- refusing a file whose meaning changed ------------------------------
# `requires` is the half that "ignore unknown keys" cannot do: a file naming
# a feature this build has never heard of must be refused outright, not
# half-loaded - and must be left alone so a newer build still reads it.
cat >"$TMP/req.ini" <<'REQ'
requires = ["prefs1", "something-from-2027"]
[display]
theme = "paper"
REQ
cp "$TMP/req.ini" "$TMP/req-before.ini"
P2500_CONFIG="$TMP/req.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY ./p2500-gui \
    --frames 3 >"$TMP/req.log" 2>&1
if [ "$(gset theme "$TMP/req.log")" = "dark default" ]; then
    pass "a file requiring an unknown feature is refused, not half-loaded"
else fail "a file with an unmet requires was loaded anyway"; fi
if cmp -s "$TMP/req.ini" "$TMP/req-before.ini"; then
    pass "and is left untouched, so a newer build can still read it"
else fail "a refused settings file was overwritten"; fi
}

sec_27() {
echo "== 27. The remembered window geometry: bounds at both ends, and a settled resize (T100)"
# The window size is the first tier-one setting that is not a value picked
# from a menu, which gives it two properties nothing else in section 26 has:
# it is BOUNDED - a remembered geometry that reopens the emulator unusable
# is worse than one that forgets - and it is written only once a resize has
# stopped moving, because a drag produces an event per frame.
if [ ! -x ./p2500-gui ]; then
    echo "  (skipped: no ./p2500-gui)"
    return
fi
mkdir -p "$TMP/geo"
gwin() { sed -n 's/^settings: .*window \([0-9]*x[0-9]*\) (\([a-z]*\)).*/\1 \2/p' "$1" | head -1; }

# --- precedence: default < file < P2500_DISPLAY < flag -------------------
cat >"$TMP/geo/ok.ini" <<'GEO'
[window]
width = 1000
height = 700
GEO
P2500_CONFIG="$TMP/geo/ok.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY \
    ./p2500-gui --frames 3 >"$TMP/geo/file.log" 2>&1
if [ "$(gwin "$TMP/geo/file.log")" = "1000x700 file" ]; then
    pass "a remembered geometry is used"
else fail "a remembered geometry was not used (got '$(gwin "$TMP/geo/file.log")')"; fi
P2500_CONFIG="$TMP/geo/ok.ini" P2500_DISPLAY=window=900x650 SDL_VIDEODRIVER=dummy \
    ./p2500-gui --frames 3 >"$TMP/geo/env.log" 2>&1
if [ "$(gwin "$TMP/geo/env.log")" = "900x650 env" ]; then
    pass "P2500_DISPLAY window beats the file"
else fail "P2500_DISPLAY window did not beat the file (got '$(gwin "$TMP/geo/env.log")')"; fi
P2500_CONFIG="$TMP/geo/ok.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY \
    ./p2500-gui --frames 3 --window 800x600 >"$TMP/geo/flag.log" 2>&1
if [ "$(gwin "$TMP/geo/flag.log")" = "800x600 flag" ]; then
    pass "--window beats the file, and is honoured exactly"
else fail "--window did not beat the file (got '$(gwin "$TMP/geo/flag.log")')"; fi
# --scale is the other flag that sizes a window, and it has to win too -
# otherwise asking for a zoom would silently get last session's drag.
P2500_CONFIG="$TMP/geo/ok.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY \
    ./p2500-gui --frames 3 --scale 1 >"$TMP/geo/scale.log" 2>&1
if [ "$(gwin "$TMP/geo/scale.log")" = "640x504 flag" ]; then
    pass "an explicit --scale beats the file too"
else fail "--scale did not beat the file (got '$(gwin "$TMP/geo/scale.log")')"; fi

# --- the minimum ---------------------------------------------------------
# The file is hand-editable, so `width = 8` is one keystroke away from
# `width = 800`. A number under the floor costs the whole pair, because half
# a geometry is not a window size.
cat >"$TMP/geo/small.ini" <<'GEO'
[window]
width = 8
height = 700
GEO
P2500_CONFIG="$TMP/geo/small.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY \
    ./p2500-gui --frames 3 >"$TMP/geo/small.log" 2>&1
if [ "$(gwin "$TMP/geo/small.log")" = "1280x984 default" ]; then
    pass "a remembered size below the minimum is refused, not clamped into use"
else fail "a too-small geometry was used anyway (got '$(gwin "$TMP/geo/small.log")')"; fi
if grep -q 'below the 320x240 minimum' "$TMP/geo/small.log"; then
    pass "and says so rather than failing silently"
else fail "a too-small geometry was dropped with no diagnostic"; fi
cat >"$TMP/geo/half.ini" <<'GEO'
[window]
width = 900
GEO
P2500_CONFIG="$TMP/geo/half.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY \
    ./p2500-gui --frames 3 >"$TMP/geo/half.log" 2>&1
if [ "$(gwin "$TMP/geo/half.log")" = "1280x984 default" ] &&
   grep -q 'needs both width and height' "$TMP/geo/half.log"; then
    pass "a width with no height is not a geometry"
else fail "a half-written geometry was accepted"; fi
# The env layer must not be the way round the floor either.
P2500_CONFIG=none P2500_DISPLAY=window=8x700 SDL_VIDEODRIVER=dummy \
    ./p2500-gui --frames 3 >"$TMP/geo/envsmall.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && grep -q 'P2500_DISPLAY window' "$TMP/geo/envsmall.log"; then
    pass "and P2500_DISPLAY is held to the same floor"
else fail "P2500_DISPLAY window=8x700 was accepted (exit $rc)"; fi

# --- the maximum, which needs a driver that reports a display -----------
# The dummy driver has no usable bounds to clamp against, which is why the
# default sizing skips the clamp under it. `offscreen` does report a display
# (1024x768), so it is what makes the ceiling testable at all - without this
# the "moved to a smaller monitor" case has no check.
cat >"$TMP/geo/huge.ini" <<'GEO'
[window]
width = 9000
height = 9000
GEO
P2500_CONFIG="$TMP/geo/huge.ini" SDL_VIDEODRIVER=offscreen env -u P2500_DISPLAY \
    ./p2500-gui --frames 3 >"$TMP/geo/huge.log" 2>&1
if [ "$(gwin "$TMP/geo/huge.log")" = "1024x768 file" ]; then
    pass "a remembered size larger than the display is clamped to it"
else fail "an oversized geometry was not clamped (got '$(gwin "$TMP/geo/huge.log")')"; fi
if grep -q 'does not fit here' "$TMP/geo/huge.log"; then
    pass "and the clamp is reported"
else fail "an oversized geometry was clamped silently"; fi
# ...and the clamp must NOT reach --window, which the suite depends on being
# honoured exactly. Same driver, same display, a flag instead of a file.
P2500_CONFIG=none SDL_VIDEODRIVER=offscreen env -u P2500_DISPLAY \
    ./p2500-gui --frames 3 --window 4000x3000 >"$TMP/geo/exact.log" 2>&1
if [ "$(gwin "$TMP/geo/exact.log")" = "4000x3000 flag" ]; then
    pass "...while --window is still never clamped"
else fail "--window was clamped to the display (got '$(gwin "$TMP/geo/exact.log")')"; fi

# --- writing a resize, once it has settled ------------------------------
# --resize drives a real SDL_SetWindowSize, so what is under test is the
# SDL_EVENT_WINDOW_RESIZED path a dragged edge produces, not a back door
# into the setting.
: >"$TMP/geo/write.ini"
P2500_CONFIG="$TMP/geo/write.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY \
    ./p2500-gui --frames 120 --resize 5:1100x720 >"$TMP/geo/write.log" 2>&1
if grep -q '^width = 1100$' "$TMP/geo/write.ini" &&
   grep -q '^height = 720$' "$TMP/geo/write.ini"; then
    pass "a resize that settles is written to the file"
else
    fail "a settled resize was not written"
    cat "$TMP/geo/write.ini"
fi
P2500_CONFIG="$TMP/geo/write.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY \
    ./p2500-gui --frames 3 >"$TMP/geo/back.log" 2>&1
if [ "$(gwin "$TMP/geo/back.log")" = "1100x720 file" ]; then
    pass "and comes back on the next run"
else fail "the written geometry did not load back (got '$(gwin "$TMP/geo/back.log")')"; fi
# The negative half, without which the one above would pass just as well
# with no settle timer at all: a resize still in flight when the run ends is
# a gesture that was never finished, and is not remembered.
: >"$TMP/geo/unsettled.ini"
P2500_CONFIG="$TMP/geo/unsettled.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY \
    ./p2500-gui --frames 8 --resize 5:1100x720 >"$TMP/geo/unsettled.log" 2>&1
if ! grep -q '^\[window\]' "$TMP/geo/unsettled.ini"; then
    pass "a resize that never settles is not written"
else
    fail "an unsettled resize was written"
    cat "$TMP/geo/unsettled.ini"
fi
# And the floor holds against a drag as well as against the file: the window
# itself carries a minimum size, so a resize under it never happens.
: >"$TMP/geo/tiny.ini"
P2500_CONFIG="$TMP/geo/tiny.ini" SDL_VIDEODRIVER=dummy env -u P2500_DISPLAY \
    ./p2500-gui --frames 120 --resize 5:100x100 >"$TMP/geo/tiny.log" 2>&1
if grep -q '^width = 320$' "$TMP/geo/tiny.ini" &&
   grep -q '^height = 240$' "$TMP/geo/tiny.ini"; then
    pass "a resize below the minimum is held at it rather than remembered small"
else
    fail "a resize below the minimum was not held at the floor"
    cat "$TMP/geo/tiny.ini"
fi
}

sec_28() {
echo "== 28. Per-toolchain build segregation, and fatal format warnings"
# This section asks make what it WOULD do and never lets it do it: every
# call is `make -n` (dry run), which prints recipes without running them.
# That matters twice over - the suite must not rebuild the binaries it is
# in the middle of testing, and these checks must hold on a machine with no
# mingw-w64 toolchain, where the recipes exist but could not be run.
#
# Why it is worth a section at all: the native and MinGW compilers used to
# write the same object and libp2500.a paths, so `MINGW=1 make` after a
# native `make` linked ELF objects into a PE binary - and never rebuilt
# them, the stale objects being newer than their sources. The cross build
# therefore only ever ran inside publish.sh, behind three full cleans,
# which is why every Windows breakage so far was found at release time.
recipe() { make -n -B "$@" 2>/dev/null; }

# --- the two toolchains write to different places ------------------------
nat=$(recipe build/native/src/core/ctc.o)
mgw=$(recipe MINGW=1 build/mingw/src/core/ctc.o)
if printf '%s' "$nat" | grep -q -- '-o build/native/src/core/ctc\.o'; then
    pass "a native object is written under build/native/"
else fail "the native recipe does not target build/native/: $nat"; fi
if printf '%s' "$mgw" | grep -q -- '-o build/mingw/src/core/ctc\.o'; then
    pass "the same object cross-compiles to build/mingw/ instead"
else fail "the cross recipe does not target build/mingw/: $mgw"; fi
# The compiler has to differ too, or the paths would be segregating nothing.
if printf '%s' "$mgw" | grep -q 'x86_64-w64-mingw32-gcc'; then
    pass "and with the cross compiler, not the host one"
else fail "the cross recipe does not use x86_64-w64-mingw32-gcc: $mgw"; fi

# The library is the object that actually broke the link, so it is checked
# by name rather than inferred from the objects above.
natlib=$(recipe build/native/libp2500.a | grep -c 'ar rcs build/native/libp2500\.a')
mgwlib=$(recipe MINGW=1 build/mingw/libp2500.a | grep -c 'ar rcs build/mingw/libp2500\.a')
if [ "$natlib" -ge 1 ] && [ "$mgwlib" -ge 1 ]; then
    pass "each toolchain archives its own libp2500.a"
else fail "libp2500.a is not per-toolchain (native $natlib, mingw $mgwlib)"; fi
# And nothing is left beside the sources, which is where they used to go.
stray=$(find src -name '*.o' -o -name '*.a' | tr '\n' ' ')
if [ -z "$stray" ]; then
    pass "no objects or libraries beside their sources"
else fail "objects still written into src/: $stray"; fi

# --- format warnings are fatal, for the project's code only --------------
# The measured reason: a %zu inside an SDL_Log is three warnings and exit 0
# on MinGW, so without this a cross build that merely succeeds proves
# nothing. It caught one live breakage the moment it was switched on.
for want in build/native/src/core/ctc.o build/native/src/gui/main.o; do
    if printf '%s' "$(recipe "$want")" | grep -q -- '-Werror=format'; then
        pass "-Werror=format on $(basename "$want" .o) (the project's own code)"
    else fail "$want compiles without -Werror=format"; fi
done
if printf '%s' "$mgw" | grep -q -- '-Werror=format'; then
    pass "...and on the cross build, which is the one it exists for"
else fail "the cross recipe compiles without -Werror=format"; fi
# The negative half: the vendored ImGui is not the project's code and is
# deliberately compiled without -Wall/-Wextra, so making warnings fatal
# there would mean carrying patches to somebody else's library.
imgui=$(recipe build/native/src/gui/vendor/imgui/imgui.o)
if ! printf '%s' "$imgui" | grep -q -- '-Werror'; then
    pass "but not on the vendored ImGui, which keeps its own warning level"
else fail "ImGui is compiled with -Werror: $imgui"; fi

# --- and the cross build actually runs on an ordinary build --------------
# Segregation is only worth having because it lets this happen outside
# publish.sh; without it the two parts above guard a build nobody runs.
if grep -q 'x86_64-w64-mingw32-gcc' build.sh && grep -q 'make MINGW=1' build.sh; then
    pass "build.sh cross-builds when the mingw toolchain is on PATH"
else fail "build.sh no longer runs the Windows cross build"; fi
}

ran=0
for e in $SECTIONS; do
    if want "${e%%:*}" "${e#*:}"; then
        "sec_${e%%:*}"
        ran=$((ran + 1))
    fi
done

echo
# The count is printed because a filtered run should be visibly short - and
# because zero checks is the one outcome that must never read as success,
# whether it came from a selector, or from every selected section skipping
# itself for want of a disk image or a GUI build.
echo "$checks check(s) in $ran section(s)."
if [ "$checks" -eq 0 ]; then
    echo "No checks ran, so nothing was proved. That is a failure, not a pass."
    exit 1
fi
if [ "$fails" -eq 0 ]; then echo "All checks passed."; exit 0; fi
echo "$fails check(s) failed."
exit 1
