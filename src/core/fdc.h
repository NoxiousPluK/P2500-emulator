#ifndef P2500_FDC_H
#define P2500_FDC_H

#include <stdint.h>

#include "log.h"
#include <stdbool.h>
#include <stddef.h>
#include "dma.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Behavioral uPD765 model for the P2500 CPU card's on-board floppy driver.
 *
 * Ground truth (see ROM Dumps/CPU-Card-Boot-EPROM/disassembly/findings.md
 * in the parent research project): command-construction tables read
 * directly out of the P2500 IPL ROM decode as real uPD765 commands -
 * $08=SENSE INTERRUPT STATUS, $04=SENSE DRIVE STATUS, $07=RECALIBRATE,
 * $0F=SEEK - with byte counts and result-phase shapes matching real
 * hardware conventions exactly. Independently cross-checked against a
 * real Philips Field Support Manual transcription for the sibling P2000M
 * machine (ifilot/p2000m-emulator), which documents the identical command
 * family and order (Sense Interrupt Status -> Specify -> Recalibrate,
 * then Seek) for its own uPD765-based floppy boot, though on different
 * I/O ports - same engineering team/convention, different wiring.
 *
 * Ports $14 (Main Status Register, read-only) and $15 (Data Register,
 * read/write) are the real P2500 ports - confirmed independently against
 * the CP/M-era CBIOS disassembly (Disk Images/findings/
 * BIOS-disassembly-findings.md), which found the same pair.
 *
 * READ DATA's sector bytes are delivered via the real Z80A-DMA model
 * (dma.{c,h}, port $16) in one shot (the moment the command's parameter
 * bytes are complete) rather than byte-by-byte in step with the uPD765's
 * actual transfer timing - see dma.c for the block length/destination
 * decode this pulls from.
 *
 * RECALIBRATE and SEEK are asynchronous on real hardware (no immediate
 * result phase - completion is signalled by an interrupt, and the driver
 * is expected to follow up with SENSE INTERRUPT STATUS). This model fires
 * that interrupt via the supplied callback as soon as the command's
 * parameter bytes are received - a real completion signal, just not
 * modeled with realistic seek-time delay yet.
 */

#define P2500_FDC_SECTOR_SIZE 256
#define P2500_FDC_SECTORS_PER_TRACK 16
#define P2500_FDC_CYLINDERS 77

typedef enum {
    P2500_FDC_IDLE,
    P2500_FDC_COMMAND,
    P2500_FDC_RESULT
} P2500FdcPhase;

/* Called once, as an edge notification, when an async command
 * (Recalibrate/Seek) completes or a Read Data finishes - optional (e.g.
 * for logging). The actual PIO/CPU delivery is level-driven off `int_line`
 * (see above), sampled by machine.c on every step. */
typedef void (*P2500FdcInterruptCallback)(void *userdata);

#define P2500_FDC_MAX_DRIVES 4

