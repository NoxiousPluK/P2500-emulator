#include "sysinfo.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "video.h"
#include "cartridge.h"

/* ======================================================================== *
 *  The structural probes
 *
 *  Ported from tools/derive_syms.py (research repo), shape test for shape
 *  test. Each reads a guest operating system's own tables; each returns
 *  false rather than a guess when the shape is not there.
 *
 *  The search window is BIOS space, $C000-$FFFF. Below that is the TPA,
 *  where a running program's data can hold any byte pattern at all - and a
 *  scan that is allowed to find a "device table" in a spreadsheet's
 *  working memory is a scan that will eventually do so.
 * ======================================================================== */

#define BIOS_LO 0xC000u
#define BIOS_HI 0xFFFFu

static uint16_t word_at(const P2500Machine *m, uint32_t a)
{
    return (uint16_t)(p2500_peek(m, (uint16_t)(a & 0xFFFF)) |
                      p2500_peek(m, (uint16_t)((a + 1) & 0xFFFF)) << 8);
}

static uint8_t byte_at(const P2500Machine *m, uint32_t a)
{
    return p2500_peek(m, (uint16_t)(a & 0xFFFF));
}

/* ------------------------------------------------------------ CP/M's map */

bool p2500_sysinfo_cpm_map(const P2500Machine *m, P2500CpmMap *out)
{
    if (!m || !out) return false;
    if (byte_at(m, 0x0000) != 0xC3 || byte_at(m, 0x0005) != 0xC3) return false;
    const uint16_t table = (uint16_t)(word_at(m, 0x0001) - 3);
    /* All 17 slots, or it is not a jump table. Checking one would pass on
     * any three bytes that happen to start $C3. */
    for (int i = 0; i < 17; i++)
        if (byte_at(m, (uint16_t)(table + 3 * i)) != 0xC3) return false;
    const uint16_t bdos = word_at(m, 0x0006);
    out->jump_table = table;
    out->bdos_entry = bdos;
    out->bdos_base = (uint16_t)(bdos & 0xFF00);
    out->ccp_base = (uint16_t)((bdos & 0xFF00) - 0x0800);
    return true;
}

/* ------------------------------------------------------- the disk structures */

static bool parses_as_dpb(const P2500Machine *m, uint16_t a, P2500Dpb *out)
{
    if ((uint32_t)a + 15 > BIOS_HI) return false;
    const uint16_t spt = word_at(m, a);
    const uint8_t bsh = byte_at(m, a + 2);
    const uint8_t blm = byte_at(m, a + 3);
    const uint8_t exm = byte_at(m, a + 4);
    const uint16_t dsm = word_at(m, a + 5);
    const uint16_t drm = word_at(m, a + 7);
    const uint16_t off = word_at(m, a + 13);
    /* BLM == 2^BSH - 1 is the load-bearing one: it ties two independent
     * bytes together arithmetically, which random data does not do. */
    if (!(spt >= 8 && spt <= 256)) return false;
    if (bsh > 7 || blm != (uint8_t)((1u << bsh) - 1u)) return false;
    if (exm > 15) return false;
    if (dsm == 0 || dsm >= 0x8000) return false;
    if (drm == 0 || drm >= 0x4000) return false;
    if (off > 80) return false;
    if (out) {
        out->spt = spt; out->bsh = bsh; out->blm = blm; out->exm = exm;
        out->dsm = dsm; out->drm = drm;
        out->cks = word_at(m, a + 11);
        out->off = off;
        out->block_size = (uint16_t)(128u << bsh);
        out->dir_entries = (uint16_t)(drm + 1);
        out->capacity = (unsigned long)(dsm + 1) * out->block_size;
    }
    return true;
}

