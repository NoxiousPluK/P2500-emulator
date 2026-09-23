#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint16_t addr;
    const char *name;
    int hit_count;
    int gated_count; /* hits at this PC where the byte there didn't match - not counted */
    bool gate_from_rom; /* fill `expected` from the loaded ROM/EPROM once it's known */
    int expected;    /* -1 = no known byte, just require nonzero; else the exact byte */
} Landmark;

/* TODO.md T8: a landmark should only count if the byte(s) actually there
 * match what the ROM/sector is supposed to contain, not just "PC happened
 * to equal this address" - that's exactly how the old $1100/$1103/$1106
 * landmarks kept reporting hits from a NOP walk through zeroed RAM (see
 * TODO.md's "Where the boot sector actually goes" section). ROM-resident
 * addresses get their expected byte read straight out of the loaded EPROM
 * at startup (gate_from_rom); the boot sector's first bytes are known
 * statically from disk (TODO.md: `00 00 FB 11 30 10 CD 03 00 ...` at
 * $1000). Addresses whose real content isn't known yet ($4A00, the
 * runtime-built $FEC9 trampoline) fall back to "byte must be nonzero",
 * which is weaker but still catches the zeroed-RAM NOP-walk case. */
static Landmark landmarks[] = {
    {0x0000, "reset vector", 0, 0, true, 0},
    {0x0100, "cold boot entry", 0, 0, true, 0},
    {0x0003, "$0003 dispatch vector", 0, 0, true, 0},
    {0x043E, "$0422 table ID=0 handler (drive prep)", 0, 0, true, 0},
    {0x1000, "sector-0 entry (sub_0333h JP $1000)", 0, 0, false, 0x00},
    {0x1002, "sector0: EI", 0, 0, false, 0xFB},
    {0x1003, "sector0: LD DE,$1030", 0, 0, false, 0x11},
    {0x1006, "sector0: CALL $0003", 0, 0, false, 0xCD},
    {0x059C, "$0422 table ID=4 handler (real READ DATA)", 0, 0, true, 0},
    {0x4A00, "sector-0's own JP target - Phase 1 success", 0, 0, false, -1},
    {0x03E3, "ISR prologue #1", 0, 0, true, 0},
    {0x0752, "ISR prologue #2 (sets $FED5)", 0, 0, true, 0},
    {0x06C6, "busy-wait on $FED5", 0, 0, true, 0},
    {0xFEC9, "dynamic trampoline target", 0, 0, false, -1},
};
#define NUM_LANDMARKS (sizeof(landmarks) / sizeof(landmarks[0]))

/* Fill in `expected` for gate_from_rom landmarks from the just-loaded ROM.
 * Must run after p2500_load_rom() and before the disk is loaded, while
 * m.bank still selects the EPROM at $0000-$0FFF (true at reset). */
static void resolve_rom_landmarks(const P2500Machine *m) {
    for (size_t i = 0; i < NUM_LANDMARKS; i++) {
        if (landmarks[i].gate_from_rom)
            landmarks[i].expected = p2500_peek(m, landmarks[i].addr);
    }
}

static void check_landmarks(const P2500Machine *m, uint16_t pc, unsigned long step) {
    for (size_t i = 0; i < NUM_LANDMARKS; i++) {
        if (landmarks[i].addr != pc) continue;
        uint8_t byte = p2500_peek(m, pc);
        bool matches = landmarks[i].expected < 0 ? byte != 0x00
                                                  : byte == (uint8_t)landmarks[i].expected;
        if (!matches) {
            landmarks[i].gated_count++;
            continue;
        }
        landmarks[i].hit_count++;
        if (landmarks[i].hit_count <= 5)
            fprintf(stderr, "[step %lu] landmark $%04X (%s) hit #%d\n",
                    step, pc, landmarks[i].name, landmarks[i].hit_count);
    }
}

static uint8_t *read_whole_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

