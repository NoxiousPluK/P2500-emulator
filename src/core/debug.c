#include "debug.h"

#include <stdio.h>
#include <string.h>

/* ======================================================================== *
 *  Z80 disassembler
 *
 *  Table-driven, and deliberately only two tables: the 0x40-0xBF block is
 *  perfectly regular (ld r,r' and the eight ALU ops over the same eight
 *  register slots), as is all of CB, so generating those is both shorter
 *  and less likely to hold a typo than 192 hand-written strings would be.
 *
 *  Templates carry placeholders, expanded left to right so operand bytes
 *  come out of memory in the order the assembler put them there:
 *    *  16-bit immediate      #  8-bit immediate
 *    @  relative branch target (printed absolute)
 *    (hl) under a DD/FD prefix becomes (ix+d)/(iy+d), consuming the
 *         displacement byte at the point it appears - which is why
 *         `ld (ix+d),n` comes out with its two bytes the right way round.
 * ======================================================================== */

static const char *const REG8[8] = { "b", "c", "d", "e", "h", "l", "(hl)", "a" };
static const char *const ALU[8] = {
    "add a,", "adc a,", "sub ", "sbc a,", "and ", "xor ", "or ", "cp "
};
static const char *const ROT[8] = {
    "rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"
};
static const char *const RP[4] = { "bc", "de", "hl", "sp" };

/* 0x00-0x3F. */
static const char *const OPS_LO[64] = {
    "nop",      "ld bc,*",   "ld (bc),a", "inc bc", "inc b",    "dec b",    "ld b,#",    "rlca",
    "ex af,af'","add hl,bc", "ld a,(bc)", "dec bc", "inc c",    "dec c",    "ld c,#",    "rrca",
    "djnz @",   "ld de,*",   "ld (de),a", "inc de", "inc d",    "dec d",    "ld d,#",    "rla",
    "jr @",     "add hl,de", "ld a,(de)", "dec de", "inc e",    "dec e",    "ld e,#",    "rra",
    "jr nz,@",  "ld hl,*",   "ld (*),hl", "inc hl", "inc h",    "dec h",    "ld h,#",    "daa",
    "jr z,@",   "add hl,hl", "ld hl,(*)", "dec hl", "inc l",    "dec l",    "ld l,#",    "cpl",
    "jr nc,@",  "ld sp,*",   "ld (*),a",  "inc sp", "inc (hl)", "dec (hl)", "ld (hl),#", "scf",
    "jr c,@",   "add hl,sp", "ld a,(*)",  "dec sp", "inc a",    "dec a",    "ld a,#",    "ccf",
};

/* 0xC0-0xFF. The three prefix slots (CB, ED, DD/FD) are never read from
 * here - they are dispatched before the table lookup - but they are filled
 * in so an accidental fallthrough is visible rather than silent. */
static const char *const OPS_HI[64] = {
    "ret nz", "pop bc", "jp nz,*", "jp *",       "call nz,*", "push bc", "add a,#", "rst $00",
    "ret z",  "ret",    "jp z,*",  "?cb",        "call z,*",  "call *",  "adc a,#", "rst $08",
    "ret nc", "pop de", "jp nc,*", "out (#),a",  "call nc,*", "push de", "sub #",   "rst $10",
    "ret c",  "exx",    "jp c,*",  "in a,(#)",   "call c,*",  "?dd",     "sbc a,#", "rst $18",
    "ret po", "pop hl", "jp po,*", "ex (sp),hl", "call po,*", "push hl", "and #",   "rst $20",
    "ret pe", "jp (hl)","jp pe,*", "ex de,hl",   "call pe,*", "?ed",     "xor #",   "rst $28",
    "ret p",  "pop af", "jp p,*",  "di",         "call p,*",  "push af", "or #",    "rst $30",
    "ret m",  "ld sp,hl","jp m,*", "ei",         "call m,*",  "?fd",     "cp #",    "rst $38",
};

/* True when s[i..] is the standalone token `t` - not a substring of a
 * longer mnemonic. Without this, the `l` in `cpl` and the `h` in `halt`
 * would become ixl/ixh under a DD prefix. */
static bool tok_at(const char *s, size_t i, const char *t)
{
    size_t n = strlen(t);
    if (strncmp(s + i, t, n) != 0) return false;
    char before = i ? s[i - 1] : ' ';
    char after = s[i + n];
    bool wordish_before = (before >= 'a' && before <= 'z');
    bool wordish_after = (after >= 'a' && after <= 'z') || (after >= '0' && after <= '9');
    return !wordish_before && !wordish_after;
}

