#include "core/machine.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

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
            /* Only interesting while the landmark has never legitimately
               hit. Once it has, a mismatch just means the address has been
               reused - which is correct behaviour for $1000's boot sector
               once CP/M owns that memory, and was reported as a failure
               every run (TODO.md T25). */
            if (landmarks[i].hit_count == 0) landmarks[i].gated_count++;
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
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
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
    /* --type-after MS: emulated milliseconds to wait before the first
     * queued keystroke. See keyboard.h for why this is needed. */
    unsigned long type_after_ms = 4000;
    bool verbose_io = false;
    /* --type STRING - queues bytes to deliver one per port $06 (console
     * RX) read, simulating keystrokes. Supports \r \n \t \\ and \xHH.
     * CP/M wants CR (\r), not LF, to end a line. */
    #define MAX_TYPE_LEN 4096
    static uint8_t type_buf[MAX_TYPE_LEN];
    static unsigned long type_release[MAX_TYPE_LEN];
    size_t type_len = 0;
    /* --peek ADDR:LEN - hex-dump `len` bytes at `addr` (bank-aware, same
     * view the CPU has) at exit. For live debugging of whatever region a
     * stuck run's PC/DE/HL point at, when no static disassembly of that
     * region exists yet. */
    #define MAX_PEEKS 8
    struct { uint16_t addr; uint16_t len; } peeks[MAX_PEEKS];
    int num_peeks = 0;
    /* --poke ADDR:HEXBYTES - directly seed RAM before running, e.g. to
     * test a synthetic request block the way ../roms/sesam_banner_test.bin
     * did for the SESAM mechanism. See README.md "Reaching READ DATA". */
    #define MAX_POKES 8
    struct { uint16_t addr; uint8_t bytes[64]; size_t len; } pokes[MAX_POKES];
    int num_pokes = 0;
    /* --watch ADDR[:LEN] - report every change to that byte (or run of
       bytes) with the PC and registers that caused it (TODO.md T25).
       Defaults to $0003 and $0039 - the two page-zero corruption canaries
       this project has needed most often - when none is given. */
    #define MAX_WATCHES 16
    struct { uint16_t addr; uint16_t len; uint8_t last[64]; } watches[MAX_WATCHES];
    int num_watches = 0;
    /* --count ADDR - count executions at that PC and print the total at
       exit. "Does this handler ever actually run?" is the single question
       this project asks most (TODO.md T25). */
    #define MAX_COUNTS 16
    struct { uint16_t addr; unsigned long hits; unsigned long first_step; } counts[MAX_COUNTS];
    int num_counts = 0;
    /* --break ADDR - stop the run the first time PC reaches it, so the
       exit report's registers/peeks describe that exact moment. */
    #define MAX_BREAKS 8
    uint16_t breaks[MAX_BREAKS];
    int num_breaks = 0;
    const char *ram_dump_path = NULL;
    bool stuck_detect = true;
    /* --swap-at MS:PATH - change the disk in the drive partway through a
     * run, the way a person would. CP/M caches directory state, so the
     * guest must be told: type Ctrl-C at the prompt afterwards to force a
     * warm boot and re-read. Times are emulated milliseconds, like
     * --type-after. */
    #define MAX_SWAPS 4
    struct { unsigned long at_ms; const char *path; uint8_t *buf; size_t size; bool done; }
        swaps[MAX_SWAPS];
    int num_swaps = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk_path = argv[++i];
        else if (!strcmp(argv[i], "--sesam") && i + 1 < argc) sesam_path = argv[++i];
        else if (!strcmp(argv[i], "--dump-vram") && i + 1 < argc) vram_dump_path = argv[++i];
        else if (!strcmp(argv[i], "--max-steps") && i + 1 < argc) max_steps = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--verbose-io")) verbose_io = true;
        else if (!strcmp(argv[i], "--type-after") && i + 1 < argc)
            type_after_ms = strtoul(argv[++i], NULL, 0);
        else if ((!strcmp(argv[i], "--type") || !strcmp(argv[i], "--type-at")) && i + 1 < argc) {
            bool timed = !strcmp(argv[i], "--type-at");
            const char *s = argv[++i];
            unsigned long at_ms = 0;
            if (timed) {
                char *colon = strchr((char *)s, ':');
                if (!colon) { fprintf(stderr, "bad --type-at syntax, want MS:STRING\n"); return 1; }
                *colon = '\0';
                at_ms = strtoul(s, NULL, 0);
                s = colon + 1;
            }
            size_t seg_start = type_len;
            while (*s && type_len < MAX_TYPE_LEN) {
                if (*s == '\\' && s[1]) {
                    s++;
                    if (*s == 'r') { type_buf[type_len++] = '\r'; s++; }
                    else if (*s == 'n') { type_buf[type_len++] = '\n'; s++; }
                    else if (*s == 't') { type_buf[type_len++] = '\t'; s++; }
                    else if (*s == '\\') { type_buf[type_len++] = '\\'; s++; }
                    else if (*s == 'x' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
                        char hex[3] = {s[1], s[2], 0};
                        type_buf[type_len++] = (uint8_t)strtoul(hex, NULL, 16);
                        s += 3;
                    } else { type_buf[type_len++] = (uint8_t)*s; s++; }
                } else {
                    type_buf[type_len++] = (uint8_t)*s;
                    s++;
                }
            }
            /* --type uses the shared --type-after default, applied below;
               --type-at pins this segment to its own time. */
            for (size_t j = seg_start; j < type_len; j++)
                type_release[j] = timed ? at_ms : ULONG_MAX;
        }
        else if (!strcmp(argv[i], "--peek") && i + 1 < argc) {
            if (num_peeks >= MAX_PEEKS) { fprintf(stderr, "too many --peek args\n"); return 1; }
            char *arg = argv[++i];
            char *colon = strchr(arg, ':');
            if (!colon) { fprintf(stderr, "bad --peek syntax, want ADDR:LEN (hex addr)\n"); return 1; }
            *colon = '\0';
            peeks[num_peeks].addr = (uint16_t)strtoul(arg, NULL, 16);
            peeks[num_peeks].len = (uint16_t)strtoul(colon + 1, NULL, 0);
            num_peeks++;
        }
        else if (!strcmp(argv[i], "--dump-ram") && i + 1 < argc) ram_dump_path = argv[++i];
        else if (!strcmp(argv[i], "--swap-at") && i + 1 < argc) {
            if (num_swaps >= MAX_SWAPS) { fprintf(stderr, "too many --swap-at args\n"); return 1; }
            char *arg = argv[++i];
            char *colon = strchr(arg, ':');
            if (!colon) { fprintf(stderr, "bad --swap-at syntax, want MS:PATH\n"); return 1; }
            *colon = '\0';
            swaps[num_swaps].at_ms = strtoul(arg, NULL, 0);
            swaps[num_swaps].path = colon + 1;
            swaps[num_swaps].buf = NULL;
            swaps[num_swaps].size = 0;
            swaps[num_swaps].done = false;
            num_swaps++;
        }
        else if (!strcmp(argv[i], "--no-stuck-detect")) stuck_detect = false;
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) {
            if (num_watches >= MAX_WATCHES) { fprintf(stderr, "too many --watch args\n"); return 1; }
            char *arg = argv[++i];
            char *colon = strchr(arg, ':');
            if (colon) *colon = '\0';
            watches[num_watches].addr = (uint16_t)strtoul(arg, NULL, 16);
            watches[num_watches].len = colon ? (uint16_t)strtoul(colon + 1, NULL, 0) : 1;
            if (watches[num_watches].len < 1) watches[num_watches].len = 1;
            if (watches[num_watches].len > 64) watches[num_watches].len = 64;
            num_watches++;
        }
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) {
            if (num_counts >= MAX_COUNTS) { fprintf(stderr, "too many --count args\n"); return 1; }
            counts[num_counts].addr = (uint16_t)strtoul(argv[++i], NULL, 16);
            counts[num_counts].hits = 0;
            counts[num_counts].first_step = 0;
            num_counts++;
        }
        else if (!strcmp(argv[i], "--break") && i + 1 < argc) {
            if (num_breaks >= MAX_BREAKS) { fprintf(stderr, "too many --break args\n"); return 1; }
            breaks[num_breaks++] = (uint16_t)strtoul(argv[++i], NULL, 16);
        }
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
            fprintf(stderr, "usage: %s [--rom path] [--disk path] [--sesam path]\n"
                            "  [--dump-vram path] [--dump-ram path] [--max-steps N] [--verbose-io]\n"
                            "  [--peek ADDR:LEN ...] [--poke ADDR:HEXBYTES ...]\n"
                            "  [--watch ADDR[:LEN] ...] [--count ADDR ...] [--type STRING] [--type-after MS]\n"
                            "  [--type-at MS:STRING ...]\n"
                            "  [--break ADDR ...] [--swap-at MS:PATH ...] [--no-stuck-detect]\n", argv[0]);
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
    if (type_len > 0) {
        p2500_keyboard_init(&m.keyboard, type_buf, type_len);
        for (size_t j = 0; j < type_len; j++)
            type_release[j] = (type_release[j] == ULONG_MAX ? type_after_ms : type_release[j])
                              * (P2500_CPU_HZ / 1000u);
        m.keyboard.release_at = type_release;
        printf("Queued %zu bytes to type on port $06 (untimed segments start at %lu ms)\n",
               type_len, type_after_ms);
    }
    m.pio.verbose = verbose_io;
    m.dma.verbose = verbose_io;
    m.ctc.verbose = verbose_io;
    m.sesam.verbose = verbose_io;
    m.keyboard.verbose = verbose_io;
    m.serial.verbose = true; /* always show console TX - it's this project's only view of program output */

    for (int sw = 0; sw < num_swaps; sw++) {
        swaps[sw].buf = read_whole_file(swaps[sw].path, &swaps[sw].size);
        if (!swaps[sw].buf) {
            fprintf(stderr, "failed to load swap disk image %s\n", swaps[sw].path);
            return 1;
        }
        printf("Will swap in %s (%zu bytes) at %lu ms emulated\n",
               swaps[sw].path, swaps[sw].size, swaps[sw].at_ms);
    }

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
    /* TODO.md T25: counting *distinct* addresses alone misses cycles - the
     * ISSUE-5 deadlock walks 64 addresses and so ran the full 5 M steps
     * reporting nothing. So also look for a repeating *state* sequence.
     *
     * It has to be state, not PC: the IPL's own 64K RAM test ($016B) is an
     * 11-instruction loop whose PC sequence repeats perfectly for hundreds
     * of thousands of steps while making real progress - only its
     * registers say so. A loop that repeats PC *and* every visible
     * register, unbroken for CYCLE_CONFIRM steps, cannot be making
     * progress: nothing it could still be waiting on is being sampled into
     * a register. (A polling wait on a memory location an ISR writes -
     * exactly ISSUE-5's $EB02 - does repeat state, and is correctly
     * reported: the ISR that would break it is not running.) */
    const unsigned MAX_CYCLE_PERIOD = 2048;
    const unsigned CYCLE_CONFIRM = 20000;
    uint16_t *ring = malloc(sizeof(uint16_t) * STUCK_WINDOW);
    uint64_t *state_ring = malloc(sizeof(uint64_t) * STUCK_WINDOW);
    if (!ring || !state_ring) { fprintf(stderr, "out of memory allocating the PC ring\n"); return 1; }
    unsigned ring_pos = 0;
    bool ring_full = false;
    static bool seen[65536];
    unsigned detected_cycle = 0;

    unsigned long step = 0;
    int reset_visits = 0;
    const char *stop_reason = NULL;

    if (num_watches == 0) { /* the two default page-zero canaries */
        watches[num_watches].addr = 0x0003; watches[num_watches].len = 1; num_watches++;
        watches[num_watches].addr = 0x0039; watches[num_watches].len = 1; num_watches++;
    }
    for (int w = 0; w < num_watches; w++)
        for (uint16_t j = 0; j < watches[w].len; j++)
            watches[w].last[j] = p2500_peek(&m, (uint16_t)(watches[w].addr + j));

    for (; step < max_steps; step++) {
        uint16_t pc = m.cpu.pc;

        for (int w = 0; w < num_watches; w++) {
            for (uint16_t j = 0; j < watches[w].len; j++) {
                uint16_t a = (uint16_t)(watches[w].addr + j);
                uint8_t now = p2500_peek(&m, a);
                if (now == watches[w].last[j]) continue;
                fprintf(stderr, "[step %lu] [watch $%04X] $%02X -> $%02X, PC=$%04X SP=$%04X "
                                "A=$%02X BC=$%02X%02X DE=$%02X%02X HL=$%02X%02X\n",
                        step, a, watches[w].last[j], now, pc, m.cpu.sp, m.cpu.a,
                        m.cpu.b, m.cpu.c, m.cpu.d, m.cpu.e, m.cpu.h, m.cpu.l);
                watches[w].last[j] = now;
            }
        }
        for (int c = 0; c < num_counts; c++) {
            if (counts[c].addr != pc) continue;
            if (counts[c].hits == 0) counts[c].first_step = step;
            counts[c].hits++;
        }
        for (int b = 0; b < num_breaks; b++) {
            if (breaks[b] != pc) continue;
            stop_reason = "--break reached";
            break;
        }
        if (stop_reason) break;

        for (int sw = 0; sw < num_swaps; sw++) {
            if (swaps[sw].done) continue;
            if (m.cpu.cyc < swaps[sw].at_ms * (P2500_CPU_HZ / 1000u)) continue;
            m.fdc.disk = swaps[sw].buf;
            m.fdc.disk_size = swaps[sw].size;
            swaps[sw].done = true;
            fprintf(stderr, "[step %lu] disk swapped: now %s (%zu bytes). CP/M caches "
                            "directory state - send Ctrl-C at the prompt to re-read.\n",
                    step, swaps[sw].path, swaps[sw].size);
        }

        check_landmarks(&m, pc, step);

        if (getenv("P2500_TRACE_FROM") && getenv("P2500_TRACE_TO")) {
            unsigned long from = strtoul(getenv("P2500_TRACE_FROM"), NULL, 0);
            unsigned long to = strtoul(getenv("P2500_TRACE_TO"), NULL, 0);
            if (step >= from && step <= to)
                fprintf(stderr, "[trace %lu] PC=$%04X op=%02X %02X %02X SP=$%04X BC=$%02X%02X DE=$%02X%02X HL=$%02X%02X A=$%02X\n",
                        step, pc, p2500_peek(&m, pc), p2500_peek(&m, (uint16_t)(pc+1)),
                        p2500_peek(&m, (uint16_t)(pc+2)),
                        m.cpu.sp, m.cpu.b, m.cpu.c, m.cpu.d, m.cpu.e, m.cpu.h, m.cpu.l, m.cpu.a);
        }

        if (pc == 0x0000) {
            /* Re-entering $0000 means a runaway reset only while the IPL
             * still owns page zero, where $0000 is "JP $0100". Once CP/M is
             * up it is "JP $E203", the CBIOS warm-boot vector, and passing
             * through it is exactly what Ctrl-C at the prompt does - so
             * gate on the documented invariant rather than on the address.
             * See TODO.md's "Known-good invariants". */
            bool ipl_owns_page_zero = p2500_peek(&m, 0) == 0xC3 &&
                                      p2500_peek(&m, 1) == 0x00 &&
                                      p2500_peek(&m, 2) == 0x01;
            reset_visits++;
            if (reset_visits > 1 && ipl_owns_page_zero) {
                stop_reason = "re-entered reset vector $0000 while the IPL still owns page zero";
                break;
            }
        }

        ring[ring_pos] = pc;
        state_ring[ring_pos] =
            (((uint64_t)pc) |
             ((uint64_t)m.cpu.sp << 16) |
             ((uint64_t)m.cpu.a << 32) |
             ((uint64_t)((m.cpu.b << 8) | m.cpu.c) << 40)) ^
            (((uint64_t)((m.cpu.d << 8) | m.cpu.e) * 0x9E3779B97F4A7C15ULL) >> 24) ^
            (((uint64_t)((m.cpu.h << 8) | m.cpu.l) * 0xC2B2AE3D27D4EB4FULL) >> 8);
        ring_pos = (ring_pos + 1) % STUCK_WINDOW;
        if (ring_pos == 0) ring_full = true;

        if (stuck_detect && ring_full && step % CHECK_INTERVAL == 0) {
            memset(seen, 0, sizeof(seen));
            unsigned distinct = 0;
            for (unsigned i = 0; i < STUCK_WINDOW; i++) {
                if (!seen[ring[i]]) { seen[ring[i]] = true; distinct++; }
            }
            if (distinct <= STUCK_DISTINCT_THRESHOLD) {
                stop_reason = "stuck: last window only touched a handful of distinct addresses";
                break;
            }
            /* Is the newest CYCLE_CONFIRM-step suffix periodic with some
             * period p <= MAX_CYCLE_PERIOD? ring_pos is the next slot to
             * write, so the newest entry sits at ring_pos-1, walking
             * backwards modulo the ring. */
            for (unsigned per = 1; per <= MAX_CYCLE_PERIOD; per++) {
                bool match = true;
                for (unsigned i = 0; i + per < CYCLE_CONFIRM; i++) {
                    unsigned a = (ring_pos + STUCK_WINDOW - 1 - i) % STUCK_WINDOW;
                    unsigned b = (ring_pos + STUCK_WINDOW - 1 - i - per) % STUCK_WINDOW;
                    if (state_ring[a] != state_ring[b]) { match = false; break; }
                }
                if (match) { detected_cycle = per; break; }
            }
            if (detected_cycle) {
                stop_reason = "stuck: the PC is repeating a fixed instruction cycle";
                break;
            }
        }

        if (m.cpu.halted) {
            stop_reason = "HALT";
            break;
        }

        /* p2500_step() runs one instruction, advances every peripheral by
         * that instruction's T-states, and then offers the interrupt daisy
         * chain's winner to the CPU core - see machine.c and intctl.h. */
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
    if (detected_cycle)
        printf("  (repeating PC cycle of %u instructions detected)\n", detected_cycle);
    for (int c = 0; c < num_counts; c++) {
        if (counts[c].hits)
            printf("  --count $%04X: %lu execution(s), first at step %lu\n",
                   counts[c].addr, counts[c].hits, counts[c].first_step);
        else
            printf("  --count $%04X: never executed\n", counts[c].addr);
    }
    printf("Emulated time: %.3f s (%lu T-states at %u Hz)\n",
           (double)m.cpu.cyc / (double)P2500_CPU_HZ, m.cpu.cyc, P2500_CPU_HZ);
    if (m.sesam.reads || m.sesam.writes)
        printf("SESAM port $0F: %lu read(s), %lu write(s)\n", m.sesam.reads, m.sesam.writes);
    printf("Interrupts (daisy chain order, requests/acknowledged):\n ");
    for (int i = 0; i < P2500_INT_SOURCES; i++) {
        int src = p2500_intctl_chain_order[i];
        printf(" %s %lu/%lu%s", p2500_intctl_name(src), m.intctl.requests[src],
               m.intctl.acknowledged[src],
               m.intctl.under_service[src] ? " (in service)" : "");
    }
    printf("\n");
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

    for (int p = 0; p < num_peeks; p++) {
        printf("\n--peek $%04X:%u\n", peeks[p].addr, peeks[p].len);
        for (int row = 0; row * 16 < peeks[p].len; row++) {
            uint16_t base = (uint16_t)(peeks[p].addr + row * 16);
            printf("  $%04X: ", base);
            for (int col = 0; col < 16 && row * 16 + col < peeks[p].len; col++)
                printf("%02X ", p2500_peek(&m, (uint16_t)(base + col)));
            printf("\n");
        }
    }

    if (ram_dump_path) {
        FILE *f = fopen(ram_dump_path, "wb");
        if (f) {
            fwrite(m.ram, 1, P2500_RAM_SIZE, f);
            fclose(f);
            printf("Wrote the full 64K RAM image to %s\n", ram_dump_path);
        } else {
            fprintf(stderr, "failed to write RAM dump to %s\n", ram_dump_path);
        }
    }

    if (vram_dump_path) {
        FILE *f = fopen(vram_dump_path, "wb");
        if (f) {
            fwrite(m.vram, 1, P2500_VRAM_SIZE, f);
            fclose(f);
            printf("Wrote video RAM ($8000-$BFFF) to %s\n", vram_dump_path);
        } else {
            fprintf(stderr, "failed to write vram dump to %s\n", vram_dump_path);
        }
    }

    for (int sw = 0; sw < num_swaps; sw++) free(swaps[sw].buf);
    free(ring);
    free(state_ring);
    free(disk_buf);
    free(sesam_buf);
    return 0;
}
