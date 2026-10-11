/* WRITE DATA, FORMAT TRACK, and the write-protect tab.
 *
 * The end-to-end proof that writing works is in tests/run_tests.sh section
 * 23, where CP/M's own SAVE and PIP write files onto a disk image and a
 * second boot reads them back. What that cannot reach is everything the
 * firmware never does: a write that outlives its track, a write into a DMA
 * programmed the wrong way round, FORMAT TRACK at all (nothing in the
 * surviving corpus formats a disk - CONFIG and CPYDSK live on the
 * double-stepped half dumps), and the write-protect path, which no guest
 * program can tell apart from a media fault.
 *
 * So this links libp2500.a directly, exactly as tests/fdc_eot.c does and
 * for the same reason. Built by `make tests/fdc_write`.
 *
 * The one-line-per-claim output is what run_tests.sh replays through its
 * own pass/fail counters - keep the two-space "  PASS  " prefix. */

#include "core/dma.h"
#include "core/fdc.h"

#include <stdio.h>
#include <string.h>

#define SECTORS P2500_FDC_SECTORS_PER_TRACK
#define SECSIZE P2500_FDC_SECTOR_SIZE
#define TRACK (SECTORS * SECSIZE)
#define TRACKS 4
#define MEDIA (TRACKS * TRACK)

#define RAM_BASE 0x1000
#define VIRGIN 0xE5      /* what CP/M's own formatter leaves behind */
#define PAYLOAD 0x5A

static int failures = 0;