/* Expand one template. `*len` is the number of bytes already accounted for
 * (the opcode and any prefix); operand bytes are read from addr + *len
 * onwards and *len grows as they are consumed. */
static void expand(const P2500Machine *m, uint16_t addr, const char *tpl,
                   const char *idx, bool disp_operand, int *len,
                   char *out, size_t outn)
{
    char tmp[64];
    size_t oi = 0;
    if (outn == 0) return;

#define PUT(...) do { \
        int wrote = snprintf(tmp, sizeof tmp, __VA_ARGS__); \
        if (wrote > 0) for (int wi = 0; wi < wrote && oi + 1 < outn; wi++) out[oi++] = tmp[wi]; \
    } while (0)

    for (size_t i = 0; tpl[i];) {
        uint16_t p = (uint16_t)(addr + *len);
        if (tpl[i] == '*') {
            uint16_t v = (uint16_t)(p2500_peek(m, p) | (p2500_peek(m, (uint16_t)(p + 1)) << 8));
            *len += 2; i++;
            PUT("$%04X", v);
        } else if (tpl[i] == '#') {
            uint8_t v = p2500_peek(m, p);
            *len += 1; i++;
            PUT("$%02X", v);
        } else if (tpl[i] == '@') {
            int8_t d = (int8_t)p2500_peek(m, p);
            *len += 1; i++;
            PUT("$%04X", (uint16_t)(addr + *len + d));
        } else if (idx && disp_operand && strncmp(tpl + i, "(hl)", 4) == 0) {
            int8_t d = (int8_t)p2500_peek(m, p);
            *len += 1; i += 4;
            PUT("(%s%c$%02X)", idx, d < 0 ? '-' : '+', (unsigned)(d < 0 ? -d : d));
        } else if (idx && tok_at(tpl, i, "hl")) {
            i += 2;
            PUT("%s", idx);
        } else if (idx && !disp_operand && (tok_at(tpl, i, "h") || tok_at(tpl, i, "l"))) {
            char which = tpl[i];
            i += 1;
            PUT("%s%c", idx, which);
        } else {
            if (oi + 1 < outn) out[oi++] = tpl[i];
            i++;
        }
    }
#undef PUT
    out[oi] = '\0';
}

/* ED page. Everything regular is generated; only the block-transfer and
 * odd one-offs need a table, and ED's holes really are undefined rather
 * than aliased, so they are reported as such. */
static bool ed_template(uint8_t op, char *tpl, size_t n)
{
    if (op >= 0x40 && op <= 0x7F) {
        int y = (op >> 3) & 7, z = op & 7;
        switch (z) {
        case 0: snprintf(tpl, n, y == 6 ? "in f,(c)" : "in %s,(c)", REG8[y]); return true;
        case 1: snprintf(tpl, n, y == 6 ? "out (c),0" : "out (c),%s", REG8[y]); return true;
        case 2: snprintf(tpl, n, "%s hl,%s", (y & 1) ? "adc" : "sbc", RP[y >> 1]); return true;
        case 3:
            if (y & 1) snprintf(tpl, n, "ld %s,(*)", RP[y >> 1]);
            else       snprintf(tpl, n, "ld (*),%s", RP[y >> 1]);
            return true;
        case 4: snprintf(tpl, n, "neg"); return true;
        case 5: snprintf(tpl, n, op == 0x4D ? "reti" : "retn"); return true;
        case 6: { static const int IM[8] = { 0, 0, 1, 2, 0, 0, 1, 2 };
                  snprintf(tpl, n, "im %d", IM[y]); return true; }
        default:
            switch (op) {
            case 0x47: snprintf(tpl, n, "ld i,a"); return true;
            case 0x4F: snprintf(tpl, n, "ld r,a"); return true;
            case 0x57: snprintf(tpl, n, "ld a,i"); return true;
            case 0x5F: snprintf(tpl, n, "ld a,r"); return true;
            case 0x67: snprintf(tpl, n, "rrd"); return true;
            case 0x6F: snprintf(tpl, n, "rld"); return true;
            default:   snprintf(tpl, n, "nop"); return true; /* ED 77 / ED 7F */
            }
        }
    }
    if (op >= 0xA0 && op <= 0xBB && (op & 4) == 0) {
        static const char *const blk[4][4] = {
            { "ldi",  "cpi",  "ini",  "outi" },
            { "ldd",  "cpd",  "ind",  "outd" },
            { "ldir", "cpir", "inir", "otir" },
            { "lddr", "cpdr", "indr", "otdr" },
        };
        int row = ((op >> 4) - 0x0A) * 2 + ((op >> 3) & 1);
        int col = op & 3;
        if (row >= 0 && row < 4) { snprintf(tpl, n, "%s", blk[row][col]); return true; }
    }
    return false;
}

