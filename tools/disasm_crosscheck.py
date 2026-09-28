#!/usr/bin/env python3
"""Cross-check core/debug.c's Z80 disassembler against z80dasm.

Both disassemblers walk the same byte stream, so they stay in step only as
long as they agree on every instruction's *length*: a single disagreement
desynchronises the rest of the stream and shows up loudly. That makes this a
real test of decoding rather than of spelling.

Syntax is normalised away on both sides - hex literals become #<decimal>,
z80dasm's `$+n` relative form becomes the absolute target - so what is left
to compare is the mnemonic, the operands and the boundaries.

    tools/disasm_crosscheck.py            # the IPL ROM plus generated blobs
    tools/disasm_crosscheck.py --rounds 8 # more random coverage
"""
import argparse, os, random, re, shutil, subprocess, sys, tempfile

ROM_BYTES = 4096  # p2500_load_rom insists on exactly one EPROM's worth

# ED's undefined opcodes are the one place the two disassemblers legitimately
# disagree on length: z80dasm emits the ED as a lone one-byte `defb`, while a
# real Z80 - and superzazu's core, which is what our PC actually follows -
# fetches the second byte and treats the pair as a two-byte NOP. Ours matches
# the CPU, so instead of encoding z80dasm's convention here, those byte pairs
# are kept out of the generated streams; letting them through would only
# desynchronise everything after them and hide real faults.
ED_DEFINED = ({op for op in range(0x40, 0x80) if (op & 7) < 4} |
              {0x44, 0x45, 0x4D, 0x46, 0x56, 0x5E,
               0x47, 0x4F, 0x57, 0x5F, 0x67, 0x6F} |
              {0xA0, 0xA1, 0xA2, 0xA3, 0xA8, 0xA9, 0xAA, 0xAB,
               0xB0, 0xB1, 0xB2, 0xB3, 0xB8, 0xB9, 0xBA, 0xBB})


def avoid_undefined_ed(data, rng):
    """Rewrite the byte after each $ED into a defined ED opcode."""
    out = bytearray(data)
    i = 0
    while i < len(out) - 1:
        if out[i] == 0xED and out[i + 1] not in ED_DEFINED:
            out[i + 1] = rng.choice(sorted(ED_DEFINED))
        i += 1
    return bytes(out)


def gen_systematic():
    """Every opcode of a page, each followed by two operand-shaped bytes.

    Padding to three bytes covers instructions that take a 16-bit operand;
    where an instruction is shorter the leftovers decode as further
    instructions, which is fine - it is still one byte stream both sides
    must agree on.
    """
    blobs, cur = [], bytearray()
    def flush():
        nonlocal cur
        if cur:
            blobs.append(bytes(cur).ljust(ROM_BYTES, b'\x00')[:ROM_BYTES])
            cur = bytearray()

    for prefix in ([], [0xCB], [0xED], [0xDD], [0xFD]):
        for op in range(256):
            if prefix == [0xED] and op not in ED_DEFINED:
                continue
            seq = bytes(prefix) + bytes([op, 0x34, 0x12])
            if len(cur) + len(seq) > ROM_BYTES:
                flush()
            cur += seq
        flush()
    # DD CB d op / FD CB d op - the displacement sits before the opcode,
    # which is the one place the operand order is easy to get wrong.
    for prefix in (0xDD, 0xFD):
        for op in range(256):
            seq = bytes([prefix, 0xCB, 0xF8, op])
            if len(cur) + len(seq) > ROM_BYTES:
                flush()
            cur += seq
        flush()
    rng = random.Random(1)
    return [avoid_undefined_ed(b, rng) for b in blobs]


def gen_random(rounds, seed):
    rng = random.Random(seed)
    return [avoid_undefined_ed(bytes(rng.randrange(256) for _ in range(ROM_BYTES)), rng)
            for _ in range(rounds)]


HEX_H = re.compile(r'(?<![0-9a-zA-Z$_])0*([0-9a-f]+)h(?![0-9a-zA-Z_])')
HEX_D = re.compile(r'\$([0-9A-Fa-f]+)')
REL = re.compile(r'\$([+-])(\d+)')


RST = re.compile(r'\brst (\d+)\b')


def norm_dasm(text, addr):
    text = re.sub(r'\s+', ' ', text.strip())
    # z80dasm prints rst targets in decimal and spells sll "sli".
    text = RST.sub(lambda m: 'rst #%d' % int(m.group(1)), text)
    text = re.sub(r'\bsli\b', 'sll', text)
    # Undocumented DD CB forms: z80dasm writes "res 6,(ix+3) & ld h,(ix+3)",
    # we write "res 6,(ix+3),h". Same instruction, same length.
    text = re.sub(r'^(.*) & ld (\w+),\((i[xy][^)]*)\)$', r'\1,\2', text)
    text = REL.sub(lambda m: '#%d' % ((addr + int(m.group(2)) *
                                      (1 if m.group(1) == '+' else -1)) & 0xFFFF), text)
    text = HEX_H.sub(lambda m: '#%d' % int(m.group(1), 16), text)
    return text


