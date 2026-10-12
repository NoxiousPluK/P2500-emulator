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
 * Command-construction tables decoded from the P2500 IPL ROM as real
 * uPD765 commands - $08=SENSE INTERRUPT STATUS, $04=SENSE DRIVE STATUS,
 * $07=RECALIBRATE, $0F=SEEK - with byte counts and result-phase shapes
 * matching real hardware conventions exactly. Cross-checked against the
 * sibling P2000M machine's floppy boot (same command family and order:
 * Sense Interrupt Status -> Specify -> Recalibrate -> Seek), though the
 * P2500 uses different I/O ports.
 *
 * Ports $14 (Main Status Register, read-only) and $15 (Data Register,
 * read/write) are the real P2500 ports, confirmed independently against
 * the CP/M-era PBIOS disassembly - PBIOS and not CBIOS, which on both
 * bootable generations contains no port access at all.
 *
 * READ DATA's and WRITE DATA's sector bytes move through the real Z80A-DMA
 * model (dma.{c,h}, port $16) in one shot (the moment the command's
 * parameter bytes are complete) rather than byte-by-byte in step with the
 * uPD765's actual transfer timing - see dma.c for the block
 * length/address/direction decode this pulls from. The two commands are
 * one function here (do_read_write), because on a real uPD765 they differ
 * in exactly three things: which way the bytes go, whether a
 * write-protected disk refuses, and the DT bit in ST1. Everything else -
 * the geometry, the EOT bound, the result-phase ID advance - is shared
 * hardware behaviour, so sharing the code keeps the two from drifting.
 *
 * What the one-shot transfer does NOT cost us is EXM, the Main Status
 * Register's "execution phase in progress" bit. EXM is set only during a
 * NON-DMA execution phase, and this machine never has one: every SPECIFY
 * this project has traced - $03 $30 $24 from the IPL, $03 $F0 $0A from
 * PBIOS, identical on all four bootable disks - carries ND=0 in bit 0 of
 * its second parameter byte, selecting DMA mode. So EXM reads 0 on real
 * hardware here too, and modelling it would mean modelling a mode no
 * P2500 firmware selects.
 *
 * WRITE DATA needs somewhere to put the bytes, and media mounted through
 * p2500_fdc_attach() is const - a read-only mount, which is not a
 * limitation to apologise for but the model of a disk with its
 * write-protect tab in place: SENSE DRIVE STATUS reports WP and a write
 * to it terminates with NW, exactly as the hardware does. Media mounted
 * through p2500_fdc_attach_writable() accepts writes, into the caller's
 * own buffer and never to a file - persisting that buffer is a front-end
 * decision (p2500-emu's --save-disk, the GUI's File menu), so the only
 * copies of rare media cannot be damaged by a guest program.
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

/* How many unsolicited post-reset attention interrupts the model hands out
 * per release of the controller's RESET line. Measured in both directions
 * against the IPL: see p2500_fdc_reset_line_released(). */
#define P2500_FDC_POST_RESET_INTERRUPTS 2

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
     * is actually pending. */
    bool seek_int_pending;

    /* Budget for p2500_fdc_raise_startup_interrupt(). A real uPD765
     * generates one unsolicited post-reset interrupt
     * *per configured drive* (the classic "issue N SENSE INTERRUPT STATUS
     * after reset" convention). Set to exactly 2 - not a cap "just in
     * case" but a value pinned down by directly tracing the ROM's own
     * control flow: the *third* PIO-arm event is
     * `$0786`'s "enable interrupt, then immediately stream the already-
     * built RECALIBRATE command" sequence - there's no `EI` between the
     * two, because real hardware can't possibly raise a completion
     * interrupt before the command bytes (and the physical seek behind
     * them) are done. Firing a synthetic interrupt here is never correct
     * - it can only hijack control before the real command is sent, not
     * substitute for a real completion. Stops being drawn on once a real
     * Recalibrate/Seek/Read Data is issued (`real_operation_started`)
     * too, as a second line of defense.
     *
     * Refilled by p2500_fdc_reset_line_released() on every release of the
     * controller's RESET line, not just the cold-boot one: a second
     * driver taking the card over after the IPL (the disk driver in
     * P25K_G's PBIOS does exactly this) resets it again and waits for its
     * own post-reset attention interrupt. */
    int startup_interrupts_remaining;
    bool real_operation_started;

    /* Real uPD765 /INT is a held level, not a pulse: it stays asserted from
     * command completion until the host reads through a result phase.
     * machine.c samples this every step to drive the PIO input
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
    /* Non-NULL, and equal to disk[u], when that drive's media accepts
     * writes; NULL is a write-protected disk. Two pointers rather than one
     * plus a flag so that the const-correctness of the read path survives:
     * nothing can write through disk[] by accident. */
    uint8_t *disk_rw[P2500_FDC_MAX_DRIVES];
    /* Sectors this run has written into each drive's buffer. The front end
     * reads them to know whether an image is worth saving and to say so
     * before an eject throws the changes away - a silent drop otherwise. */
    unsigned long sectors_written[P2500_FDC_MAX_DRIVES];
    uint8_t unit; /* unit selected by the command being executed */

    /* set once by machine.c at init: the flat 64K RAM image (destination
     * for DMA-delivered READ DATA bytes) and the DMA model that owns the
     * transfer's length/address, as programmed via port $16 */
    uint8_t *ram;
    P2500Dma *dma;

    /* Diagnostics sink, pointed at the machine's own by p2500_init().
     * NULL is fine - the messages are simply discarded. */
    const P2500Log *log;
    P2500FdcInterruptCallback on_interrupt;
    void *interrupt_userdata;

    /* Set by core/history.c around a silent replay. A rewind restores a
     * snapshot and re-executes forward, so every WRITE DATA between the
     * snapshot and the target would be performed a second time - into
     * media that already holds it, since the guest's RAM is identical.
     * Suppressing the commit (not the DMA transfer, and not the interrupt,
     * which the replayed code is waiting on) is what keeps a rewind free of
     * side effects, which is the property --verify-rewind asserts for
     * everything that lives inside the machine struct and the media does
     * not.
     *
     * It does not make a rewind undo a write: media is the front end's
     * buffer, outside the snapshot ring by the same design that stops a
     * rewind resurrecting a pointer the front end has freed. Rewind past a
     * write and then diverge, and the disk still holds it. */
    bool suppress_writes;

    bool verbose;
} P2500Fdc;