int p2500_disasm(const P2500Machine *m, uint16_t addr, char *buf, size_t buflen)
{
    char tpl[48];
    const char *idx = NULL;
    int len = 1;
    uint16_t o = addr;
    uint8_t op = p2500_peek(m, addr);

    if (op == 0xDD || op == 0xFD) {
        idx = (op == 0xDD) ? "ix" : "iy";
        o = (uint16_t)(addr + 1);
        op = p2500_peek(m, o);
        len = 2;
        /* DD DD, DD FD and DD ED are legal but meaningless: the CPU treats
         * the first prefix as a one-cycle no-op. Report it as one byte so
         * the caller lands on the prefix that matters. */
        if (op == 0xDD || op == 0xFD || op == 0xED) {
            snprintf(buf, buflen, "defb $%02X ; ignored %s prefix", p2500_peek(m, addr), idx);
            return 1;
        }
    }

    if (op == 0xCB) {
        uint8_t sub;
        int8_t d = 0;
        if (idx) {                       /* DD CB d op - displacement first */
            d = (int8_t)p2500_peek(m, (uint16_t)(o + 1));
            sub = p2500_peek(m, (uint16_t)(o + 2));
            len = 4;
        } else {
            sub = p2500_peek(m, (uint16_t)(o + 1));
            len = 2;
        }
        char target[24];
        if (idx) snprintf(target, sizeof target, "(%s%c$%02X)", idx,
                          d < 0 ? '-' : '+', (unsigned)(d < 0 ? -d : d));
        else snprintf(target, sizeof target, "%s", REG8[sub & 7]);
        /* An indexed CB whose register field is not 6 is one of the
         * undocumented "copy the result into r too" forms; naming the
         * register is how z80dasm reports them. */
        const char *also = (idx && (sub & 7) != 6) ? REG8[sub & 7] : NULL;
        if (sub < 0x40)
            snprintf(buf, buflen, "%s %s%s%s", ROT[(sub >> 3) & 7], target,
                     also ? "," : "", also ? also : "");
        else
            snprintf(buf, buflen, "%s %d,%s%s%s",
                     sub < 0x80 ? "bit" : (sub < 0xC0 ? "res" : "set"),
                     (sub >> 3) & 7, target, also ? "," : "", also ? also : "");
        return len;
    }

    if (op == 0xED) {
        uint8_t sub = p2500_peek(m, (uint16_t)(o + 1));
        len = 2;
        if (!ed_template(sub, tpl, sizeof tpl)) {
            snprintf(buf, buflen, "defb $ED,$%02X", sub);
            return 2;
        }
        expand(m, addr, tpl, NULL, false, &len, buf, buflen);
        return len;
    }

    if (op < 0x40) {
        snprintf(tpl, sizeof tpl, "%s", OPS_LO[op]);
    } else if (op < 0x80) {
        if (op == 0x76) snprintf(tpl, sizeof tpl, "halt");
        else snprintf(tpl, sizeof tpl, "ld %s,%s", REG8[(op >> 3) & 7], REG8[op & 7]);
    } else if (op < 0xC0) {
        snprintf(tpl, sizeof tpl, "%s%s", ALU[(op >> 3) & 7], REG8[op & 7]);
    } else {
        snprintf(tpl, sizeof tpl, "%s", OPS_HI[op - 0xC0]);
    }

    /* $E9 is the exception to "(hl) means (ix+d)": DD E9 is jp (ix), with
     * no displacement byte at all. */
    bool disp_operand = strstr(tpl, "(hl)") != NULL && op != 0xE9;
    expand(m, addr, tpl, idx, disp_operand, &len, buf, buflen);
    return len;
}

/* ======================================================================== *
 *  Watches, counters, breakpoints
 * ======================================================================== */

void p2500_debug_init(P2500Debug *d)
{
    memset(d, 0, sizeof *d);
    d->hit_break = -1;
    d->hit_watch = -1;
    d->last_pc = 0xFFFF;
}

