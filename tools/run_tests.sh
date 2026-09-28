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

echo "== 4d. Swapping a disk mid-run"
# What a swap does and does not cost, both asserted, because the front-end
# used to tell people the wrong one.
#
# Reads refresh by themselves: CP/M re-reads the directory on every search,
# so a DIR straight after a swap lists the new disk with no warm boot. What
# the swap does cost is WRITE access - these drives carry CKS=16 in their
# DPB, i.e. removable media, so BDOS compares the directory checksum, finds
# it changed, and sets the drive's bit in its read-only vector at $E1AD.
# Ctrl-C clears it through the BDOS reset at $E086. That flag has no visible
# consequence until writing exists (T30), which is exactly why the claim
# needs a test rather than a sentence in a tooltip.
SYSDISK="../Disk Images/extracted/P25K_S/P25K_S.raw"
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
# labels under the old strip and it went on passing.
PROBE_X, PROBE_Y = range(118, 144), range(2, 22)
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
    # The offset itself comes from the front-end's own layout report: a
    # screenshot cannot distinguish an offset screen from one drawn at y=0,
    # because the screen background and the window clear colour are the same
    # and the top rows of a character cell are blank.
    if grep -qE 'layout: menu [0-9]+ px, screen at y=[1-9]' "$TMP/gui.log"; then
        pass "screen is offset below the menu bar"
    else fail "screen not offset below the menu bar"; fi

    # --- the debugger panels (TODO.md T39) ---------------------------------
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
    LAMPROW=$(sed -n 's/.*lamps: caps \([0-9]*\) run \([0-9]*\) drives \([0-9]*\) [0-9]* [0-9]* rows \([0-9]*\)-\([0-9]*\).*/\1 \2 \3 \4 \5/p' "$TMP/gui.log" | head -1)
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
    # loses focus, which turns SDL_EVENT_TEXT_INPUT off for the whole window.
    # That is where every printable key the guest receives comes from, so one
    # use of any address field used to leave the machine untypeable. Key
    # events were unaffected, so the menus and F-keys went on working and hid
    # it - which is why this is asserted on SDL's own text-input state and
    # not on WantCaptureKeyboard, which looks correct throughout.
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
else
    echo "== 7. SDL3 front-end - skipped (run 'make gui' to build it)"
fi

echo "== 8. The shared debug core (TODO.md T39)"
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
    if python3 tools/disasm_crosscheck.py --rounds 2 >"$TMP/crosscheck.log" 2>&1; then
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
# after it. The poll happens between instructions, so the naive answer is one
# late - which is what this used to print.
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

echo "== 9. Core diagnostics go through the log callback (TODO.md T34)"
# The core no longer writes to stderr; the CLI installs a sink that puts the
# "[cat] " prefix back, so this output is what it always was. The risk in
# that refactor is a message quietly going nowhere, so check every category
# still arrives.
./p2500-emu --disk "$DISK" --max-steps 4000000 --verbose-io --type 'DIR\r' \
    >/dev/null 2>"$TMP/verbose.err"
missing=""
for cat in ctc dma fdc io int pio sesam; do
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

echo "== 10. Building a disk image (tools/cpm_build.py)"
# The write half of the disk format had nothing testing it: cpm_extract.py
# has been checked against nine real images, but nothing proved this project
# could produce one. Build a bootable disk from scratch, boot it, and run a
# program off it - which exercises the sector skew, the directory encoding,
# the block map and the system-file copy in one go. A wrong skew fails here
# and not subtly.
if python3 tools/mk_cpm_probe.py "$TMP/PROBE.COM" 'BEFORE\eY\x20\x20\ek\e0PREVERSED\e0@ plain' >/dev/null 2>&1 &&
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

echo "== 11. High-resolution graphics mode (TODO.md T47)"
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
python3 tools/mk_cpm_probe.py "$TMP/GFX.COM" "$(cat "$TMP/gfx.seq")" >/dev/null
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

echo "== 13. The speed control (TODO.md T54)"
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
    L_SPEED=$(sed -n 's/.*lamps: .*speed \([0-9]*\).*/\1/p' "$TMP/gui.log" | head -1)
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

echo
if [ "$fails" -eq 0 ]; then echo "All checks passed."; exit 0; fi
echo "$fails check(s) failed."
exit 1