void p2500_fdc_init(P2500Fdc *fdc, const uint8_t *disk, size_t disk_size);

/* Attach (or with NULL, eject) the image in a drive. Unit 0 is A:. */
void p2500_fdc_attach(P2500Fdc *fdc, unsigned unit,
                      const uint8_t *data, size_t size);
/* The same, for media the guest may write to: `data` is the front end's own
 * buffer and the FDC writes sectors into it in place. Nothing here ever
 * touches a file. Attaching read-only media over it (p2500_fdc_attach, or
 * NULL to eject) clears the writable pointer too, so a drive cannot be left
 * holding a stale one. */
void p2500_fdc_attach_writable(P2500Fdc *fdc, unsigned unit,
                               uint8_t *data, size_t size);
/* Fires one of the uPD765's real, well-documented post-reset unsolicited
 * interrupts - a real chip generates one per
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
 * one-shot pulse rather than a held level (see machine.c: not every
 * consumer of this synthetic interrupt reads a SENSE INTERRUPT STATUS
 * result to clear a held level). */
bool p2500_fdc_raise_startup_interrupt(P2500Fdc *fdc);

/*
 * The controller's RESET line (PIO port B bit 2) went from asserted to
 * released: refill the post-reset interrupt budget, because a real uPD765
 * re-runs its drive-polling sweep after every reset and raises a fresh
 * unsolicited interrupt from it - not only the first time.
 *
 * This matters for any driver that takes the card over after the IPL has
 * already spent the cold-boot budget. The driver that does is in PBIOS,
 * not CBIOS: neither bootable generation's CBIOS contains a single port
 * access. P25K_G's PBIOS resets the controller at $FA1A ($56 -> $52, a
 * settle delay, then "IN A,($14) / CP $80" to confirm it came back idle),
 * and its disk driver at $FB20 then arms one asynchronous operation -
 * "port B = $42, patch the continuation, OUT ($12),$F3, arm PA3, RET"
 * ($FB2B-$FB3C) - which needs exactly one attention interrupt to get
 * going. Once it arrives the driver sets its post-reset flag and every
 * later transfer runs the handler inline instead. Without this refill it
 * waited forever and the disk never reached the CP/M prompt.
 *
 * That flag is the firmware's own evidence for the mechanism, and it is
 * written in exactly two places: set to $FF at the tail of the
 * asynchronous completion, and cleared to 0 immediately after a RESET
 * release. So the driver arms the interrupt for ONE operation after each
 * controller reset and drives the handler inline ever after - which is
 * the shape this refill models. It is $FD24 on P25K_G, $FCF3 on P25K_B.
 *
 * Both paths are exercised, and the counts are asserted (section 1c of
 * tests/run_tests.sh). P25K_G takes the arm-and-return path exactly once
 * per boot ($FB2D: 1 of its driver's 8 calls). P25K_B never does: its
 * newer driver at $FAB7 has an extra "IN A,($11) / AND $80" escape at
 * $FABA - PB7, read as a card-capability strap and modelled as 0 until
 * someone measures it - which sends all 8 calls down the inline path
 * ($FAC8: never). That is why P25K_B booted regardless and hid this gap
 * for so long.
 *
 * Two is measured rather than chosen, and the IPL brackets it from both
 * sides - diskless and on both bootable disks alike:
 *
 *   1 - hangs in the busy-wait on $FED5 at $06C6 (symbols/ipl.sym's
 *       BUSYWAIT1). It is the SECOND synthetic interrupt that reaches the
 *       ISR which sets that flag, so one is not enough.
 *   2 - boots.
 *   3 - derails, and exactly where the field comment on
 *       startup_interrupts_remaining says it would: the third PIO-arm
 *       event is $0786's "enable interrupt, then immediately stream
 *       RECALIBRATE", so a synthetic attention there hijacks a command
 *       instead of standing in for one. Traced, the spare interrupt
 *       fires at PC=$0793, the IPL walks into a transfer it never
 *       completes, and it spins at $0AC3 ("IN A,($14) / AND $10 / JR NZ")
 *       on a CB bit that never clears.
 *
 * So the one free parameter in this stand-in is pinned, and section 1b
 * catches a change to it with no disk image at all.
 */
void p2500_fdc_reset_line_released(P2500Fdc *fdc);

uint8_t p2500_fdc_read_status(P2500Fdc *fdc);   /* port $14 */
uint8_t p2500_fdc_read_data(P2500Fdc *fdc);     /* port $15 read */
void p2500_fdc_write_data(P2500Fdc *fdc, uint8_t value); /* port $15 write */

#ifdef __cplusplus
}
#endif

#endif