int p2500_debug_add_watch(P2500Debug *d, uint16_t addr, uint16_t len)
{
    if (len == 0) len = 1;
    if (len > P2500_DEBUG_WATCH_MAX_LEN) len = P2500_DEBUG_WATCH_MAX_LEN;
    for (int i = 0; i < d->watches; i++)
        if (d->watch[i].addr == addr && d->watch[i].len == len) return i;
    if (d->watches >= P2500_DEBUG_MAX_WATCHES) return -1;
    int i = d->watches++;
    d->watch[i].addr = addr;
    d->watch[i].len = len;
    d->watch[i].enabled = true;
    memset(d->watch[i].last, 0, sizeof d->watch[i].last);
    return i;
}

int p2500_debug_add_count(P2500Debug *d, uint16_t addr)
{
    for (int i = 0; i < d->counts; i++)
        if (d->count[i].addr == addr) return i;
    if (d->counts >= P2500_DEBUG_MAX_COUNTS) return -1;
    int i = d->counts++;
    d->count[i].addr = addr;
    d->count[i].hits = 0;
    d->count[i].first_step = 0;
    d->count[i].enabled = true;
    return i;
}

int p2500_debug_find_break(const P2500Debug *d, uint16_t addr)
{
    for (int i = 0; i < d->breaks; i++)
        if (d->brk[i].addr == addr) return i;
    return -1;
}

int p2500_debug_add_break(P2500Debug *d, uint16_t addr)
{
    int existing = p2500_debug_find_break(d, addr);
    if (existing >= 0) return existing;
    if (d->breaks >= P2500_DEBUG_MAX_BREAKS) return -1;
    int i = d->breaks++;
    d->brk[i].addr = addr;
    d->brk[i].enabled = true;
    return i;
}

#define REMOVE_AT(arr, n, index) do { \
        if ((index) < 0 || (index) >= (n)) return; \
        for (int ri = (index); ri + 1 < (n); ri++) (arr)[ri] = (arr)[ri + 1]; \
        (n)--; \
    } while (0)

void p2500_debug_remove_watch(P2500Debug *d, int index) { REMOVE_AT(d->watch, d->watches, index); }
void p2500_debug_remove_count(P2500Debug *d, int index) { REMOVE_AT(d->count, d->counts, index); }
void p2500_debug_remove_break(P2500Debug *d, int index) { REMOVE_AT(d->brk, d->breaks, index); }
#undef REMOVE_AT

void p2500_debug_toggle_break(P2500Debug *d, uint16_t addr)
{
    int at = p2500_debug_find_break(d, addr);
    if (at >= 0) p2500_debug_remove_break(d, at);
    else p2500_debug_add_break(d, addr);
}

void p2500_debug_baseline(P2500Debug *d, const P2500Machine *m)
{
    for (int i = 0; i < d->watches; i++)
        for (uint16_t j = 0; j < d->watch[i].len; j++)
            d->watch[i].last[j] = p2500_peek(m, (uint16_t)(d->watch[i].addr + j));
}

void p2500_debug_resume(P2500Debug *d)
{
    d->hit_break = -1;
    d->hit_watch = -1;
    d->skip_one = true;
}

bool p2500_debug_before_step(P2500Debug *d, const P2500Machine *m, unsigned long step)
{
    const uint16_t pc = m->cpu.pc;
    bool stop = false;
    d->hit_watch = -1;

    for (int i = 0; i < d->watches; i++) {
        if (!d->watch[i].enabled) continue;
        for (uint16_t j = 0; j < d->watch[i].len; j++) {
            uint16_t a = (uint16_t)(d->watch[i].addr + j);
            uint8_t now = p2500_peek(m, a);
            if (now == d->watch[i].last[j]) continue;
            uint8_t was = d->watch[i].last[j];
            d->watch[i].last[j] = now;
            if (d->watch[i].stop) {
                stop = true;
                d->hit_watch = i;
                d->hit_watch_addr = a;
            }
            if (!d->on_watch) continue;
            /* The change is noticed on the poll AFTER the instruction that
             * made it, so `pc` is the next instruction, not the culprit.
             * last_pc is: report both, because "what wrote this" is the
             * question being asked and the other number is one instruction
             * away from answering it. */
            char text[224];
            snprintf(text, sizeof text,
                     "[step %lu] [watch $%04X] $%02X -> $%02X, written by $%04X, "
                     "now PC=$%04X SP=$%04X A=$%02X BC=$%02X%02X DE=$%02X%02X HL=$%02X%02X",
                     step, a, was, now, d->last_pc, pc, m->cpu.sp, m->cpu.a,
                     m->cpu.b, m->cpu.c, m->cpu.d, m->cpu.e, m->cpu.h, m->cpu.l);
            d->on_watch(d->userdata, a, was, now, text);
        }
    }
    d->last_pc = pc;

    for (int i = 0; i < d->counts; i++) {
        if (!d->count[i].enabled || d->count[i].addr != pc) continue;
        if (d->count[i].hits == 0) d->count[i].first_step = step;
        d->count[i].hits++;
    }

    /* A watchpoint stop is not bypassed by resume(): the byte has already
     * been recorded as its new value, so continuing cannot re-trigger on
     * the same change the way stepping off a breakpoint address would. */
    if (stop) return true;

    if (d->skip_one) { d->skip_one = false; return false; }
    for (int i = 0; i < d->breaks; i++) {
        if (!d->brk[i].enabled || d->brk[i].addr != pc) continue;
        d->hit_break = i;
        return true;
    }
    return false;
}