bool p2500_sysinfo_disk_map(const P2500Machine *m, P2500DiskMap *out)
{
    if (!m || !out) return false;
    /* The longest run of DPHs at stride 16 sharing one DPB. A single DPH
     * is not enough to be sure: the run is what makes it a table. */
    uint16_t best_at = 0, best_dpb = 0;
    int best_len = 0;
    uint32_t a = BIOS_LO;
    while (a + 16 <= BIOS_HI) {
        const uint16_t dpb = word_at(m, a + 10);
        if (dpb < BIOS_LO || (uint32_t)dpb + 15 > BIOS_HI ||
            !parses_as_dpb(m, dpb, NULL)) { a++; continue; }
        const uint16_t dirbuf = word_at(m, a + 8);
        if (dirbuf < BIOS_LO) { a++; continue; }
        /* Walk the run from here. */
        int len = 0;
        uint32_t r = a;
        while (r + 16 <= BIOS_HI && word_at(m, r + 10) == dpb &&
               word_at(m, r + 8) >= BIOS_LO) {
            len++;
            r += 16;
        }
        if (len > best_len) { best_len = len; best_at = (uint16_t)a; best_dpb = dpb; }
        a++;
    }
    if (best_len == 0) return false;
    memset(out, 0, sizeof *out);
    out->table = best_at;
    out->count = best_len;
    out->dpb_addr = best_dpb;
    out->xlt = word_at(m, best_at);
    out->dirbuf = word_at(m, best_at + 8);
    parses_as_dpb(m, best_dpb, &out->dpb);
    return true;
}

/* ------------------------------------------------------- the device table */

/* The ids this project has established. An id not listed here is printed
 * as an id and nothing more - naming it would be inventing a meaning. */
const char *p2500_sysinfo_device_name(uint8_t id)
{
    switch (id) {
    case 0x30: return "keyboard (console in)";
    case 0x31: return "screen (console out)";
    case 0x40: return "serial / printer";
    case 0x00: case 0x01: case 0x02: case 0x03: return "disk";
    default: return NULL;
    }
}

static bool id_is_known(uint8_t id)
{
    return p2500_sysinfo_device_name(id) != NULL;
}

/* A candidate table at `at`: a count byte, then `n` {id, ptr} records whose
 * pointers all land on [semaphore][driver] pairs in BIOS space. Returns the
 * record count, or 0. */
static int plausible_device_table(const P2500Machine *m, uint16_t at)
{
    const uint8_t n = byte_at(m, at);
    if (n < 2 || n > 32) return 0;
    uint8_t ids[32];
    bool any_known = false;
    for (int i = 0; i < n; i++) {
        const uint32_t r = (uint32_t)at + 1 + 3 * i;
        if (r + 2 > BIOS_HI) return 0;
        const uint8_t id = byte_at(m, r);
        const uint16_t ptr = word_at(m, r + 1);
        if (ptr < BIOS_LO || (uint32_t)ptr + 2 > BIOS_HI) return 0;
        /* +0 is a semaphore: a real one is 0 or 1, never a code byte. */
        if (byte_at(m, ptr) > 1) return 0;
        if (word_at(m, ptr + 1) < BIOS_LO) return 0;
        ids[i] = id;
        if (id_is_known(id)) any_known = true;
    }
    /* Distinct ids - allowing a few repeats, because the disk driver is
     * listed once per unit on some builds - and at least one id this
     * project knows, which is what makes this the machine's device table
     * rather than a well-shaped coincidence. */
    int distinct = 0;
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) if (ids[j] == ids[i]) { dup = true; break; }
        if (!dup) distinct++;
    }
    if (distinct < n - 3 || !any_known) return 0;
    return n;
}

/* How many functions a driver publishes: its prologue does
 * `ld hl,FNTAB / call DISPATCH`, and FNTAB is a count byte then
 * {code, addr} records. -1 when the prologue does not have that shape. */
static int driver_functions(const P2500Machine *m, uint16_t drv, uint16_t *fntab)
{
    for (uint32_t a = drv; a < (uint32_t)drv + 48 && a + 4 <= BIOS_HI; a++) {
        if (byte_at(m, a) != 0x21 || byte_at(m, a + 3) != 0xCD) continue;
        const uint16_t tbl = word_at(m, a + 1);
        if (tbl < BIOS_LO) return -1;
        const uint8_t n = byte_at(m, tbl);
        if (n < 1 || n > 16) return -1;
        if (fntab) *fntab = tbl;
        return n;
    }
    return -1;
}

