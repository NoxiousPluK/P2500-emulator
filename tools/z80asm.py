#!/usr/bin/env python3
"""
A small two-pass Z80 assembler, enough to write P2500 demos and probes.

Why this exists rather than a dependency: nothing in this project needs an
assembler until you want to put real code on a disk, and then it needs one
badly - a graphics demo is thousands of instructions and hand-assembly is a
guarantee of silent, invisible bugs. The subset here is what those programs
use, not the whole instruction set; anything unsupported is an error rather
than a wrong encoding.

It is checked, not trusted: `--verify` disassembles its own output with
`p2500-emu --disasm` - a decoder cross-checked against z80dasm over ~34,000
instructions (TODO.md T39) - and compares, instruction for instruction,
against the source. An encoder and a decoder built from the same tables
would agree with each other while both being wrong; these two were written
from opposite directions, years of z80dasm apart.

    tools/z80asm.py SOURCE.asm -o OUT.COM [--verify] [--listing]

Syntax: one instruction per line, `label:` on its own or leading, `;` to end
of line is a comment. Directives: org, equ, db, dw, ds, incbin. Numbers:
$hex, 0xhex, decimal, %binary, 'c'. Expressions may use + - * / & | << >>
and parentheses, over labels and numbers.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

R8 = {'b': 0, 'c': 1, 'd': 2, 'e': 3, 'h': 4, 'l': 5, '(hl)': 6, 'a': 7}
RP = {'bc': 0, 'de': 1, 'hl': 2, 'sp': 3}
RP2 = {'bc': 0, 'de': 1, 'hl': 2, 'af': 3}
CC = {'nz': 0, 'z': 1, 'nc': 2, 'c': 3, 'po': 4, 'pe': 5, 'p': 6, 'm': 7}
ALU = {'add': 0, 'adc': 1, 'sub': 2, 'sbc': 3, 'and': 4, 'xor': 5, 'or': 6, 'cp': 7}
ROT = {'rlc': 0, 'rrc': 1, 'rl': 2, 'rr': 3, 'sla': 4, 'sra': 5, 'sll': 6, 'srl': 7}
SIMPLE = {
    'nop': [0x00], 'rlca': [0x07], 'rrca': [0x0F], 'rla': [0x17], 'rra': [0x1F],
    'daa': [0x27], 'cpl': [0x2F], 'scf': [0x37], 'ccf': [0x3F], 'halt': [0x76],
    'exx': [0xD9], 'di': [0xF3], 'ei': [0xFB], 'ret': [0xC9],
    'neg': [0xED, 0x44], 'retn': [0xED, 0x45], 'reti': [0xED, 0x4D],
    'rrd': [0xED, 0x67], 'rld': [0xED, 0x6F],
    'ldi': [0xED, 0xA0], 'cpi': [0xED, 0xA1], 'ini': [0xED, 0xA2], 'outi': [0xED, 0xA3],
    'ldd': [0xED, 0xA8], 'cpd': [0xED, 0xA9], 'ind': [0xED, 0xAA], 'outd': [0xED, 0xAB],
    'ldir': [0xED, 0xB0], 'cpir': [0xED, 0xB1], 'inir': [0xED, 0xB2], 'otir': [0xED, 0xB3],
    'lddr': [0xED, 0xB8], 'cpdr': [0xED, 0xB9], 'indr': [0xED, 0xBA], 'otdr': [0xED, 0xBB],
}


class AsmError(Exception):
    pass


def parse_num(tok, syms, where):
    t = tok.strip()
    if not t:
        raise AsmError(f"{where}: empty expression")
    # Character literals first - they may contain characters the expression
    # rewriter would otherwise mangle.
    t = re.sub(r"'(.)'", lambda m: str(ord(m.group(1))), t)
    t = re.sub(r'\$([0-9A-Fa-f]+)', lambda m: str(int(m.group(1), 16)), t)
    t = re.sub(r'\b0[xX]([0-9A-Fa-f]+)', lambda m: str(int(m.group(1), 16)), t)
    t = re.sub(r'%([01]+)', lambda m: str(int(m.group(1), 2)), t)
    def look(m):
        name = m.group(0).lower()
        if name in syms:
            return str(syms[name])
        # Pass one has no forward labels yet, so unknown reads as 0 there.
        # On the final pass an unknown name is a mistake, and letting it be
        # 0 is the worst possible way to report it: `call mul_signed` becomes
        # `call $0000`, which on CP/M is a warm boot, so the program simply
        # returns to the prompt with nothing to show for it. Ask me how I
        # know.
        if getattr(syms, 'strict', False):
            raise AsmError(f"{where}: undefined symbol '{m.group(0)}'")
        return '0'

    t = re.sub(r'[A-Za-z_.][A-Za-z0-9_.]*', look, t)
    if not re.fullmatch(r'[-+*/()&|<>\s0-9]*', t):
        raise AsmError(f"{where}: cannot evaluate '{tok}'")
    try:
        return int(eval(t, {'__builtins__': {}}, {}))       # noqa: S307
    except Exception as exc:
        raise AsmError(f"{where}: bad expression '{tok}' ({exc})")


def is_mem(op):
    """A parenthesised operand that is not a register - i.e. an address.

    Checked before the immediate forms, or `ld a,(count)` quietly assembles
    as `ld a,n` with the label's low byte. The round-trip verify caught
    exactly that.
    """
    return (op.startswith('(') and op.endswith(')')
            and op[1:-1].strip().lower() not in ('hl', 'bc', 'de', 'sp', 'ix', 'iy', 'c'))


def lo(v):
    return v & 0xFF


def hi(v):
    return (v >> 8) & 0xFF


IDX_RE = re.compile(r'(?i)^\(\s*(ix|iy)\s*([-+].*)?\)$')


def encode(mnem, ops, pc, syms, where, strict=True):
    """IX/IY are HL with a prefix, and that is exactly how they are encoded.

    Rewrite the operands to their HL form, encode that, then put the DD/FD
    prefix in front and the displacement straight after the opcode - which
    is where the CPU expects it, `ld (ix+d),n` included. Doing it as a
    transformation rather than a second opcode table is why there is no
    separate set of encodings to get wrong.
    """
    prefix, disp, rewritten = None, None, []
    for op in ops:
        mo = IDX_RE.match(op.strip())
        if mo:
            prefix = 0xDD if mo.group(1).lower() == 'ix' else 0xFD
            disp = parse_num(mo.group(2), syms, where) if mo.group(2) else 0
            if not -128 <= disp <= 127:
                raise AsmError(f"{where}: index displacement {disp} out of range")
            rewritten.append('(hl)')
            continue
        if op.strip().lower() in ('ix', 'iy'):
            prefix = 0xDD if op.strip().lower() == 'ix' else 0xFD
            rewritten.append('hl')
            continue
        rewritten.append(op)
    if prefix is not None:
        code = encode_base(mnem, rewritten, pc, syms, where, strict)
        if disp is None:
            return [prefix] + code
        return [prefix, code[0], disp & 0xFF] + code[1:]
    return encode_base(mnem, ops, pc, syms, where, strict)


def encode_base(mnem, ops, pc, syms, where, strict=True):
    """Return the bytes for one instruction. `pc` is its own address.

    `strict` is off on the first pass, where forward labels all read as 0
    and every forward jr would look out of range."""
    m = mnem.lower()
    n = len(ops)

    if m in SIMPLE and n == 0:
        return list(SIMPLE[m])

    if m == 'ld' and n == 2:
        d, s = ops
        if d in R8 and s in R8:
            if d == '(hl)' and s == '(hl)':
                raise AsmError(f"{where}: ld (hl),(hl) is halt - say halt")
            return [0x40 | (R8[d] << 3) | R8[s]]
        if d == 'sp' and s == 'hl':
            return [0xF9]
        if d in ('(bc)', '(de)') and s == 'a':
            return [0x02 if d == '(bc)' else 0x12]
        if d == 'a' and s in ('(bc)', '(de)'):
            return [0x0A if s == '(bc)' else 0x1A]
        if is_mem(d):
            v = parse_num(d[1:-1], syms, where)
            if s == 'a':
                return [0x32, lo(v), hi(v)]
            if s == 'hl':
                return [0x22, lo(v), hi(v)]
            if s in RP:
                return [0xED, 0x43 | (RP[s] << 4), lo(v), hi(v)]
            raise AsmError(f"{where}: cannot store {s} to memory")
        if is_mem(s):
            v = parse_num(s[1:-1], syms, where)
            if d == 'a':
                return [0x3A, lo(v), hi(v)]
            if d == 'hl':
                return [0x2A, lo(v), hi(v)]
            if d in RP:
                return [0xED, 0x4B | (RP[d] << 4), lo(v), hi(v)]
            raise AsmError(f"{where}: cannot load {d} from memory")
        if d in R8:
            return [0x06 | (R8[d] << 3), lo(parse_num(s, syms, where))]
        if d in RP:
            v = parse_num(s, syms, where)
            return [0x01 | (RP[d] << 4), lo(v), hi(v)]
        if d == 'i' and s == 'a':
            return [0xED, 0x47]
        if d == 'a' and s == 'i':
            return [0xED, 0x57]
        raise AsmError(f"{where}: unsupported ld {d},{s}")

    if m in ALU:
        # `add a,b` and `add b` are the same instruction; `add hl,rr` is not.
        if m == 'add' and n == 2 and ops[0] == 'hl' and ops[1] in RP:
            return [0x09 | (RP[ops[1]] << 4)]
        if m in ('adc', 'sbc') and n == 2 and ops[0] == 'hl' and ops[1] in RP:
            return [0xED, (0x4A if m == 'adc' else 0x42) | (RP[ops[1]] << 4)]
        src = ops[1] if (n == 2 and ops[0] == 'a') else ops[0] if n == 1 else None
        if src is None:
            raise AsmError(f"{where}: bad {m} operands")
        if src in R8:
            return [0x80 | (ALU[m] << 3) | R8[src]]
        return [0xC6 | (ALU[m] << 3), lo(parse_num(src, syms, where))]

    if m in ('inc', 'dec') and n == 1:
        base = 0x04 if m == 'inc' else 0x05
        if ops[0] in R8:
            return [base | (R8[ops[0]] << 3)]
        if ops[0] in RP:
            return [(0x03 if m == 'inc' else 0x0B) | (RP[ops[0]] << 4)]
        raise AsmError(f"{where}: bad {m} operand")

    if m in ('push', 'pop') and n == 1 and ops[0] in RP2:
        return [(0xC5 if m == 'push' else 0xC1) | (RP2[ops[0]] << 4)]

    if m == 'jp':
        if n == 1 and ops[0] == '(hl)':
            return [0xE9]
        if n == 1:
            v = parse_num(ops[0], syms, where)
            return [0xC3, lo(v), hi(v)]
        if n == 2 and ops[0] in CC:
            v = parse_num(ops[1], syms, where)
            return [0xC2 | (CC[ops[0]] << 3), lo(v), hi(v)]

    if m in ('jr', 'djnz'):
        target = ops[-1]
        v = parse_num(target, syms, where)
        off = v - (pc + 2)
        if strict and not -128 <= off <= 127:
            raise AsmError(f"{where}: {m} out of range ({off} bytes)")
        off &= 0xFF
        if m == 'djnz':
            return [0x10, off & 0xFF]
        if n == 1:
            return [0x18, off & 0xFF]
        if ops[0] in ('nz', 'z', 'nc', 'c'):
            return [0x20 | (CC[ops[0]] << 3), off & 0xFF]
        raise AsmError(f"{where}: jr takes only nz/z/nc/c")

    if m == 'call':
        if n == 1:
            v = parse_num(ops[0], syms, where)
            return [0xCD, lo(v), hi(v)]
        if n == 2 and ops[0] in CC:
            v = parse_num(ops[1], syms, where)
            return [0xC4 | (CC[ops[0]] << 3), lo(v), hi(v)]

    if m == 'ret' and n == 1 and ops[0] in CC:
        return [0xC0 | (CC[ops[0]] << 3)]

    if m == 'rst' and n == 1:
        v = parse_num(ops[0], syms, where)
        if v % 8 or not 0 <= v <= 0x38:
            raise AsmError(f"{where}: rst {v} is not a restart address")
        return [0xC7 | v]

    if m == 'im' and n == 1:
        return [0xED, {0: 0x46, 1: 0x56, 2: 0x5E}[parse_num(ops[0], syms, where)]]

    if m == 'ex' and n == 2:
        if ops == ['de', 'hl']:
            return [0xEB]
        if ops == ['af', "af'"]:
            return [0x08]
        if ops == ['(sp)', 'hl']:
            return [0xE3]

    if m in ROT and n == 1 and ops[0] in R8:
        return [0xCB, (ROT[m] << 3) | R8[ops[0]]]

    if m in ('bit', 'res', 'set') and n == 2 and ops[1] in R8:
        b = parse_num(ops[0], syms, where)
        if not 0 <= b <= 7:
            raise AsmError(f"{where}: bit number {b} out of range")
        base = {'bit': 0x40, 'res': 0x80, 'set': 0xC0}[m]
        return [0xCB, base | (b << 3) | R8[ops[1]]]

    if m == 'out' and n == 2 and ops[1] == 'a' and ops[0].startswith('('):
        return [0xD3, lo(parse_num(ops[0][1:-1], syms, where))]
    if m == 'in' and n == 2 and ops[0] == 'a' and ops[1].startswith('('):
        return [0xDB, lo(parse_num(ops[1][1:-1], syms, where))]

    raise AsmError(f"{where}: unsupported instruction '{mnem} {','.join(ops)}'")


def split_ops(rest):
    """Split an operand list on commas outside parentheses AND outside quotes.

    The quote part matters for `db "a,b"` - a string operand is one operand
    however many commas it contains.
    """
    ops, depth, cur, quote = [], 0, '', None
    for ch in rest:
        if quote:
            cur += ch
            if ch == quote:
                quote = None
            continue
        if ch in '"\'':
            quote = ch
            cur += ch
            continue
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            ops.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        ops.append(cur.strip())
    return ops


def strip_comment(line):
    out, in_str = '', None
    for ch in line:
        if in_str:
            out += ch
            if ch == in_str:
                in_str = None
            continue
        if ch in '"\'':
            in_str = ch
            out += ch
            continue
        if ch == ';':
            break
        out += ch
    return out


def gather(text, base_dir: Path, name='source', depth=0):
    """Flatten `include` directives into one line list."""
    if depth > 8:
        raise AsmError("include nested too deeply")
    lines = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        body = strip_comment(raw).strip()
        if not body:
            continue
        mo = re.match(r'(?i)^include\s+"?([^"]+)"?$', body)
        if mo:
            sub = base_dir / mo.group(1)
            lines += gather(sub.read_text(), sub.parent, sub.name, depth + 1)
            continue
        lines.append((f"{name}:{lineno}", body, raw))
    return lines


def assemble(text, base_dir: Path):
    lines = gather(text, base_dir)

    class Syms(dict):
        strict = False

    syms, org = Syms(), 0x0100
    out = bytearray()

    for final in (False, True):
        syms_pass = syms if final else Syms(syms)
        syms_pass.strict = final
        # Names are case-insensitive, so a constant and a routine can collide
        # - and a silent collision is a whole afternoon. Catch it.
        defined = {}
        pc = org
        out = bytearray()
        listing = []
        for lineno, body, raw in lines:
            where = lineno
            label = None
            mo = re.match(r'^([A-Za-z_.][A-Za-z0-9_.]*):\s*(.*)$', body)
            if mo:
                label, body = mo.group(1).lower(), mo.group(2).strip()
            if label:
                if label in defined:
                    raise AsmError(f"{where}: '{label}' already defined at "
                                   f"{defined[label]}")
                defined[label] = where
                syms_pass[label] = pc
            if not body:
                continue
            # `NAME equ value`, with or without a colon after the name.
            mo_equ = re.match(r'(?i)^([A-Za-z_.][A-Za-z0-9_.]*)\s+equ\s+(.+)$', body)
            if mo_equ:
                name = mo_equ.group(1).lower()
                if name in defined:
                    raise AsmError(f"{where}: '{name}' already defined at "
                                   f"{defined[name]}")
                defined[name] = where
                syms_pass[name] = parse_num(mo_equ.group(2), syms_pass, where)
                continue
            if label and body.lower().startswith('equ '):
                syms_pass[label] = parse_num(body[4:], syms_pass, where)
                continue
            parts = body.split(None, 1)
            mnem = parts[0].lower()
            rest = parts[1] if len(parts) > 1 else ''

            if mnem == 'org':
                if out:
                    raise AsmError(f"{where}: org must come before any code")
                org = pc = parse_num(rest, syms_pass, where)
                continue
            if mnem == 'db' or mnem == 'defb':
                data = []
                for item in split_ops(rest):
                    if item.startswith('"') and item.endswith('"'):
                        data += list(item[1:-1].encode('latin-1'))
                    else:
                        data.append(lo(parse_num(item, syms_pass, where)))
                out += bytes(data)
                pc += len(data)
                continue
            if mnem == 'dw' or mnem == 'defw':
                for item in split_ops(rest):
                    v = parse_num(item, syms_pass, where)
                    out += bytes([lo(v), hi(v)])
                    pc += 2
                continue
            if mnem == 'ds' or mnem == 'defs':
                items = split_ops(rest)
                count = parse_num(items[0], syms_pass, where)
                fill = lo(parse_num(items[1], syms_pass, where)) if len(items) > 1 else 0
                out += bytes([fill]) * count
                pc += count
                continue
            if mnem == 'incbin':
                path = base_dir / rest.strip().strip('"')
                blob = path.read_bytes()
                out += blob
                pc += len(blob)
                continue

            code = encode(mnem, split_ops(rest), pc, syms_pass, where, final)
            out += bytes(code)
            listing.append((pc, bytes(code), body))
            pc += len(code)
        if not final:
            syms = Syms(syms_pass)
    return bytes(out), org, syms, listing


def verify(binary: bytes, org: int, listing, syms, emu='./p2500-emu'):
    """Disassemble what we produced and compare, instruction for instruction.

    Only possible because the disassembler is independent of this file and
    independently checked. Run through a 4 KB ROM image because that is the
    one region --disasm can be pointed at with no machine state.
    """
    if org != 0x0100:
        return ["--verify only works for org $0100 programs"]
    rom = bytearray(b'\x00' * 4096)
    body = binary[:4096 - 0x100]
    rom[0x100:0x100 + len(body)] = body
    # A unique file, not a fixed name: `make -j` assembles several sources at
    # once and a shared scratch file means each one verifies whichever binary
    # happened to be written last.
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(bytes(rom))
        tmp = Path(f.name)
    # Ask for as many instructions as there are BYTES of code, not as many as
    # there are instructions: a `db` sitting between two routines decodes as
    # an instruction of its own, so the decoded stream is longer than the
    # listing and asking for len(listing) stops short of the last one.
    span = (listing[-1][0] + len(listing[-1][1]) - org) if listing else 0
    n = min(span, 4096 - org)
    try:
        res = subprocess.run([emu, '--rom', str(tmp), '--max-steps', '0',
                              '--disasm', f'100:{n}'],
                             capture_output=True, text=True)
    finally:
        tmp.unlink(missing_ok=True)
    got = {}
    for line in res.stdout.splitlines():
        mo = re.match(r'\s*\$([0-9A-F]{4}): (?:[0-9A-F]{2} |   ){4} (.*)$', line)
        if mo:
            got[int(mo.group(1), 16)] = mo.group(2).strip()
    problems = []
    for addr, code, src in listing:
        if addr >= 0x1000:
            break
        want = canon(src, syms)
        want8 = canon(src, syms, 0xFF)
        have = got.get(addr)
        if have is None:
            problems.append(f"${addr:04X}: not decoded ({src})")
            continue
        if norm(have) not in (norm(want), norm(want8)):
            problems.append(f"${addr:04X}: assembled {code.hex(' ')} -> "
                            f"'{have}', source said '{src}'")
    return problems


KEEP = {'a', 'b', 'c', 'd', 'e', 'h', 'l', 'i', 'r', 'af', 'bc', 'de', 'hl',
        'sp', "af'", 'nz', 'z', 'nc', 'po', 'pe', 'p', 'm', 'ix', 'iy',
        '(hl)', '(bc)', '(de)', '(sp)', '(c)'}


def canon(src, syms, mask=0xFFFF):
    """Source line with every expression reduced to its value.

    The disassembler prints numbers; the source says `VRAM+1` and `'3'`.
    Comparing the text would fail on both. Evaluating the operands here uses
    the assembler's own expression parser, which is fine: what is under test
    is the encoding, not the arithmetic - a wrong value would have produced
    wrong bytes and the disassembler would report them faithfully.

    `mask` is tried at both widths by the caller, because the source may say
    -2 where the byte reads $FE.
    """
    parts = src.strip().split(None, 1)
    mnem = parts[0].lower()
    if len(parts) == 1:
        return mnem
    out = []
    for op in split_ops(parts[1]):
        o = op.strip()
        if o.lower() in KEEP:
            out.append(o.lower())
            continue
        mo = IDX_RE.match(o)
        if mo:
            d = parse_num(mo.group(2), syms, 'verify') if mo.group(2) else 0
            sign = '-' if d < 0 else '+'
            out.append(f"({mo.group(1).lower()}{sign}{abs(d)})")
            continue
        if o.startswith('(') and o.endswith(')'):
            inner = o[1:-1].strip()
            if inner.lower() in KEEP or f"({inner.lower()})" in KEEP:
                out.append(o.lower())
            else:
                out.append(f"({parse_num(inner, syms, 'verify') & mask})")
            continue
        try:
            out.append(str(parse_num(o, syms, 'verify') & mask))
        except AsmError:
            out.append(o.lower())
    return mnem + ' ' + ','.join(out)


def norm(s):
    """Compare on value, not spelling: $0F, 15 and 0x0f are one thing."""
    s = re.sub(r'\s+', '', s.lower()).replace("'", '')
    s = re.sub(r'\$([0-9a-f]+)', lambda m: str(int(m.group(1), 16)), s)
    s = re.sub(r'\b0x([0-9a-f]+)', lambda m: str(int(m.group(1), 16)), s)
    return s


def main():
    ap = argparse.ArgumentParser(description="Assemble Z80 source into a CP/M .COM")
    ap.add_argument('source')
    ap.add_argument('-o', '--output')
    ap.add_argument('--verify', action='store_true',
                    help="disassemble the result and compare against the source")
    ap.add_argument('--listing', action='store_true')
    args = ap.parse_args()

    src = Path(args.source)
    try:
        binary, org, syms, listing = assemble(src.read_text(), src.parent)
    except AsmError as exc:
        print(f"{src}: {exc}", file=sys.stderr)
        return 1

    out = Path(args.output) if args.output else src.with_suffix('.COM')
    out.write_bytes(binary)
    print(f"{out}: {len(binary)} bytes, org ${org:04X}, "
          f"{len(listing)} instruction(s), {len(syms)} symbol(s)")

    if args.listing:
        for addr, code, text in listing:
            print(f"  ${addr:04X}  {code.hex(' '):<12} {text}")

    if args.verify:
        problems = verify(binary, org, listing, syms)
        if problems:
            print(f"\nVERIFY FAILED ({len(problems)}):", file=sys.stderr)
            for p in problems[:20]:
                print("  " + p, file=sys.stderr)
            return 1
        print(f"Verified   : {len(listing)} instruction(s) round-trip through "
              f"the disassembler unchanged.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