/* ======================================================================== *
 *  Live state, as label/value lines
 *
 *  One line per fact, so an ImGui two-column table and a `--state` text
 *  dump are the same data laid out differently rather than two readings of
 *  the same registers.
 * ======================================================================== */

#include <stdarg.h>
#include "video.h"

static void put(char *dst, size_t n, const char *fmt, ...)
{
    if (!dst || n == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, n, fmt, ap);
    va_end(ap);
}

#define LBL(...) put(label, label_n, __VA_ARGS__)
#define VAL(...) put(value, value_n, __VA_ARGS__)

static const char *const TOPIC_NAMES[P2500_DBG_TOPICS] = {
    "CPU", "Interrupts", "CTC", "PIO", "DMA", "FDC", "CRTC", "Video", "Misc",
};

const char *p2500_debug_topic_name(P2500DebugTopic topic)
{
    if (topic < 0 || topic >= P2500_DBG_TOPICS) return "?";
    return TOPIC_NAMES[topic];
}

static int fdc_lines(const P2500Machine *m)
{
    return 7 + (m->fdc.command_len ? 1 : 0) + (m->fdc.result_len ? 1 : 0);
}

int p2500_debug_topic_lines(const P2500Machine *m, P2500DebugTopic topic)
{
    switch (topic) {
    case P2500_DBG_CPU:   return 15;
    case P2500_DBG_INT:   return P2500_INT_SOURCES + 1;
    case P2500_DBG_CTC:   return P2500_CTC_CHANNELS + 1;
    case P2500_DBG_PIO:   return P2500_PIO_PORTS;
    case P2500_DBG_DMA:   return 6;
    case P2500_DBG_FDC:   return fdc_lines(m);
    case P2500_DBG_CRTC:  return 6;
    case P2500_DBG_VIDEO: return 5;
    case P2500_DBG_MISC:  return 6;
    default: return 0;
    }
}

static void line_cpu(const P2500Machine *m, int line,
                     char *label, size_t label_n, char *value, size_t value_n)
{
    const z80 *z = &m->cpu;
    switch (line) {
    case 0: {
        char text[64];
        p2500_disasm(m, z->pc, text, sizeof text);
        LBL("PC"); VAL("$%04X   %s", z->pc, text);
        break;
    }
    case 1: LBL("SP"); VAL("$%04X", z->sp); break;
    case 2: LBL("A / flags");
            VAL("$%02X   %c%c%c%c%c%c%c%c", z->a,
                z->sf ? 'S' : '.', z->zf ? 'Z' : '.', z->yf ? '5' : '.',
                z->hf ? 'H' : '.', z->xf ? '3' : '.', z->pf ? 'P' : '.',
                z->nf ? 'N' : '.', z->cf ? 'C' : '.');
            break;
    case 3: LBL("BC"); VAL("$%02X%02X", z->b, z->c); break;
    case 4: LBL("DE"); VAL("$%02X%02X", z->d, z->e); break;
    case 5: LBL("HL"); VAL("$%02X%02X", z->h, z->l); break;
    case 6: LBL("IX"); VAL("$%04X", z->ix); break;
    case 7: LBL("IY"); VAL("$%04X", z->iy); break;
    case 8: LBL("alternates");
            VAL("AF'=$%02X%02X BC'=$%02X%02X DE'=$%02X%02X HL'=$%02X%02X",
                z->a_, z->f_, z->b_, z->c_, z->d_, z->e_, z->h_, z->l_);
            break;
    case 9: LBL("I / R"); VAL("$%02X / $%02X", z->i, z->r); break;
    case 10: LBL("interrupts");
             VAL("IM %d   IFF1=%d IFF2=%d%s", z->interrupt_mode, z->iff1, z->iff2,
                 z->int_pending ? "   vector pending" : "");
             break;
    case 11: LBL("state"); VAL("%s", z->halted ? "HALTED" : "running"); break;
    case 12: {
        /* Port $05's two decoded fields. An undecodable value is called
         * out rather than shown as a plain hex byte: six of the eight
         * window combinations have never been observed and one of them
         * showing up is a finding, not noise. */
        const uint8_t b = m->bank;
        LBL("bank (port $05)");
        VAL("$%02X   $0000-$0FFF %s, $8000-$BFFF %s%s", b,
            (b & 0x08) ? "RAM" : "EPROM",
            p2500_video_window_selected(m) ? "video DRAM" : "main DRAM",
            p2500_bank_is_unknown(m) ? "  [UNDECODED]" : "");
        break;
    }
    case 13: LBL("T-states");
             VAL("%lu   (%.3f s emulated)", z->cyc,
                 (double)z->cyc / (double)P2500_CPU_HZ);
             break;
    default: LBL("instructions"); VAL("%lu", m->total_instructions); break;
    }
}

