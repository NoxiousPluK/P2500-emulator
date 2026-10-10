/* A multi-sector read is bounded by EOT, not by the end of the disk image.
 *
 * Nothing in the surviving software corpus issues a read that outlives its
 * track - every READ DATA this project has traced carries EOT=$10 with a
 * DMA block length of either one sector or one whole track, so the bound
 * is never reached and the suite's boot tests cannot see it. This test
 * reaches it on purpose.
 *
 * It drives the core directly rather than through p2500-emu: the only way
 * to ask for more than a track holds is to program the DMA for it, and no
 * guest program does. That is also a standing check on the property the
 * Makefile claims for src/core/ - no global state, embeddable on its own -
 * since this is the first embedding that is neither the CLI nor the GUI.
 *
 * Built by `make tests/fdc_eot` and run from tests/run_tests.sh. */

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
#define SENTINEL 0xA5

static int failures = 0;

static void ok(int cond, const char *what) {
    printf("  %s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

static void okf(int cond, const char *what, long got, long want) {
    printf("  %s  %s (got %ld, expected %ld)\n", cond ? "PASS" : "FAIL", what, got, want);
    if (!cond) failures++;
}

/* The IPL's own register stream, byte for byte as the ROM's template at
 * $FEB3 sends it (traced with --verbose-io on a real boot), with the block
 * length and Port B address substituted. Using the real stream rather than
 * poking struct fields keeps this a test of the decoder too. */
static void program_dma(P2500Dma *dma, uint16_t block_len, uint16_t ram_addr) {
    /* The wire value is the count minus one, as the Z80 DMA defines it -
     * the IPL programs $0FFF for its 4096-byte track read. */
    const uint16_t raw = (uint16_t)(block_len - 1);
    const uint8_t stream[] = {
        0x6D,                                     /* WR0: IO->memory, Port A addr lo + block length follow */
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

/* Issues READ DATA and drains the result phase into `res`. */
static void read_data(P2500Fdc *fdc, uint8_t c, uint8_t r, uint8_t eot, uint8_t res[7]) {
    const uint8_t cmd[9] = {0x06, 0x00, c, 0x00, r, 0x01, eot, 0x0E, 0x00};
    for (size_t i = 0; i < sizeof cmd; i++) p2500_fdc_write_data(fdc, cmd[i]);
    for (int i = 0; i < 7; i++) res[i] = p2500_fdc_read_data(fdc);
}

/* Every sector carries its own LBA in every byte, so data landing at the
 * wrong offset is visible rather than merely plausible. */
static void fill_media(uint8_t *media) {
    for (size_t lba = 0; lba < TRACKS * SECTORS; lba++)
        memset(media + lba * SECSIZE, (uint8_t)(lba & 0xFF), SECSIZE);
}

/* How many leading bytes of RAM at RAM_BASE match the media from `off`. */
static size_t matching_prefix(const uint8_t *ram, const uint8_t *media, size_t off) {
    size_t n = 0;
    while (off + n < MEDIA && ram[RAM_BASE + n] == media[off + n]) n++;
    return n;
}

int main(void) {
    static uint8_t media[MEDIA];
    static uint8_t ram[0x10000];
    uint8_t res[7];
    fill_media(media);

    P2500Dma dma;
    P2500Fdc fdc;
    p2500_dma_init(&dma);
    p2500_fdc_init(&fdc, media, sizeof media);
    fdc.ram = ram;
    fdc.dma = &dma;

    /* 1. The boot case: the requester wants exactly the track. This is the
     *    shape of every read the firmware actually issues, so it is the
     *    regression guard on the bound not having become too tight. */
    memset(ram, SENTINEL, sizeof ram);
    program_dma(&dma, TRACK, RAM_BASE);
    read_data(&fdc, 1, 1, SECTORS, res);
    printf("== an exact-fit read: one whole track, which is what the firmware asks for\n");
    okf(matching_prefix(ram, media, 0) == TRACK, "the whole track lands",
        (long)matching_prefix(ram, media, 0), (long)TRACK);
    ok(ram[RAM_BASE + TRACK] == SENTINEL, "nothing is written past it");
    okf(res[0] == 0x00 && res[1] == 0x00, "terminates normally", res[0], 0x00);
    okf(res[3] == 2 && res[5] == 1, "result ID is the next cylinder, sector 1", res[3] * 256 + res[5], 2 * 256 + 1);

    /* 2. The bug T31 fixed: ask for two tracks in one command. The read
     *    must stop at EOT and say why, not run on into cylinder 2. */
    memset(ram, SENTINEL, sizeof ram);
    program_dma(&dma, 2 * TRACK, RAM_BASE);
    read_data(&fdc, 1, 1, SECTORS, res);
    printf("== a read that outlives its track stops at EOT\n");
    okf(matching_prefix(ram, media, 0) == TRACK, "exactly one track lands",
        (long)matching_prefix(ram, media, 0), (long)TRACK);
    ok(ram[RAM_BASE + TRACK] == SENTINEL, "the next track is NOT read in behind it");
    okf(res[0] == 0x40, "ST0 reports abnormal termination", res[0], 0x40);
    okf(res[1] == 0x80, "ST1 reports EN, end of cylinder", res[1], 0x80);
    okf(res[3] == 2 && res[5] == 1, "result ID is the next cylinder, sector 1", res[3] * 256 + res[5], 2 * 256 + 1);

    /* 3. A short read, well inside the track: the DMA's own count ends it,
     *    so this is a normal termination and R advances by the sectors
     *    moved. Discriminating: if the bound were applied as a hard
     *    truncation to EOT it would still pass 1 and 2 but not this. */
    memset(ram, SENTINEL, sizeof ram);
    program_dma(&dma, 4 * SECSIZE, RAM_BASE);
    read_data(&fdc, 1, 1, SECTORS, res);
    printf("== a read the DMA's count ends inside the track terminates normally\n");
    okf(matching_prefix(ram, media, 0) == 4 * SECSIZE, "four sectors land",
        (long)matching_prefix(ram, media, 0), (long)(4 * SECSIZE));
    okf(res[0] == 0x00 && res[1] == 0x00, "terminates normally", res[0], 0x00);
    okf(res[3] == 1 && res[5] == 5, "cylinder stands and R advances to 5", res[3] * 256 + res[5], 1 * 256 + 5);

    /* 4. Starting late in the track, where only two sectors remain. The
     *    data must come from those two sectors - not from sector 1, and
     *    not from the next cylinder. */
    memset(ram, SENTINEL, sizeof ram);
    program_dma(&dma, TRACK, RAM_BASE);
    read_data(&fdc, 1, SECTORS - 1, SECTORS, res);
    printf("== a read starting two sectors from EOT gets two sectors\n");
    okf(matching_prefix(ram, media, (SECTORS - 2) * SECSIZE) == 2 * SECSIZE,
        "both remaining sectors land, from the right offset",
        (long)matching_prefix(ram, media, (SECTORS - 2) * SECSIZE), (long)(2 * SECSIZE));
    ok(ram[RAM_BASE] == (uint8_t)(SECTORS - 2), "the first byte is sector 15's, not sector 1's");
    ok(ram[RAM_BASE + 2 * SECSIZE] == SENTINEL, "nothing follows from cylinder 2");
    okf(res[0] == 0x40 && res[1] == 0x80, "end of cylinder again", res[1], 0x80);

    /* 5. EOT below R: no sectors in range at all. A driver bug, and it
     *    must not transfer anything. */
    memset(ram, SENTINEL, sizeof ram);
    program_dma(&dma, TRACK, RAM_BASE);
    read_data(&fdc, 1, 8, 4, res);
    printf("== EOT below R transfers nothing\n");
    ok(ram[RAM_BASE] == SENTINEL, "RAM is untouched");
    okf(res[0] == 0x40 && res[1] == 0x80, "reported as end of cylinder", res[1], 0x80);

    /* 6. A track the image does not carry: the last cylinder of a 4-track
     *    image, read with the image one sector short. Reports sector not
     *    found rather than reading off the end of the buffer. */
    memset(ram, SENTINEL, sizeof ram);
    p2500_fdc_attach(&fdc, 0, media, MEDIA - SECSIZE);
    program_dma(&dma, TRACK, RAM_BASE);
    read_data(&fdc, TRACKS, 1, SECTORS, res);
    printf("== a track running past the end of the image reports no data\n");
    okf(matching_prefix(ram, media, (TRACKS - 1) * TRACK) == TRACK - SECSIZE,
        "everything the image does carry lands",
        (long)matching_prefix(ram, media, (TRACKS - 1) * TRACK), (long)(TRACK - SECSIZE));
    ok(ram[RAM_BASE + TRACK - SECSIZE] == SENTINEL, "and not a byte more");
    okf(res[0] == 0x40 && res[1] == 0x04, "ST1 reports ND, sector not found", res[1], 0x04);

    if (failures) {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll EOT checks passed.\n");
    return 0;
}