bool p2500_sysinfo_device_map(const P2500Machine *m, P2500DeviceMap *out)
{
    if (!m || !out) return false;
    uint16_t best_at = 0;
    int best_n = 0;
    for (uint32_t at = BIOS_LO; at + 4 <= BIOS_HI; at++) {
        const int n = plausible_device_table(m, (uint16_t)at);
        if (n > best_n) { best_n = n; best_at = (uint16_t)at; }
    }
    if (best_n == 0) return false;
    memset(out, 0, sizeof *out);
    out->table = best_at;
    out->count = best_n > P2500_SYSINFO_MAX_DEVICES
                 ? P2500_SYSINFO_MAX_DEVICES : best_n;
    for (int i = 0; i < out->count; i++) {
        const uint32_t r = (uint32_t)best_at + 1 + 3 * i;
        const uint16_t ptr = word_at(m, r + 1);
        out->dev[i].id = byte_at(m, r);
        out->dev[i].driver = word_at(m, ptr + 1);
        out->dev[i].functions = driver_functions(m, out->dev[i].driver,
                                                 &out->dev[i].fntab);
    }
    return true;
}

/* ======================================================================== *
 *  The report
 * ======================================================================== */

/* The CRC32 of the boot EPROM dump this project's findings were derived
 * from: roms/ipl.bin, the CPU card's 4K EPROM
 * (../ROM Dumps/CPU-Card-Boot-EPROM/). It is here so the report can say
 * whether the ROM in front of it is that dump - every address in
 * symbols/ipl.sym and every landmark in the IPL trace belongs to this one
 * build, and a different EPROM makes all of them claims about nothing.
 * `make test` asserts the shipped image still produces it, which is what
 * keeps this constant falsifiable rather than decorative. */
#define IPL_KNOWN_CRC32 0x4CB66255u

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}

/* The report is built by appending, so each section can decline to add
 * anything without the ones after it needing to know. */
typedef struct {
    P2500SysLine *out;
    int max, n;
} Sink;

static void add(Sink *s, P2500SysKind kind, const char *label,
                const char *fmt, ...)
{
    if (s->n >= s->max) return;
    P2500SysLine *l = &s->out[s->n++];
    l->kind = kind;
    snprintf(l->label, sizeof l->label, "%s", label ? label : "");
    l->value[0] = '\0';
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(l->value, sizeof l->value, fmt, ap);
        va_end(ap);
    }
}

#define HEAD(t)      add(&s, P2500_SYS_HEADING, (t), NULL)
#define ROW(l, ...)  add(&s, P2500_SYS_ROW, (l), __VA_ARGS__)
#define NOTE(...)    add(&s, P2500_SYS_NOTE, "", __VA_ARGS__)