static void line_int(const P2500Machine *m, int line,
                     char *label, size_t label_n, char *value, size_t value_n)
{
    if (line >= P2500_INT_SOURCES) {
        LBL("offered to CPU");
        VAL("%s", m->int_offered >= 0 ? p2500_intctl_name(m->int_offered) : "none");
        return;
    }
    const int src = p2500_intctl_chain_order[line];
    const P2500IntCtl *ic = &m->intctl;
    LBL("%d. %s", line, p2500_intctl_name(src));
    VAL("vec $%02X   %lu req / %lu ack   %s", ic->vector[src],
        ic->requests[src], ic->acknowledged[src],
        ic->under_service[src] ? "IN SERVICE"
                               : (ic->requested[src] ? "REQUESTING" : "idle"));
}

static void line_ctc(const P2500Machine *m, int line,
                     char *label, size_t label_n, char *value, size_t value_n)
{
    const P2500Ctc *c = &m->ctc;
    if (line == 0) {
        LBL("vector base");
        VAL("$%02X %s", c->vector_base, c->vector_set ? "" : "(never programmed)");
        return;
    }
    const int ch = line - 1;
    LBL("channel %d", ch);
    VAL("%s %s  TC=$%02X  counter=%u  %s  vec $%02X  CLK/TRG=%d%s%s",
        c->counter_mode[ch] ? "COUNTER" : "TIMER",
        c->counter_mode[ch] ? "     " : (c->prescaler_256[ch] ? "/256 " : "/16  "),
        c->time_constant[ch], c->counter[ch],
        c->int_enabled[ch] ? "int on " : "int off",
        (uint8_t)(c->vector_base + ch * 2), c->clk_trg[ch],
        c->started[ch] ? "" : "  [no time constant yet]",
        c->state[ch] == P2500_CTC_WAIT_TIME_CONSTANT ? "  [awaiting TC]" : "");
}

static void line_pio(const P2500Machine *m, int line,
                     char *label, size_t label_n, char *value, size_t value_n)
{
    const P2500Pio *p = &m->pio;
    LBL("port %c", 'A' + line);
    VAL("mode %u  in-mask $%02X  out $%02X  in $%02X  %s vec $%02X%s  %s %s  monitor $%02X",
        p->mode[line], p->io_mask[line], p->output_latch[line], p->input_latch[line],
        p->int_enabled[line] ? "int on " : "int off", p->vector[line],
        p->vector_set[line] ? "" : " (unset)",
        p->and_mode[line] ? "AND" : "OR ",
        p->active_high[line] ? "active-high" : "active-low ",
        p->monitor_mask[line]);
}

static void line_dma(const P2500Machine *m, int line,
                     char *label, size_t label_n, char *value, size_t value_n)
{
    const P2500Dma *d = &m->dma;
    switch (line) {
    case 0: LBL("direction");
            VAL("%s", d->direction == P2500_DMA_DIR_IO_TO_MEMORY ? "IO -> memory (read)"
                    : d->direction == P2500_DMA_DIR_MEMORY_TO_IO ? "memory -> IO (write)"
                    : "not programmed");
            break;
    case 1: LBL("block length"); VAL("$%04X (%u bytes)", d->block_length, d->block_length); break;
    case 2: LBL("port B address"); VAL("$%04X", d->port_b_addr); break;
    case 3: LBL("vector"); VAL("$%02X", d->vector); break;
    case 4: LBL("flags");
            VAL("%s  %s  %s", d->loaded ? "loaded" : "not loaded",
                d->interrupts_enabled ? "int on " : "int off",
                d->dma_enabled ? "enabled" : "disabled");
            break;
    default: LBL("register stream");
             VAL("%s", d->queue_len ? "mid-group" : "expecting a base register byte");
             break;
    }
}