int main(int argc, char **argv) {
    const char *rom_path = "roms/ipl.bin";
    const char *disk_path = NULL;
    const char *sesam_path = NULL;
    const char *vram_dump_path = NULL;
    unsigned long max_steps = 2000000UL;
    bool verbose_io = false;
    /* --poke ADDR:HEXBYTES - directly seed RAM before running, e.g. to
     * test a synthetic request block the way ../roms/sesam_banner_test.bin
     * did for the SESAM mechanism. See README.md "Reaching READ DATA". */
    #define MAX_POKES 8
    struct { uint16_t addr; uint8_t bytes[64]; size_t len; } pokes[MAX_POKES];
    int num_pokes = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk_path = argv[++i];
        else if (!strcmp(argv[i], "--sesam") && i + 1 < argc) sesam_path = argv[++i];
        else if (!strcmp(argv[i], "--dump-vram") && i + 1 < argc) vram_dump_path = argv[++i];
        else if (!strcmp(argv[i], "--max-steps") && i + 1 < argc) max_steps = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--verbose-io")) verbose_io = true;
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) {
            if (num_pokes >= MAX_POKES) { fprintf(stderr, "too many --poke args\n"); return 1; }
            char *arg = argv[++i];
            char *colon = strchr(arg, ':');
            if (!colon) { fprintf(stderr, "bad --poke syntax, want ADDR:HEXBYTES\n"); return 1; }
            *colon = '\0';
            pokes[num_pokes].addr = (uint16_t)strtoul(arg, NULL, 16);
            const char *hex = colon + 1;
            size_t n = strlen(hex) / 2;
            for (size_t j = 0; j < n && j < sizeof(pokes[0].bytes); j++) {
                char byte_str[3] = {hex[j * 2], hex[j * 2 + 1], 0};
                pokes[num_pokes].bytes[j] = (uint8_t)strtoul(byte_str, NULL, 16);
            }
            pokes[num_pokes].len = n;
            num_pokes++;
        } else {
            fprintf(stderr, "unknown or malformed argument: %s\n", argv[i]);
            fprintf(stderr, "usage: %s [--rom path] [--disk path] [--sesam path] "
                            "[--dump-vram path] [--max-steps N] [--verbose-io] "
                            "[--poke ADDR:HEXBYTES ...]\n", argv[0]);
            return 1;
        }
    }

    P2500Machine m;
    p2500_init(&m);
    m.verbose_unknown_ports = verbose_io;

    if (!p2500_load_rom(&m, rom_path)) {
        fprintf(stderr, "failed to load ROM from %s\n", rom_path);
        return 1;
    }
    printf("Loaded ROM from %s\n", rom_path);
    resolve_rom_landmarks(&m);

    uint8_t *disk_buf = NULL;
    size_t disk_size = 0;
    if (disk_path) {
        disk_buf = read_whole_file(disk_path, &disk_size);
        if (!disk_buf) { fprintf(stderr, "failed to load disk image %s\n", disk_path); return 1; }
        m.fdc.disk = disk_buf;
        m.fdc.disk_size = disk_size;
        m.fdc.verbose = verbose_io;
        printf("Loaded disk image from %s (%zu bytes)\n", disk_path, disk_size);
    }
    m.pio.verbose = verbose_io;
    m.dma.verbose = verbose_io;

    uint8_t *sesam_buf = NULL;
    size_t sesam_size = 0;
    if (sesam_path) {
        sesam_buf = read_whole_file(sesam_path, &sesam_size);
        if (!sesam_buf) { fprintf(stderr, "failed to load SESAM stream %s\n", sesam_path); return 1; }
        m.sesam.stream = sesam_buf;
        m.sesam.stream_len = sesam_size;
        printf("Loaded SESAM stream from %s (%zu bytes)\n", sesam_path, sesam_size);
    }

    for (int p = 0; p < num_pokes; p++) {
        for (size_t j = 0; j < pokes[p].len; j++)
            m.ram[(uint16_t)(pokes[p].addr + j)] = pokes[p].bytes[j];
        printf("Poked %zu bytes at $%04X: ", pokes[p].len, pokes[p].addr);
        for (size_t j = 0; j < pokes[p].len; j++)
            printf("%02X ", m.ram[(uint16_t)(pokes[p].addr + j)]);
        printf("\n");
    }

    const unsigned STUCK_WINDOW = 200000;
    const unsigned CHECK_INTERVAL = 5000;
    const unsigned STUCK_DISTINCT_THRESHOLD = 8;
    uint16_t *ring = malloc(sizeof(uint16_t) * STUCK_WINDOW);
    unsigned ring_pos = 0;
    bool ring_full = false;
    static bool seen[65536];

    unsigned long step = 0;
    int reset_visits = 0;
    const char *stop_reason = NULL;

    uint8_t watch_addr3_last = p2500_peek(&m, 3);
    uint8_t watch_addr39_last = p2500_peek(&m, 0x39);
    for (; step < max_steps; step++) {
        uint16_t pc = m.cpu.pc;

        if (p2500_peek(&m, 3) != watch_addr3_last) {
            fprintf(stderr, "[step %lu] [$0003 watch] changed $%02X -> $%02X, PC=$%04X SP=$%04X "
                            "DE=$%02X%02X HL=$%02X%02X BC=$%02X%02X\n",
                    step, watch_addr3_last, p2500_peek(&m, 3), pc, m.cpu.sp,
                    m.cpu.d, m.cpu.e, m.cpu.h, m.cpu.l, m.cpu.b, m.cpu.c);
            watch_addr3_last = p2500_peek(&m, 3);
        }
        if (p2500_peek(&m, 0x39) != watch_addr39_last) {
            fprintf(stderr, "[step %lu] [$0039 watch] changed $%02X -> $%02X, PC=$%04X SP=$%04X "
                            "DE=$%02X%02X HL=$%02X%02X BC=$%02X%02X\n",
                    step, watch_addr39_last, p2500_peek(&m, 0x39), pc, m.cpu.sp,
                    m.cpu.d, m.cpu.e, m.cpu.h, m.cpu.l, m.cpu.b, m.cpu.c);
            watch_addr39_last = p2500_peek(&m, 0x39);
        }

        check_landmarks(&m, pc, step);

        if (getenv("P2500_TRACE_FROM") && getenv("P2500_TRACE_TO")) {
            unsigned long from = strtoul(getenv("P2500_TRACE_FROM"), NULL, 0);
            unsigned long to = strtoul(getenv("P2500_TRACE_TO"), NULL, 0);
            if (step >= from && step <= to)
                fprintf(stderr, "[trace %lu] PC=$%04X op=%02X %02X %02X SP=$%04X DE=$%02X%02X HL=$%02X%02X\n",
                        step, pc, p2500_peek(&m, pc), p2500_peek(&m, (uint16_t)(pc+1)),
                        p2500_peek(&m, (uint16_t)(pc+2)),
                        m.cpu.sp, m.cpu.d, m.cpu.e, m.cpu.h, m.cpu.l);
        }

        if (pc == 0x0000) {
            reset_visits++;
            if (reset_visits > 1) {
                stop_reason = "re-entered reset vector $0000";
                break;
            }
        }

        ring[ring_pos] = pc;
        ring_pos = (ring_pos + 1) % STUCK_WINDOW;
        if (ring_pos == 0) ring_full = true;

        if (ring_full && step % CHECK_INTERVAL == 0) {
            memset(seen, 0, sizeof(seen));
            unsigned distinct = 0;
            for (unsigned i = 0; i < STUCK_WINDOW; i++) {
                if (!seen[ring[i]]) { seen[ring[i]] = true; distinct++; }
            }
            if (distinct <= STUCK_DISTINCT_THRESHOLD) {
                stop_reason = "stuck: last window only touched a handful of distinct addresses";
                break;
            }
        }

        if (m.cpu.halted) {
            stop_reason = "HALT";
            break;
        }

        /* Note: superzazu/z80's z80_step() runs the current opcode then
         * calls process_interrupts() in the *same* call, so a z80_gen_int()
         * triggered from a port-out callback mid-instruction is delivered
         * before the next fetch - there's no separate "pending" step to
         * observe here, confirmed by reading z80.c directly (see
         * ../README.md's "Open finding" section). */
        p2500_step(&m);
    }
    if (step >= max_steps && !stop_reason) stop_reason = "hit max-steps";

    printf("\nStopped after %lu steps: %s\n", step, stop_reason);
    for (size_t i = 0; i < NUM_LANDMARKS; i++) {
        if (landmarks[i].gated_count > 0)
            printf("  (landmark $%04X (%s): %d hit(s) at this PC didn't match the "
                   "expected byte(s), not counted - see T8)\n",
                   landmarks[i].addr, landmarks[i].name, landmarks[i].gated_count);
    }
    printf("Final: PC=$%04X SP=$%04X A=$%02X BC=$%02X%02X DE=$%02X%02X HL=$%02X%02X "
           "I=$%02X IM=%d IFF1=%d halted=%d\n",
           m.cpu.pc, m.cpu.sp, m.cpu.a, m.cpu.b, m.cpu.c, m.cpu.d, m.cpu.e,
           m.cpu.h, m.cpu.l, m.cpu.i, m.cpu.interrupt_mode, m.cpu.iff1, m.cpu.halted);

    if (ring_full) {
        memset(seen, 0, sizeof(seen));
        unsigned distinct = 0;
        printf("\nDistinct addresses in the last %u steps: ", STUCK_WINDOW);
        for (unsigned i = 0; i < STUCK_WINDOW; i++) {
            if (!seen[ring[i]]) { seen[ring[i]] = true; distinct++; }
        }
        printf("%u\n", distinct);
        if (distinct <= 40) {
            memset(seen, 0, sizeof(seen));
            printf("Addresses: ");
            for (unsigned i = 0; i < STUCK_WINDOW; i++) {
                if (!seen[ring[i]]) { seen[ring[i]] = true; printf("$%04X ", ring[i]); }
            }
            printf("\n");
        }
    }

    if (verbose_io) {
        printf("\nRAM $FE00-$FEFF (IM2 template + device-descriptor scratch area):\n");
        for (int row = 0; row < 16; row++) {
            printf("  $FE%02X: ", row * 16);
            for (int col = 0; col < 16; col++)
                printf("%02X ", m.ram[0xFE00 + row * 16 + col]);
            printf("\n");
        }
    }
    printf("\nMemory $0000-$000F as the CPU would see it (sanity check - should be "
           "the ROM's own reset vector table whenever the EPROM is banked in): ");
    for (int i = 0; i < 16; i++) printf("%02X ", p2500_peek(&m, (uint16_t)i));
    printf("\n");

    if (vram_dump_path) {
        FILE *f = fopen(vram_dump_path, "wb");
        if (f) {
            fwrite(&m.ram[0x8000], 1, 0x4000, f);
            fclose(f);
            printf("Wrote video RAM ($8000-$BFFF) to %s\n", vram_dump_path);
        } else {
            fprintf(stderr, "failed to write vram dump to %s\n", vram_dump_path);
        }
    }

    free(ring);
    free(disk_buf);
    free(sesam_buf);
    return 0;
}
