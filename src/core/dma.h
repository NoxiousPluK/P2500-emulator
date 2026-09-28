#ifndef P2500_DMA_H
#define P2500_DMA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Z80A-DMA (Z8410) model for port $16 - the FDD card's data path between
 * the uPD765 (fixed I/O address $15) and RAM.
 *
 * An earlier version of this model assumed the IPL always sends one fixed
 * 20-byte register-load template at fixed byte positions. That assumption
 * broke on the READ DATA path (TODO.md ISSUE-2), which legitimately sends
 * a shorter, different stream - and reading the real datasheet (see
 * below) showed why: the Z80-DMA's register-load protocol was never
 * positional to begin with. Every byte the chip receives at the "top
 * level" (i.e. not already expected as a follow-up data byte for a
 * register group already in progress) is self-describing: specific bits
 * in that byte identify which of the seven write-register groups
 * (WR0-WR6) it belongs to, and other bits in that same byte say how many
 * of the following bytes carry more data for that group, and in what
 * order. This lets real firmware send short, incremental "only
 * reprogram what changed" streams instead of always resending
 * everything - an intentional, documented Z80-DMA feature ("Next-
 * operation loading without disturbing current operations", per the
 * Zilog Component Data Book's product summary for the Z8410).
 *
 * This model implements that real, general, self-describing protocol -
 * not a position-based template - based on:
 *   "Zilog Z80 Family CPU Peripherals User Manual" (UM008101-0601),
 *   chapter "Direct Memory Access", section "Write Registers"
 *   (Figures 39-46, manual pp. 92-107) - a clean, modern, typeset
 *   document. Trusted here, unlike an earlier OCR'd scan this project
 *   had already flagged as unreliable for this same chip (see TODO.md
 *   T5's original caveat about the WR0 bit layout).
 * Every group's identification bits and follow-byte rules below were
 * additionally cross-checked, byte by byte, against two independent,
 * real, captured streams from this ROM (see TODO.md ISSUE-2) and matched
 * exactly - including two standalone WR6 "RESET AND DISABLE INTERRUPTS"
 * ($A3) commands sent *in the middle* of the register stream, which only
 * makes sense under a self-describing reading, never a fixed template.
 *
 * Group identification (checked in this order in p2500_dma_write's
 * P2500_DMA_NEXT_BASE state - WR6 first since it's a simple, disjoint
 * bit pattern; the two D7=0 groups next; the D7=1 groups last):
 *   WR6: D7,D1,D0 = 1,1,1  (16 single-byte commands, D6-D2 select which -
 *        already fully validated independently, see execute_command)
 *   WR0: D7=0, D1D0 in {01,10,11} (not 00)      - class of operation
 *   WR1: D7,D2,D1,D0 = 0,1,0,0                   - Port A device/timing
 *   WR2: D7,D2,D1,D0 = 0,0,0,0                   - Port B device/timing
 *   WR3: D7,D1,D0 = 1,0,0                        - match/mask, fast enables
 *        This project first decoded WR3 as D7=0, which made the branch
 *        dead code (every such byte is claimed by WR1 or WR2 above it) and
 *        was blamed on an irreducible ambiguity in the manual. It is not
 *        ambiguous, it is an erratum: UM008101's Figure 43 prints D7=0, but
 *        the Zilog Component Data Book (1985) WR3 bit map - the figure
 *        carrying DMA ENABLE / INTERRUPT ENABLE / STOP ON MATCH - shows
 *        D7=1, which is the only value that decodes at all against WR1/WR2
 *        and is what every other Z80-DMA implementation uses. See TODO.md
 *        T21. WR3's D6 is DMA Enable and D5 is Interrupt Enable, the
 *        documented one-byte alternative to WR6 $87/$AB.
 *   WR4: D7,D1,D0 = 1,0,1                        - mode, Port B addr, ints
 *   WR5: D7,D6,D2,D1,D0 = 1,0,0,1,0               - Ready/CE/EOB behavior
 *
 * Follow-byte rules (each base byte's own bits independently say which
 * of its group's optional data bytes come next, always in the fixed
 * order listed below - this is what replaces the old fixed-position
 * template):
 *   WR0: Port A addr low (D3), Port A addr high (D4),
 *        block length low (D5), block length high (D6)
 *   WR1/WR2: variable-timing byte (D6)
 *   WR3: mask byte (D3), match byte (D4)
 *   WR4: Port B addr low + high (D2 AND D3 together), then interrupt
 *        control byte (D4) - which, when IT arrives, is itself decoded
 *        the same way: pulse control byte follows if ITS D2 AND D3 are
 *        both set, then the interrupt vector follows if ITS D4 is set.
 *   WR5, WR6: no follow bytes (pure data byte / immediate command)
 *
 * Not implemented: WR3's mask/match bytes and WR4's pulse-control byte
 * are correctly *parsed* (consumed in the right position, so the stream
 * never desyncs if the ROM ever sends them) but their values are
 * discarded - this model has no search-mode or pulse-counting behavior
 * to feed them into, since nothing this project has traced ever uses
 * either feature.
 */

typedef enum {
    P2500_DMA_DIR_UNKNOWN,
    P2500_DMA_DIR_IO_TO_MEMORY,  /* READ DATA: FDC ($15) -> RAM (Port B) */
    P2500_DMA_DIR_MEMORY_TO_IO,  /* WRITE DATA: RAM (Port B) -> FDC ($15) */
} P2500DmaDirection;

/* What the next incoming byte means. P2500_DMA_NEXT_BASE (the queue
 * empty) means a fresh, self-describing base register byte is expected;
 * every other value means the byte is a follow-up data byte for the
 * group currently being programmed. */
typedef enum {
    P2500_DMA_NEXT_BASE = 0,
    P2500_DMA_NEXT_A_ADDR_LO,
    P2500_DMA_NEXT_A_ADDR_HI,
    P2500_DMA_NEXT_BLOCKLEN_LO,
    P2500_DMA_NEXT_BLOCKLEN_HI,
    P2500_DMA_NEXT_WR1_TIMING,
    P2500_DMA_NEXT_WR2_TIMING,
    P2500_DMA_NEXT_WR3_MASK,
    P2500_DMA_NEXT_WR3_MATCH,
    P2500_DMA_NEXT_B_ADDR_LO,
    P2500_DMA_NEXT_B_ADDR_HI,
    P2500_DMA_NEXT_INT_CTRL,
    P2500_DMA_NEXT_PULSE_CTRL,
    P2500_DMA_NEXT_VECTOR,
} P2500DmaByteRole;

/* WR4's own worst case (Port B addr low+high, interrupt control, pulse
 * control, vector) is the deepest chain - 5 is enough headroom. */
#define P2500_DMA_QUEUE_CAP 8

typedef void (*P2500DmaInterruptCallback)(void *userdata, uint8_t vector);

typedef struct {
    /* Parser state: a small FIFO of "what does the next raw byte mean"
     * roles, queued by whichever base register byte is currently being
     * expanded. Empty (queue_len==0) means the next byte is a fresh base
     * register byte - see P2500DmaByteRole. */
    P2500DmaByteRole queue[P2500_DMA_QUEUE_CAP];
    int queue_len;

    P2500DmaDirection direction;
    uint16_t block_length_raw; /* wire value (count-1), byte-addressable */
    uint16_t block_length;     /* = block_length_raw + 1, recomputed live */
    uint16_t port_b_addr;      /* RAM buffer address, byte-addressable */
    uint8_t vector;            /* end-of-block interrupt vector */

    bool loaded;             /* WR6 $CF (Load) seen since last reset */
    bool interrupts_enabled; /* WR6 $AB seen, cleared by $AF (or WR3 D5) */
    bool dma_enabled;        /* WR6 $87 seen, cleared by $83 (or WR3 D6) */

    P2500DmaInterruptCallback on_interrupt;
    /* Called when a command resets the chip or disables its interrupts -
     * see notify_int_reset in dma.c. */
    void (*on_int_reset)(void *userdata);
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

#ifdef __cplusplus
}
#endif

#endif