typedef struct {
    P2500FdcPhase phase;
    uint8_t command[16];
    size_t command_len;
    size_t command_expected;

    uint8_t result[8];
    size_t result_len;
    size_t result_pos;

    uint8_t cylinder;
    bool last_seek_ok;

    /* Set by RECALIBRATE/SEEK completion, cleared by the SENSE INTERRUPT
     * STATUS that reports it - real uPD765 semantics: SIS returns Invalid
     * Command ($80, 1 byte) rather than a fabricated Seek End if nothing
     * is actually pending (TODO.md ISSUE-1). */
    bool seek_int_pending;

    /* Budget for p2500_fdc_raise_startup_interrupt() (TODO.md ISSUE-1 fix
     * 2). A real uPD765 generates one unsolicited post-reset interrupt
     * *per configured drive* (the classic "issue N SENSE INTERRUPT STATUS
     * after reset" convention). Set to exactly 2 - not a cap "just in
     * case" but a value pinned down by directly tracing the ROM's own
     * control flow (TODO.md ISSUE-3): the *third* PIO-arm event is
     * `$0786`'s "enable interrupt, then immediately stream the already-
     * built RECALIBRATE command" sequence - there's no `EI` between the
     * two, because real hardware can't possibly raise a completion
     * interrupt before the command bytes (and the physical seek behind
     * them) are done. Firing a synthetic interrupt here is never correct
     * - it can only hijack control before the real command is sent, not
     * substitute for a real completion. Stops being drawn on once a real
     * Recalibrate/Seek/Read Data is issued (`real_operation_started`)
     * too, as a second line of defense. */
    int startup_interrupts_remaining;
    bool real_operation_started;

    /* Real uPD765 /INT is a held level, not a pulse: it stays asserted from
     * command completion until the host reads through a result phase (see
     * TODO.md T9). machine.c samples this every step to drive the PIO input
     * bit, so polling code (e.g. the ROM's "IN A,($10)/RRA" at $0832) sees
     * it held, not just edge-triggered PIO delivery. */
    bool int_line;

    /* Up to four units, as the uPD765 addresses. CBIOS supports three of
     * them: SELDSK ($E620) indexes a per-drive record table at $E880 whose
     * first byte selects an availability byte from $E87C - drives 0, 1 and
     * 2 map to $00 (available) and 3 upwards to $01 (rejected, SELDSK
     * returns 0). Each has its own DPH with its own CSV/ALV buffers at
     * $E81D, so A:, B: and C: are real as far as the firmware is concerned.
     * The unit comes from the command's second byte, bits 0-1. */
    const uint8_t *disk[P2500_FDC_MAX_DRIVES];
    size_t disk_size[P2500_FDC_MAX_DRIVES];
    uint8_t unit; /* unit selected by the command being executed */

    /* set once by machine.c at init: the flat 64K RAM image (destination
     * for DMA-delivered READ DATA bytes) and the DMA model that owns the
     * transfer's length/address, as programmed via port $16 */
    uint8_t *ram;
    P2500Dma *dma;

    /* Diagnostics sink, pointed at the machine's own by p2500_init().
     * NULL is fine - the messages are simply discarded (TODO.md T34). */
    const P2500Log *log;
    P2500FdcInterruptCallback on_interrupt;
    void *interrupt_userdata;

    bool verbose;
} P2500Fdc;

void p2500_fdc_init(P2500Fdc *fdc, const uint8_t *disk, size_t disk_size);

/* Attach (or with NULL, eject) the image in a drive. Unit 0 is A:. */
void p2500_fdc_attach(P2500Fdc *fdc, unsigned unit,
                      const uint8_t *data, size_t size);
/* Fires one of the uPD765's real, well-documented post-reset unsolicited
 * interrupts (TODO.md ISSUE-1 fix 2) - a real chip generates one per
 * configured drive shortly after coming out of reset, before any
 * Recalibrate/Seek has ever been issued, which is exactly what this ROM's
 * own defensive early SENSE INTERRUPT STATUS calls (see fix 1) are
 * written to expect and drain. This emulator has no elapsed-time model,
 * so machine.c calls this event-triggered (each time PIO port A
 * interrupts are (re-)armed) rather than time-triggered - standing in for
 * "the interrupt was already pending by the time the driver started
 * listening for it." No-op (returns false) once
 * startup_interrupts_remaining is exhausted or a real operation has
 * started; returns true when it actually marked one pending - the caller
 * (machine.c) only pulses the PIO input bit when this returns true. Note
 * this does NOT touch `int_line`/call the interrupt callback itself
 * (unlike a real FDC completion) - delivery is the caller's job, as a
 * one-shot pulse rather than a held level (see machine.c and TODO.md
 * ISSUE-3 for why: not every consumer of this synthetic interrupt reads
 * a SENSE INTERRUPT STATUS result to clear a held level). */
bool p2500_fdc_raise_startup_interrupt(P2500Fdc *fdc);
uint8_t p2500_fdc_read_status(P2500Fdc *fdc);   /* port $14 */
uint8_t p2500_fdc_read_data(P2500Fdc *fdc);     /* port $15 read */
void p2500_fdc_write_data(P2500Fdc *fdc, uint8_t value); /* port $15 write */

#ifdef __cplusplus
}
#endif

#endif
