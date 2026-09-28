#!/bin/sh
# Regression suite for the P2500 emulator (TODO.md T24). Every check is a
# falsifiable claim about observable state, not "it didn't crash":
#
#   1. SESAM  - the banner VRAM must be byte-identical to the Python-era
#               reference dump. Guards the CPU core, the SESAM port and the
#               video write path.
#   2. IPL    - Phase 1: a real disk boot must reach $4A00, plus the four
#               sector-0 landmarks.
#   3. CP/M   - Phase 2: page zero must hold CP/M's real vectors, CBIOS's
#               $E200 jump table must be JP instructions, and the screen
#               must read "58K CP/M Ver. 2.2" with an A> prompt.
#   4. Console- typing DIR at the prompt must list exactly the six files
#               the disk image actually contains. This exercises the whole
#               chain at once: CTC channel 3 + port $06 in, the interrupt
#               daisy chain, the FDC/DMA read path, and CONOUT.
#
# usage: tools/run_tests.sh   (or: make test)
set -u
cd "$(dirname "$0")/.."

EMU=./p2500-emu
DISK="../Disk Images/extracted/P25K_B/P25K_B.raw"
REF="../ROM Dumps/CPU-Card-Boot-EPROM/emulation/vram_after_banner.bin"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fails=0

pass() { printf '  PASS  %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }

# Render the 80x24 text screen out of a VRAM dump.
screen() { od -An -v -tu1 -w1 "$1" | awk 'NR<=1920{c=$1;
    printf "%s", (c>=32 && c<127) ? sprintf("%c",c) : " ";
    if (NR%80==0) printf "\n"}'; }

echo "== 1. SESAM bootable-cartridge banner (byte-exact VRAM)"
if [ ! -f "$REF" ]; then
    fail "reference dump missing: $REF"
else
    $EMU --rom roms/ipl.bin --sesam roms/sesam_banner_test.bin \
         --max-steps 2000000 --dump-vram "$TMP/sesam.bin" >"$TMP/sesam.log" 2>&1
    if cmp -s "$TMP/sesam.bin" "$REF"; then pass "VRAM identical to $REF"
    else fail "VRAM differs from $REF"; fi
fi

echo "== 2. IPL disk boot reaches Phase 1's landmarks"
if [ ! -f "$DISK" ]; then
    fail "disk image missing: $DISK"
else
    $EMU --disk "$DISK" --max-steps 2000000 >"$TMP/ipl.log" 2>&1
    for lm in 1000 1002 1003 1006 4A00; do
        if grep -q "landmark \$$lm .* hit #1" "$TMP/ipl.log"; then pass "landmark \$$lm hit"
        else fail "landmark \$$lm never hit"; fi
    done

    echo "== 3. CP/M 2.2 is up"
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

    echo "== 4. Typing DIR at the prompt lists the disk's real contents"
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

echo "== 4b. The IPL's own give-up path still works"
# A disk that is readable but not bootable must make sub_0333h return, so the
# IPL prints its own banner and halts - rather than blocking in the $06C6
# busy-wait. This is the only test of the failure path.
PSYS="../Disk Images/extracted/P2k5_LOGIC/P2k5_LOGIC_deinterleaved.raw"
if [ ! -f "$PSYS" ]; then
    echo "  SKIP  $PSYS not present"
else
    $EMU --disk "$PSYS" --max-steps 30000000 --dump-vram "$TMP/ipl.bin" \
         >"$TMP/ipl.log" 2>&1
    screen "$TMP/ipl.bin" >"$TMP/ipl.screen"
    if grep -q 'P H I L I P S' "$TMP/ipl.screen" &&
       grep -q 'MICROCOMPUTER' "$TMP/ipl.screen" &&
       grep -q 'P2000/B' "$TMP/ipl.screen"; then
        pass "unbootable disk prints the IPL banner"
    else fail "IPL banner not shown for an unbootable disk"; fi
    if grep -q 'Final: PC=\$014D' "$TMP/ipl.log"; then
        pass "IPL halted at \$014D as designed"
    else fail "IPL did not reach its halt at \$014D"; fi
fi

echo "== 4c. Drive B: (T44)"
GAMES="../Disk Images/extracted/P2500GAM/P2500GAM.raw"
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

echo "== 5. The undecoded-video-bank tripwire still fires (T27)"
# roms/sesam_bank_probe.bin is a SESAM cartridge whose payload is
# LD A,$01 / OUT ($05),A / HALT - i.e. it selects one of port $05's six
# undecoded $8000-$BFFF windows, the most likely home of the video card's
# attribute plane. Nothing in any disk image does this, so without a
# deliberate probe the diagnostic would be untested code that quietly rots.
$EMU --sesam roms/sesam_bank_probe.bin --max-steps 3000000 \
     >"$TMP/bank.log" 2>&1
if grep -q 'selects an unknown \$8000-\$BFFF window' "$TMP/bank.log"; then
    pass "unknown bank select is logged"
else fail "unknown bank select was NOT logged - the T27 tripwire is dead"; fi
if grep -q 'selected an undecoded \$8000-\$BFFF window' "$TMP/bank.log"; then
    pass "unknown bank select is counted in the exit report"
else fail "unknown bank select missing from the exit report"; fi

echo "== 6. The core renderer (T37) and the live keyboard ring (T38)"
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

if [ -x ./p2500-gui ]; then
    echo "== 7. The SDL3 front-end boots headless"
    # A stale p2500-gui once passed this whole section while the library
    # under it had moved on by hours, so check the binary is actually newer
    # than what it links against. `make test` now depends on the GUI too, but
    # this script can be run on its own.
    if [ ./p2500-gui -nt libp2500.a ]; then
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
    SDL_VIDEODRIVER=dummy ./p2500-gui --disk "$DISK" --frames 200 \
        --shot-window "$TMP/win.ppm" >>"$TMP/gui.log" 2>&1
    python3 - "$TMP/win.ppm" <<'PY' && pass "menu bar renders and offsets the screen" || fail "menu bar missing or screen not offset"
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
# The emulated screen must start below the bar, not at y=0.
check(not any(lit(x, 1) for x in range(200, w)),
      "content at y=1 outside the menu area - screen was not offset")
sys.exit(0 if ok else 1)
PY
else
    echo "== 7. SDL3 front-end - skipped (run 'make gui' to build it)"
fi

echo
if [ "$fails" -eq 0 ]; then echo "All checks passed."; exit 0; fi
echo "$fails check(s) failed."
exit 1
