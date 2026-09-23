#ifndef P2500_DMA_H
#define P2500_DMA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Z80A-DMA (Z8410) model for port $16 - the FDD card's data path between
 * the uPD765 (fixed I/O address $15) and RAM. See ../TODO.md
 * "2. Port $16 is the Z80A-DMA" for the evidence this decode is built on.
 *
 * The IPL only ever programs the DMA with one 20-byte register-load
 * template (copied from ROM $0B74, patched at three fields, then OTIR'd to
 * this port) - never the fully general WR0-WR6 byte stream the real chip
 * accepts in principle. This model tracks byte position within that fixed
 * template and decodes exactly the fields TODO.md cross-validated against
 * the RAM scratch area the ROM patches before sending (the offsets line
 * up 1:1 with $FEB4+n), rather than a byte-exact general decode of the
 * Z8410's WR0/WR1/WR2/WR4/WR5 bit fields - TODO.md T5 flags that fuller
 * decode as a stretch goal, not a blocker, since nothing in this firmware
 * exercises any other register sequence.
 *
 * WR6 command bytes ARE decoded by exact value - cross-checked against
 * both the IPL (sends $83 x6 to disable) and CP/M's SYSPBI.PHI (sends $C3
 * x6 then $AF). Recognizing them only at template position 0 (a fresh,
 * standalone command burst) or position >=17 (the trailer of the 20-byte
 * template) avoids misreading the direction byte at position 2 - $CF also
 * happens to be the WRITE-direction template's byte there.
 */

typedef enum {
    P2500_DMA_DIR_UNKNOWN,
    P2500_DMA_DIR_IO_TO_MEMORY,  /* READ DATA: FDC ($15) -> RAM (Port B) */
    P2500_DMA_DIR_MEMORY_TO_IO,  /* WRITE DATA: RAM (Port B) -> FDC ($15) */
} P2500DmaDirection;

typedef void (*P2500DmaInterruptCallback)(void *userdata, uint8_t vector);

typedef struct {
    uint8_t template_bytes[20];
    int pos;

    P2500DmaDirection direction;
    uint16_t block_length;   /* byte count, already +1'd from the wire value */
    uint16_t port_b_addr;    /* RAM buffer address */
    uint8_t vector;          /* end-of-block interrupt vector */

    bool loaded;             /* WR6 $CF (Load) seen since last reset */
    bool interrupts_enabled; /* WR6 $AB seen, cleared by $AF */
    bool dma_enabled;        /* WR6 $87 seen, cleared by $83 */

    P2500DmaInterruptCallback on_interrupt;
    void *interrupt_userdata;
    bool verbose;
} P2500Dma;

void p2500_dma_init(P2500Dma *dma);
void p2500_dma_write(P2500Dma *dma, uint8_t value); /* port $16 */

/* Called by fdc.c when a READ DATA command completes: copies up to the
 * DMA's own programmed block_length bytes from `src` into RAM at the
 * DMA's programmed Port B address (clamped to not run off the end of a
 * 64K RAM image), then fires the end-of-block interrupt on the DMA's own
 * vector if interrupts are enabled. No-op if the DMA hasn't been enabled
 * for an IO->memory transfer. */
void p2500_dma_deliver(P2500Dma *dma, uint8_t *ram, const uint8_t *src, size_t len);

#endif