int p2500_sysinfo(const P2500Machine *m, const P2500Debug *d,
                  const char *symbol_dir, P2500SysLine *out, int max)
{
    if (!m || !out || max <= 0) return 0;
    Sink s = { out, max, 0 };

    /* ---------------------------------------------------------- Machine */
    HEAD("Machine");
    const uint32_t rom_crc = crc32_of(m->eprom, P2500_EPROM_SIZE);
    ROW("boot EPROM", "%u KB, CRC32 $%08X   %s",
        (unsigned)(P2500_EPROM_SIZE / 1024), rom_crc,
        rom_crc == IPL_KNOWN_CRC32
            ? "the dump this project's findings come from"
            : "NOT the known dump - symbols/ipl.sym and the IPL landmarks "
              "describe a different EPROM");

    /* A character ROM of all zeroes is "none loaded" rather than a ROM
     * full of blank glyphs: p2500_load_charrom leaves it zeroed on
     * failure, and a blank screen is worth explaining. */
    bool charrom = false;
    for (size_t i = 0; i < sizeof m->charrom; i++)
        if (m->charrom[i]) { charrom = true; break; }
    P2500VideoInfo vi;
    p2500_video_info(m, &vi);

    /* The stride is 16 bytes per code but the GLYPH is only the cell's own
     * R9+1 scanlines; 12-15 are padding that happens to hold packed Z80
     * code. Saying "rows 0-15" would restate the bug that truncates every
     * descender when you read 8. */
    if (charrom)
        ROW("character ROM", "4 KB, CRC32 $%08X   %d bytes per code, rows 0-%d "
            "are the glyph", crc32_of(m->charrom, sizeof m->charrom),
            P2500_CHARROM_STRIDE, vi.cell_h - 1);
    else
        ROW("character ROM", "not loaded - the text screen will render blank");

    ROW("bank latch (port $05)", "$%02X   $0000-$0FFF %s, $8000-$BFFF %s%s",
        m->bank, (m->bank & 0x08) ? "RAM" : "EPROM",
        p2500_video_window_selected(m) ? "video DRAM" : "main DRAM",
        p2500_bank_is_unknown(m) ? "   [UNDECODED]" : "");
    /* The frame geometry is the CRTC's own arithmetic, not an assumption:
     * (R4+1)*(R9+1)+R5 scanlines of R0+1 character times. Printed because
     * it is what T29 turns into a frame rate once the dot clock is known. */
    const int total_lines = (m->crtc_regs[4] + 1) * (m->crtc_regs[9] + 1) +
                            m->crtc_regs[5];
    ROW("CRTC geometry", "%dx%d chars, %d scanlines/row, %dx%d pixels%s",
        vi.cols, vi.rows, vi.cell_h, vi.width, vi.height,
        vi.graphics ? "   [high-resolution graphics]" : "");
    ROW("CRTC frame", "%d scanlines of %d character times (%d dots)",
        total_lines, m->crtc_regs[0] + 1,
        total_lines * (m->crtc_regs[0] + 1));

    /* The drives, and what is in them. The identity comes from the image's
     * own bytes (a `# match-disk` rule), so it is answerable with the
     * machine halted at reset. */
    for (unsigned u = 0; u < P2500_FDC_MAX_DRIVES; u++) {
        char label[P2500_SYSINFO_LABEL];
        snprintf(label, sizeof label, "drive %c:", 'A' + (int)u);
        if (!m->fdc.disk[u]) { ROW(label, "empty"); continue; }
        char id[96] = "";
        if (symbol_dir)
            p2500_debug_identify_disk(m->fdc.disk[u], m->fdc.disk_size[u],
                                      symbol_dir, id, sizeof id, NULL, 0);
        if (id[0])
            ROW(label, "%lu KB   %s",
                (unsigned long)(m->fdc.disk_size[u] / 1024), id);
        else
            ROW(label, "%lu KB   image not recognised by any shipped rule",
                (unsigned long)(m->fdc.disk_size[u] / 1024));
    }

    /* -------------------------------------------------------- Cartridge
     *
     * The slot is generic (the SESAM dongle is its best-documented use,
     * the P2500's own "Maint" diagnostic plug is another), so the report
     * describes the STREAM rather than naming a product - what is
     * plugged in is the front-end's business, what the machine can see is
     * this. The read and write counts are here because they are the only
     * evidence a protection check leaves */
    HEAD("Cartridge slot (port $0F)");
    const P2500Cartridge *cart = &m->cartridge;
    if (!cart->stream || cart->stream_len == 0) {
        ROW("slot", "empty - every read returns $FF");
    } else {
        /* Byte 0 is the ROM's own presence check: $00 means "a bootable
         * cartridge" and anything else sends the IPL on to the floppy. */
        const uint8_t b0 = cart->stream[0];
        ROW("slot", "%lu byte stream", (unsigned long)cart->stream_len);
        ROW("byte 0", "$%02X   %s", b0, b0 == 0x00
            ? "the IPL will boot from this cartridge"
            : "not a bootable cartridge; the IPL skips it");
        if (cart->stream_len >= 3)
            ROW("first three bytes", "$%02X $%02X $%02X%s",
                cart->stream[0], cart->stream[1], cart->stream[2],
                cart->stream_len == 3
                    ? "   likely a SESAM key" : "");
        ROW("read position", "%lu of %lu", (unsigned long)cart->pos,
            (unsigned long)cart->stream_len);
    }
    
    ROW("traffic", "%lu read%s, %lu write%s",
        cart->reads, cart->reads == 1 ? "" : "s",
        cart->writes, cart->writes == 1 ? "" : "s");

    /* ---------------------------------------------------------- Symbols */
    HEAD("Symbols");
    if (d)
        ROW("names loaded", "%d", d->syms);
    if (symbol_dir) {
        /* The dry run: d == NULL means "report what WOULD be loaded",
         * so this line cannot drift from what `--symbols auto` does. */
        char fits[256] = "";
        const int would = p2500_debug_auto_symbols(NULL, m, symbol_dir,
                                                   fits, sizeof fits);
        if (would > 0 && fits[0])
            ROW("fingerprint", "%s", fits);
        else if (would == 0)
            ROW("fingerprint",
                "none - no shipped file's `# match` window fits this memory");
        else
            ROW("fingerprint", "%s could not be read", symbol_dir);
    }

    /* ------------------------------------------------------------- CP/M */
    P2500CpmMap cpm;
    if (!p2500_sysinfo_cpm_map(m, &cpm)) {
        HEAD("Operating system");
        NOTE("No CP/M page zero here: $0000 and $0005 do not hold two JP "
             "vectors leading to 17 JP slots.");
        NOTE("Nothing below the Machine section can be proved, so nothing "
             "below it is shown.");
        return s.n;
    }

    HEAD("CP/M");
    ROW("CCP base", "$%04X   BDOS base less $800, CP/M's own layout",
        cpm.ccp_base);
    ROW("BDOS", "base $%04X, entry $%04X (from page zero $0006)",
        cpm.bdos_base, cpm.bdos_entry);
    ROW("CBIOS jump table", "$%04X   17 slots, all JP", cpm.jump_table);
    /* The TPA is what a program actually gets, and it is the number the
     * P2206 manual disagrees with (T59) - so print it rather than leave it
     * to arithmetic. The LOADABLE size is one record less than the TPA:
     * the CCP refuses to overwrite itself while loading, measured by
     * bisection at 405 records with 406 giving BAD LOAD. A running program
     * may use that memory afterwards, which is why the two differ. */
    const unsigned long tpa = (unsigned long)(cpm.ccp_base - 0x0100);
    ROW("TPA", "$0100-$%04X   %lu bytes (%lu records)",
        (unsigned)(cpm.ccp_base - 1), tpa, tpa / 128);
    ROW("largest .COM", "%lu records - one less than the TPA, because the CCP "
        "will not be overwritten while loading", tpa / 128 - 1);

    P2500DiskMap dm;
    if (p2500_sysinfo_disk_map(m, &dm)) {
        ROW("DPH table", "$%04X   %d drive letter%s (%c:%s%c:)",
            dm.table, dm.count, dm.count == 1 ? "" : "s", 'A',
            dm.count == 1 ? "" : "-", 'A' + dm.count - 1);
        ROW("drive A: DPB", "$%04X   SPT %u, BSH %u, BLM %u, EXM %u, DSM %u, "
            "DRM %u, CKS %u, OFF %u",
            dm.dpb_addr, dm.dpb.spt, dm.dpb.bsh, dm.dpb.blm, dm.dpb.exm,
            dm.dpb.dsm, dm.dpb.drm, dm.dpb.cks, dm.dpb.off);
        ROW("  decoded", "%u-byte blocks, %u directory entries, "
            "%u reserved tracks, %lu KB of data",
            dm.dpb.block_size, dm.dpb.dir_entries, dm.dpb.off,
            dm.dpb.capacity / 1024);
        /* CKS 16 rather than 0 is why a swapped disk costs write access:
         * these drives are checksummed as removable media. Worth a line
         * because the consequence is invisible until something writes. */
        if (dm.dpb.cks)
            ROW("  removable", "CKS %u, so BDOS checksums the directory and "
                "marks a swapped disk read-only until a warm boot",
                dm.dpb.cks);
    } else {
        NOTE("No DPH table found: no 16-byte record whose +10 parses as a "
             "DPB repeats at stride 16.");
    }

    /* ---------------------------------------------------------- Devices */
    P2500DeviceMap dev;
    if (!p2500_sysinfo_device_map(m, &dev)) {
        HEAD("Devices");
        NOTE("No device table found by shape, so what this BIOS publishes "
             "cannot be listed.");
        return s.n;
    }
    HEAD("Devices");
    ROW("device table", "$%04X   %d device%s", dev.table, dev.count,
        dev.count == 1 ? "" : "s");
    for (int i = 0; i < dev.count; i++) {
        const P2500DeviceEntry *e = &dev.dev[i];
        char label[P2500_SYSINFO_LABEL];
        snprintf(label, sizeof label, "  id $%02X", e->id);
        const char *name = p2500_sysinfo_device_name(e->id);
        if (e->functions >= 0)
            ROW(label, "driver $%04X, %d function%s at $%04X%s%s",
                e->driver, e->functions, e->functions == 1 ? "" : "s",
                e->fntab, name ? "   " : "", name ? name : "");
        else
            ROW(label, "driver $%04X, no function table found%s%s",
                e->driver, name ? "   " : "", name ? name : "");
    }
    return s.n;
}