static void line_fdc(const P2500Machine *m, int line,
                     char *label, size_t label_n, char *value, size_t value_n)
{
    const P2500Fdc *f = &m->fdc;
    switch (line) {
    case 0: LBL("phase");
            if (f->phase == P2500_FDC_COMMAND)
                VAL("COMMAND  %zu of %zu byte(s) taken", f->command_len, f->command_expected);
            else if (f->phase == P2500_FDC_RESULT)
                VAL("RESULT   %zu of %zu byte(s) read", f->result_pos, f->result_len);
            else
                VAL("IDLE     main status $%02X", 0x80);
            break;
    case 1: LBL("selected unit"); VAL("%u (%c:)", f->unit, 'A' + f->unit); break;
    case 2: LBL("cylinder"); VAL("%u   last seek %s", f->cylinder,
                                 f->last_seek_ok ? "ok" : "failed"); break;
    case 3: LBL("INT line"); VAL("%s", f->int_line ? "ASSERTED (held until the result phase is read)"
                                                   : "released"); break;
    case 4: LBL("seek interrupt"); VAL("%s", f->seek_int_pending ? "pending" : "none"); break;
    case 5: LBL("post-reset ints");
            VAL("%d of 2 left%s", f->startup_interrupts_remaining,
                f->real_operation_started ? ", a real command has been issued" : "");
            break;
    case 6: {
        char t[128] = "";
        size_t at = 0;
        for (unsigned u = 0; u < P2500_FDC_MAX_DRIVES; u++) {
            int w;
            if (f->disk[u])
                w = snprintf(t + at, sizeof t - at, "%s%c: %zu KB", at ? "  " : "",
                             'A' + (int)u, f->disk_size[u] / 1024);
            else
                w = snprintf(t + at, sizeof t - at, "%s%c: -", at ? "  " : "", 'A' + (int)u);
            if (w > 0) at += (size_t)w;
            if (at >= sizeof t) break;
        }
        LBL("drives"); VAL("%s", t);
        break;
    }
    default: {
        /* Lines 7 and 8 exist only while the corresponding phase holds
         * bytes, which is what fdc_lines() counts. */
        bool want_cmd = f->command_len && line == 7;
        const uint8_t *bytes = want_cmd ? f->command : f->result;
        size_t n = want_cmd ? f->command_len : f->result_len;
        char t[64] = "";
        size_t at = 0;
        for (size_t i = 0; i < n && at + 3 < sizeof t; i++)
            at += (size_t)snprintf(t + at, sizeof t - at, "%s%02X", i ? " " : "", bytes[i]);
        LBL("%s bytes", want_cmd ? "command" : "result"); VAL("%s", t);
        break;
    }
    }
}