static void ok(int cond, const char *what) {
    printf("  %s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

static void okf(int cond, const char *what, long got, long want) {
    printf("  %s  %s (got %ld, expected %ld)\n", cond ? "PASS" : "FAIL", what, got, want);
    if (!cond) failures++;
}

/* The driver's own register stream, byte for byte as it was traced off a
 * `SAVE 1 ZZ.COM` at the CCP prompt, with the block length and Port B
 * address substituted. The ONLY difference from the read stream in
 * tests/fdc_eot.c is WR0's D2: $69 instead of $6D. Keeping the real stream
 * rather than poking struct fields makes this a test of the decoder too. */
static void program_dma(P2500Dma *dma, uint16_t block_len, uint16_t ram_addr, bool write) {
    const uint16_t raw = (uint16_t)(block_len - 1);
    const uint8_t stream[] = {
        (uint8_t)(write ? 0x69 : 0x6D),           /* WR0: direction, Port A addr lo + block length follow */
        0x15,                                     /* Port A = the FDC's data port */
        (uint8_t)(raw & 0xFF),
        (uint8_t)(raw >> 8),
        0x2C, 0xA3,                               /* WR1 + its timing byte */
        0x10, 0xA3,                               /* WR2 + its timing byte */
        0x9D,                                     /* WR4: byte mode, Port B addr + interrupt control follow */
        (uint8_t)(ram_addr & 0xFF),
        (uint8_t)(ram_addr >> 8),
        0x12, 0x00,                               /* interrupt control + vector */
        0x8A,                                     /* WR5 */
        0xCF,                                     /* LOAD */
        0x87,                                     /* ENABLE DMA (interrupts deliberately left off) */
    };
    p2500_dma_init(dma);
    for (size_t i = 0; i < sizeof stream; i++) p2500_dma_write(dma, stream[i]);
}

/* Issues WRITE DATA and drains the result phase. Byte for byte the command
 * the driver sends, opcode included. */
static void write_data(P2500Fdc *fdc, uint8_t c, uint8_t r, uint8_t eot, uint8_t res[7]) {
    const uint8_t cmd[9] = {0x05, 0x00, c, 0x00, r, 0x01, eot, 0x0E, 0x00};
    for (size_t i = 0; i < sizeof cmd; i++) p2500_fdc_write_data(fdc, cmd[i]);
    for (int i = 0; i < 7; i++) res[i] = p2500_fdc_read_data(fdc);
}

static uint8_t sense_drive_status(P2500Fdc *fdc, uint8_t unit) {
    p2500_fdc_write_data(fdc, 0x04);
    p2500_fdc_write_data(fdc, unit);
    return p2500_fdc_read_data(fdc);
}

/* FORMAT TRACK: SC sector IDs live in RAM as C,H,R,N quadruples, which the
 * DMA hands over exactly as it hands over a write's data. */
static void format_track(P2500Fdc *fdc, P2500Dma *dma, uint8_t *ram,
                         uint8_t c, uint8_t sc, uint8_t filler, uint8_t res[7]) {
    for (unsigned s = 0; s < sc; s++) {
        ram[RAM_BASE + s * 4 + 0] = c;
        ram[RAM_BASE + s * 4 + 1] = 0;
        ram[RAM_BASE + s * 4 + 2] = (uint8_t)(s + 1);
        ram[RAM_BASE + s * 4 + 3] = 1;
    }
    program_dma(dma, (uint16_t)(sc * 4), RAM_BASE, true);
    const uint8_t cmd[6] = {0x0D, 0x00, 0x01, sc, 0x0E, filler};
    for (size_t i = 0; i < sizeof cmd; i++) p2500_fdc_write_data(fdc, cmd[i]);
    for (int i = 0; i < 7; i++) res[i] = p2500_fdc_read_data(fdc);
}

/* How many leading bytes at `off` carry `byte`. */
static size_t run_of(const uint8_t *media, size_t off, uint8_t byte) {
    size_t n = 0;
    while (off + n < MEDIA && media[off + n] == byte) n++;
    return n;
}

int main(void) {
    static uint8_t media[MEDIA];
    static uint8_t ram[0x10000];
    uint8_t res[7];

    P2500Dma dma;
    P2500Fdc fdc;
    p2500_dma_init(&dma);
    p2500_fdc_init(&fdc, NULL, 0);
    fdc.ram = ram;
    fdc.dma = &dma;

    /* 1. One sector, the shape every write the firmware issues takes:
     *    unit 0, C=3, R=1, EOT=$10, one sector of block length. */
    memset(media, VIRGIN, sizeof media);
    memset(ram, PAYLOAD, sizeof ram);
    p2500_fdc_attach_writable(&fdc, 0, media, sizeof media);
    program_dma(&dma, SECSIZE, RAM_BASE, true);
    write_data(&fdc, 3, 1, SECTORS, res);
    printf("== a one-sector write, which is the shape the firmware issues\n");
    /* C=3 is physical track 2, so the sector lands at 2*4096. */
    okf(run_of(media, 2 * TRACK, PAYLOAD) == SECSIZE, "exactly one sector of payload lands",
        (long)run_of(media, 2 * TRACK, PAYLOAD), (long)SECSIZE);
    ok(media[2 * TRACK - 1] == VIRGIN, "the sector before it is untouched");
    ok(media[2 * TRACK + SECSIZE] == VIRGIN, "the sector after it is untouched");
    okf(res[0] == 0x00 && res[1] == 0x00, "terminates normally", res[0], 0x00);
    okf(res[3] == 3 && res[5] == 2, "cylinder stands and R advances to 2", res[3] * 256 + res[5], 3 * 256 + 2);
    okf((long)fdc.sectors_written[0], "the write is counted", (long)fdc.sectors_written[0], 1);

    /* 2. A write that outlives its track must stop at EOT, the same bound
     *    reads are held to (T31) - and the track after it must survive. */
    memset(media, VIRGIN, sizeof media);
    fdc.sectors_written[0] = 0;
    program_dma(&dma, 2 * TRACK, RAM_BASE, true);
    write_data(&fdc, 1, 1, SECTORS, res);
    printf("== a write that outlives its track stops at EOT\n");
    okf(run_of(media, 0, PAYLOAD) == TRACK, "exactly one track is written",
        (long)run_of(media, 0, PAYLOAD), (long)TRACK);
    ok(media[TRACK] == VIRGIN, "the next track is NOT overwritten behind it");
    okf(res[0] == 0x40, "ST0 reports abnormal termination", res[0], 0x40);
    okf(res[1] == 0x80, "ST1 reports EN, end of cylinder", res[1], 0x80);
    okf((long)fdc.sectors_written[0] == SECTORS, "one track's worth is counted",
        (long)fdc.sectors_written[0], (long)SECTORS);

    /* 3. The write-protect tab: the same media, mounted read-only. Nothing
     *    may change, and the controller must say why. */
    memset(media, VIRGIN, sizeof media);
    fdc.sectors_written[0] = 0;
    p2500_fdc_attach(&fdc, 0, media, sizeof media);
    program_dma(&dma, SECSIZE, RAM_BASE, true);
    write_data(&fdc, 3, 1, SECTORS, res);
    printf("== write-protected media refuses the write and says so\n");
    okf(run_of(media, 0, VIRGIN) == MEDIA, "not one byte of the image changes",
        (long)run_of(media, 0, VIRGIN), (long)MEDIA);
    okf(res[0] == 0x40, "ST0 reports abnormal termination", res[0], 0x40);
    okf(res[1] == 0x02, "ST1 reports NW, not writable", res[1], 0x02);
    okf((long)fdc.sectors_written[0] == 0, "nothing is counted as written",
        (long)fdc.sectors_written[0], 0);
    okf((sense_drive_status(&fdc, 0) & 0x40) == 0x40,
        "SENSE DRIVE STATUS sets ST3's WP bit", sense_drive_status(&fdc, 0) & 0x40, 0x40);

    /* The same check the other way round, so a model that always reported
     *    WP would fail here rather than passing both. */
    p2500_fdc_attach_writable(&fdc, 0, media, sizeof media);
    okf((sense_drive_status(&fdc, 0) & 0x40) == 0x00,
        "and clears it again for writable media", sense_drive_status(&fdc, 0) & 0x40, 0x00);

    /* 4. An empty drive. A write to it is the datasheet's not-ready case,
     *    exactly as a read is. */
    p2500_fdc_attach(&fdc, 0, NULL, 0);
    program_dma(&dma, SECSIZE, RAM_BASE, true);
    write_data(&fdc, 3, 1, SECTORS, res);
    printf("== a write to an empty drive reports not ready\n");
    okf(res[0] == 0x48, "ST0 carries IC=01 and NR", res[0], 0x48);

    /* 5. The bug this whole task was about: a DMA programmed for a READ
     *    cannot serve a WRITE. Nothing may move, and the result must not
     *    read as a successful transfer of zero bytes - which is what the
     *    code did before writes existed, and is the silent drop that hid
     *    the read path's missing DMA for a whole phase. */
    memset(media, VIRGIN, sizeof media);
    p2500_fdc_attach_writable(&fdc, 0, media, sizeof media);
    program_dma(&dma, SECSIZE, RAM_BASE, false); /* IO->memory: the wrong way */
    write_data(&fdc, 3, 1, SECTORS, res);
    printf("== a write with the DMA programmed for a read moves nothing, and reports it\n");
    okf(run_of(media, 0, VIRGIN) == MEDIA, "not one byte of the image changes",
        (long)run_of(media, 0, VIRGIN), (long)MEDIA);
    okf(res[0] == 0x40, "ST0 reports abnormal termination", res[0], 0x40);
    okf(res[1] == 0x10, "ST1 reports OR, overrun", res[1], 0x10);

    /* 6. A silent replay (core/history.c) must take the bytes off the DMA
     *    and raise the same result, but not write the disk a second time. */
    memset(media, VIRGIN, sizeof media);
    fdc.sectors_written[0] = 0;
    fdc.suppress_writes = true;
    program_dma(&dma, SECSIZE, RAM_BASE, true);
    write_data(&fdc, 3, 1, SECTORS, res);
    fdc.suppress_writes = false;
    printf("== a replayed write is staged and dropped, not applied twice\n");
    okf(run_of(media, 0, VIRGIN) == MEDIA, "the image is untouched",
        (long)run_of(media, 0, VIRGIN), (long)MEDIA);
    okf(res[0] == 0x00 && res[1] == 0x00, "but the result phase is the same as a real write",
        res[0] * 256 + res[1], 0x0000);
    okf((long)fdc.sectors_written[0] == 0, "and the sector is not double-counted",
        (long)fdc.sectors_written[0], 0);

    /* 7. FORMAT TRACK. Datasheet-derived, exercised by nothing else: a
     *    whole track of the filler byte, on the sectors the ID list names. */
    memset(media, PAYLOAD, sizeof media);
    fdc.sectors_written[0] = 0;
    format_track(&fdc, &dma, ram, 2, SECTORS, VIRGIN, res);
    printf("== FORMAT TRACK fills the track its ID list names\n");
    okf(run_of(media, TRACK, VIRGIN) == TRACK, "the whole track becomes the filler byte",
        (long)run_of(media, TRACK, VIRGIN), (long)TRACK);
    ok(media[TRACK - 1] == PAYLOAD, "the track before it is untouched");
    ok(media[2 * TRACK] == PAYLOAD, "the track after it is untouched");
    okf(res[0] == 0x00, "terminates normally", res[0], 0x00);
    okf((long)fdc.sectors_written[0] == SECTORS, "every sector is counted",
        (long)fdc.sectors_written[0], (long)SECTORS);

    /* 8. A format whose ID list points off the media reports no data
     *    rather than writing past the buffer. */
    memset(media, PAYLOAD, sizeof media);
    format_track(&fdc, &dma, ram, TRACKS + 2, SECTORS, VIRGIN, res);
    printf("== FORMAT TRACK refuses an ID list that is not on the media\n");
    okf(res[0] == 0x40 && res[1] == 0x04, "ST1 reports ND, no data", res[1], 0x04);
    okf(run_of(media, 0, PAYLOAD) == MEDIA, "and the image is untouched",
        (long)run_of(media, 0, PAYLOAD), (long)MEDIA);

    /* 9. And the tab stops a format exactly as it stops a write. */
    memset(media, PAYLOAD, sizeof media);
    p2500_fdc_attach(&fdc, 0, media, sizeof media);
    format_track(&fdc, &dma, ram, 2, SECTORS, VIRGIN, res);
    printf("== FORMAT TRACK on write-protected media refuses too\n");
    okf(res[0] == 0x40 && res[1] == 0x02, "ST1 reports NW, not writable", res[1], 0x02);
    okf(run_of(media, 0, PAYLOAD) == MEDIA, "and the image is untouched",
        (long)run_of(media, 0, PAYLOAD), (long)MEDIA);

    if (failures) {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll write checks passed.\n");
    return 0;
}