def norm_mine(text):
    text = re.sub(r'\s+', ' ', text.strip())
    return HEX_D.sub(lambda m: '#%d' % int(m.group(1), 16), text)


ROW = re.compile(r';([0-9a-f]{4})\t([0-9a-f ]*)\t')


def run_z80dasm(path):
    """(address, mnemonic, is_defb, byte_count) per disassembled line.

    The address comment is found by pattern rather than by splitting on the
    first `;`, because an illegal-byte line carries a second comment of its
    own ("defb 0fdh ;illegal sequence ;0004 fd") and splitting would throw
    the address away - which silently drops exactly the rows this tool needs
    in order to resynchronise.
    """
    out = subprocess.run(['z80dasm', '-a', '-t', '-u', '-g', '0', path],
                         capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        m = ROW.search(line)
        if not m or '\torg\t' in line:
            continue
        mnem = line.split(';')[0].strip()
        if not mnem:
            continue
        nbytes = len(m.group(2).split()) or 1
        rows.append((int(m.group(1), 16), mnem, mnem.startswith('defb'), nbytes))
    return rows


def run_mine(emu, path, count):
    out = subprocess.run([emu, '--rom', path, '--max-steps', '0',
                          '--disasm', '0:%d' % count, '--no-stuck-detect'],
                         capture_output=True, text=True).stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r'\s*\$([0-9A-F]{4}): ((?:[0-9A-F]{2} |   ){4}) (.*)$', line)
        if m:
            rows.append((int(m.group(1), 16), m.group(3)))
    return rows


def compare(emu, blob, name, limit):
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(blob)
        path = f.name
    try:
        theirs = run_z80dasm(path)
        mine = run_mine(emu, path, len(theirs) + 8)
    finally:
        os.unlink(path)

    # Walked in lockstep, so a length disagreement is reported once where it
    # happens rather than as a cascade of missing addresses.
    #
    # z80dasm renders bytes it will not decode as a one-byte `defb`, then
    # decodes the following byte on its own; we instead report what the CPU
    # will really do - a DD/FD prefix on an opcode with no index form, or an
    # undefined ED, is a two-byte instruction. Both consume the same bytes,
    # so the streams resynchronise immediately; those steps are counted as
    # `convention` rather than as faults.
    bad = convention = compared = 0
    i = j = 0
    after_defb = False
    while i < len(theirs) and j < len(mine) and bad < limit:
        ta, tt, t_defb, _ = theirs[i]
        ma, mt = mine[j]
        if ta == ma:
            a, b = norm_dasm(tt, ta), norm_mine(mt)
            if t_defb or after_defb:
                convention += 1
            elif a != b:
                print('%s $%04X: z80dasm "%s" != ours "%s"' % (name, ta, a, b))
                bad += 1
            else:
                compared += 1
            after_defb = t_defb
            i += 1
            j += 1
        elif ta < ma:
            if t_defb or after_defb:
                convention += 1
            else:
                print('%s: length disagreement - we skipped $%04X, where z80dasm has '
                      '"%s"' % (name, ta, norm_dasm(tt, ta)))
                bad += 1
            after_defb = t_defb or after_defb
            i += 1
        else:
            if after_defb:
                convention += 1
            else:
                print('%s: length disagreement - z80dasm skipped $%04X, where we have '
                      '"%s"' % (name, ma, norm_mine(mt)))
                bad += 1
            j += 1
    if bad >= limit:
        print('%s: stopping after %d mismatch(es)' % (name, bad))
    return bad, compared, convention


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--emu', default='./p2500-emu')
    ap.add_argument('--rom', default='roms/ipl.bin')
    ap.add_argument('--rounds', type=int, default=4, help='random blobs to try')
    ap.add_argument('--seed', type=int, default=20260928)
    ap.add_argument('--max-report', type=int, default=25)
    args = ap.parse_args()

    if not shutil.which('z80dasm'):
        print('z80dasm not installed - skipping the cross-check')
        return 0
    if not os.path.exists(args.emu):
        print('%s not built' % args.emu)
        return 1

    cases = []
    if os.path.exists(args.rom):
        rom = open(args.rom, 'rb').read()[:ROM_BYTES].ljust(ROM_BYTES, b'\0')
        cases.append(('ipl.bin', avoid_undefined_ed(rom, random.Random(0))))
    cases += [('systematic-%d' % i, b) for i, b in enumerate(gen_systematic())]
    cases += [('random-%d' % i, b) for i, b in enumerate(gen_random(args.rounds, args.seed))]

    total_bad = total = total_conv = 0
    for name, blob in cases:
        bad, n, conv = compare(args.emu, blob, name, args.max_report)
        total_bad += bad
        total += n
        total_conv += conv
    print('%d instruction(s) compared, %d mismatch(es), %d skipped as a known '
          'convention difference' % (total, total_bad, total_conv))
    return 1 if total_bad else 0


if __name__ == '__main__':
    sys.exit(main())