static void line_crtc(const P2500Machine *m, int line,
                      char *label, size_t label_n, char *value, size_t value_n)
{
    const uint8_t *r = m->crtc_regs;
    switch (line) {
    case 0: LBL("index (port $08)"); VAL("R%u", m->crtc_index); break;
    case 1: LBL("R0-R8");
            VAL("%02X %02X %02X %02X %02X %02X %02X %02X %02X",
                r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
            break;
    case 2: LBL("R9-R17");
            VAL("%02X %02X %02X %02X %02X %02X %02X %02X %02X",
                r[9], r[10], r[11], r[12], r[13], r[14], r[15], r[16], r[17]);
            break;
    case 3: {
        P2500VideoInfo vi;
        p2500_video_info(m, &vi);
        LBL("geometry");
        VAL("%d x %d chars, %d scanlines/row -> %d x %d px",
            vi.cols, vi.rows, vi.cell_h, vi.width, vi.height);
        break;
    }
    case 4: LBL("start address (R12/13)");
            VAL("$%04X", (unsigned)(((r[12] & 0x3F) << 8) | r[13]));
            break;
    default: {
        int cell = p2500_video_cursor_cell(m);
        P2500VideoInfo vi;
        p2500_video_info(m, &vi);
        unsigned cur = (unsigned)(((r[14] & 0x3F) << 8) | r[15]);
        LBL("cursor (R14/15)");
        if (cell >= 0 && vi.cols > 0)
            VAL("$%04X -> row %d col %d, scanlines %u-%u%s", cur,
                cell / vi.cols, cell % vi.cols, (unsigned)(r[10] & 0x1F),
                (unsigned)(r[11] & 0x1F),
                (r[10] & 0x60) == 0x20 ? ", hidden" : (r[10] & 0x40) ? ", blinking" : "");
        else
            VAL("$%04X -> outside the displayed area", cur);
        break;
    }
    }
}

static void line_video(const P2500Machine *m, int line,
                       char *label, size_t label_n, char *value, size_t value_n)
{
    switch (line) {
    case 0: {
        const uint8_t a = m->attr_latch;
        LBL("attribute latch");
        VAL("$%X   %s%s%s%s%s", a,
            (a & P2500_ATTR_UNDERLINE) ? "underline " : "",
            (a & P2500_ATTR_DIM) ? "dim " : "",
            (a & P2500_ATTR_REVERSE) ? "reverse " : "",
            (a & P2500_ATTR_FLASH) ? "flash " : "",
            a ? "" : "(plain)");
        break;
    }
    case 1: LBL("port $0A");
            VAL("$%02X   %s", m->port0a_latch,
                (m->port0a_latch & 0x40) ? "HIGH-RESOLUTION GRAPHICS (see T47)"
                                         : "character mode");
            break;
    case 2: LBL("attributed writes"); VAL("%lu", m->attr_writes); break;
    case 3: LBL("cursor cell"); VAL("%d", p2500_video_cursor_cell(m)); break;
    default: LBL("$8000-$BFFF");
             VAL("%s", p2500_video_window_selected(m) ? "the video card's DRAM"
                                                      : "main DRAM");
             break;
    }
}

static void line_misc(const P2500Machine *m, int line,
                      char *label, size_t label_n, char *value, size_t value_n)
{
    switch (line) {
    case 0: LBL("Cartridge (port $0F)");
            VAL("%lu read(s), %lu write(s)", m->cartridge.reads, m->cartridge.writes);
            break;
    case 1: {
        char t[128] = "";
        size_t at = 0;
        for (int p = 0; p < 256 && at + 16 < sizeof t; p++) {
            if (m->unhandled_out[p])
                at += (size_t)snprintf(t + at, sizeof t - at, "%s$%02X out x%lu",
                                       at ? " " : "", p, m->unhandled_out[p]);
            if (at + 16 >= sizeof t) break;
            if (m->unhandled_in[p])
                at += (size_t)snprintf(t + at, sizeof t - at, "%s$%02X in x%lu",
                                       at ? " " : "", p, m->unhandled_in[p]);
        }
        LBL("ports with no model"); VAL("%s", at ? t : "none");
        break;
    }
    case 2: LBL("undecoded $05 writes"); VAL("%lu", m->unknown_bank_writes); break;
    case 3: {
        const P2500Keyboard *k = &m->keyboard;
        unsigned depth = (unsigned)((k->ring_head - k->ring_tail) & 63u);
        LBL("keyboard");
        VAL("%u byte(s) in the live ring; scripted %zu of %zu delivered",
            depth, k->pos, k->queue_len);
        break;
    }
    case 4: LBL("50 Hz clock strobe");
            VAL("level %d, %u of %u T-states into the phase", m->clock_strobe_level,
                m->clock_strobe_phase, P2500_CPU_HZ / P2500_CLOCK_TICK_HZ);
            break;
    default: LBL("serial lines (port $05)");
             VAL("RXD=%d  TX-ready=%d", m->serial_rxd, m->serial_tx_ready);
             break;
    }
}

void p2500_debug_line(const P2500Machine *m, P2500DebugTopic topic, int line,
                      char *label, size_t label_n, char *value, size_t value_n)
{
    if (label && label_n) label[0] = '\0';
    if (value && value_n) value[0] = '\0';
    if (line < 0 || line >= p2500_debug_topic_lines(m, topic)) return;
    switch (topic) {
    case P2500_DBG_CPU:   line_cpu(m, line, label, label_n, value, value_n); break;
    case P2500_DBG_INT:   line_int(m, line, label, label_n, value, value_n); break;
    case P2500_DBG_CTC:   line_ctc(m, line, label, label_n, value, value_n); break;
    case P2500_DBG_PIO:   line_pio(m, line, label, label_n, value, value_n); break;
    case P2500_DBG_DMA:   line_dma(m, line, label, label_n, value, value_n); break;
    case P2500_DBG_FDC:   line_fdc(m, line, label, label_n, value, value_n); break;
    case P2500_DBG_CRTC:  line_crtc(m, line, label, label_n, value, value_n); break;
    case P2500_DBG_VIDEO: line_video(m, line, label, label_n, value, value_n); break;
    default:              line_misc(m, line, label, label_n, value, value_n); break;
    }
}

#undef LBL
#undef VAL
