#include "fdc.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t opcode;
    const char *name;
    size_t total_len; /* including opcode byte */
} CmdInfo;

static const CmdInfo COMMANDS[] = {
    {0x03, "SPECIFY", 3},
    {0x04, "SENSE DRIVE STATUS", 2},
    /* WRITE DATA's shape is not assumed from the datasheet alone: the
     * P2500's own driver was traced issuing it, and it is byte-for-byte a
     * READ DATA with the opcode changed - `$05 $00 $03 $00 $01 $01 $10
     * $0E $00` against the read's `$06 $00 $03 $00 $01 $01 $10 $0E $00`,
     * same unit, same C/H/R/N, same EOT, same GPL. Captured from a `SAVE 1
     * ZZ.XXX` at the CCP prompt on P25K_B. */
    {0x05, "WRITE DATA", 9},
    {0x06, "READ DATA", 9},
    {0x07, "RECALIBRATE", 2},
    {0x08, "SENSE INTERRUPT STATUS", 1},
    /* Datasheet shape, not traced: nothing in the surviving corpus formats
     * a disk (CONFIG and CPYDSK live on the half dumps). See
     * do_format_track. */
    {0x0D, "FORMAT TRACK", 6},
    {0x0F, "SEEK", 3},
};
#define NUM_COMMANDS (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

static const CmdInfo *find_command(uint8_t opcode_byte) {
    uint8_t opcode = opcode_byte & 0x1F;
    for (size_t i = 0; i < NUM_COMMANDS; i++)
        if (COMMANDS[i].opcode == opcode) return &COMMANDS[i];
    return NULL;
}

void p2500_fdc_attach(P2500Fdc *fdc, unsigned unit,
                      const uint8_t *data, size_t size) {
    if (unit >= P2500_FDC_MAX_DRIVES) return;
    fdc->disk[unit] = data;
    fdc->disk_size[unit] = size;
    /* A read-only mount over writable media must take the writable
     * pointer with it, or the drive would keep accepting writes into the
     * image that is no longer in it. */
    fdc->disk_rw[unit] = NULL;
}

void p2500_fdc_attach_writable(P2500Fdc *fdc, unsigned unit,
                               uint8_t *data, size_t size) {
    if (unit >= P2500_FDC_MAX_DRIVES) return;
    fdc->disk[unit] = data;
    fdc->disk_rw[unit] = data;
    fdc->disk_size[unit] = size;
}

void p2500_fdc_init(P2500Fdc *fdc, const uint8_t *disk, size_t disk_size) {
    /* Re-initialising a device must not silently take its diagnostics
     * away with it: a caller that swaps a keystroke queue in, as the
     * CLI does, would otherwise lose every message the device had to
     * make from then on - a silent drop, which is a failure mode worth
     * guarding against explicitly. */
    const P2500Log *log = fdc->log;
    memset(fdc, 0, sizeof(*fdc));
    fdc->log = log;
    fdc->phase = P2500_FDC_IDLE;
    fdc->cylinder = 0;
    fdc->last_seek_ok = true;
    fdc->disk[0] = disk;
    fdc->disk_size[0] = disk_size;
    fdc->startup_interrupts_remaining = 2; /* see fdc.h - pinned down by tracing, not a guess */
}

uint8_t p2500_fdc_read_status(P2500Fdc *fdc) {
    uint8_t rqm = 1; /* always ready for the next byte in this simplified model */
    uint8_t dio = (fdc->phase == P2500_FDC_RESULT) ? 1 : 0;
    /* CB (FDC Busy): set for the whole command+execution+result phase, not
     * just while it's IDLE waiting for the next opcode byte - visible to a
     * driver that polls $14 mid-command (e.g. between a multi-byte
     * command's parameter writes).
     *
     * EXM (bit 5) reads 0, and that is the hardware's answer rather than
     * this model's. The datasheet sets EXM only during a NON-DMA execution
     * phase, and no P2500 firmware ever selects one: every SPECIFY traced
     * here carries ND=0 (bit 0 of its second parameter byte) - $30 $24
     * from the IPL, $F0 $0A from CBIOS, the same on all four bootable
     * disks. A real controller in this machine therefore never raises EXM
     * either, so the one-shot DMA transfer below costs no CPU-visible
     * state. (This comment used to claim the opposite.) */
    uint8_t cb = (fdc->phase != P2500_FDC_IDLE) ? 1 : 0;
    return (uint8_t)((rqm << 7) | (dio << 6) | (cb << 4));
}

static void fire_interrupt(P2500Fdc *fdc) {
    fdc->int_line = true;
    if (fdc->on_interrupt) fdc->on_interrupt(fdc->interrupt_userdata);
}

void p2500_fdc_reset_line_released(P2500Fdc *fdc) {
    /* See fdc.h. The budget goes back to its full cold-boot value rather
     * than to 1: the IPL pulses RESET twice back-to-back at $0AE5 before
     * drawing on it at all, so anything smaller would hand the IPL a
     * shorter budget than it has today. */
    fdc->real_operation_started = false;
    fdc->startup_interrupts_remaining = 2;
    if (fdc->verbose)
        p2500_logf(fdc->log, P2500_LOG_TRACE, "fdc",
                   "RESET released - post-reset interrupt budget refilled to 2");
}

bool p2500_fdc_raise_startup_interrupt(P2500Fdc *fdc) {
    if (fdc->real_operation_started || fdc->startup_interrupts_remaining <= 0) return false;
    fdc->startup_interrupts_remaining--;
    fdc->seek_int_pending = true;
    if (fdc->verbose)
        p2500_logf(fdc->log, P2500_LOG_TRACE, "fdc", "post-reset unsolicited interrupt (%d remaining)",
                   fdc->startup_interrupts_remaining);
    /* Deliberately doesn't call fire_interrupt()/touch int_line - the
     * caller (machine.c) delivers this as a one-shot pulse instead of
     * the held level real completions use. See machine.c's port $12
     * handler for why. */
    return true;
}

static void set_result(P2500Fdc *fdc, const uint8_t *bytes, size_t n) {
    memcpy(fdc->result, bytes, n);
    fdc->result_len = n;
    fdc->result_pos = 0;
    fdc->phase = n ? P2500_FDC_RESULT : P2500_FDC_IDLE;
}

/* Where a (C,H,R) address lands in a flat .raw image.
 *
 * The P2500/P2000M disk format writes the on-disk track ID one higher than
 * the physical track. The firmware issues C matching what is recorded on
 * the disk (the logical ID), so the physical track - and this flat dump's
 * own track order - is C-1, not C. C=1,R=1 (the IPL's first read) resolves
 * to byte offset 0, the boot sector. */
static size_t sector_offset(uint8_t c, uint8_t r) {
    size_t physical_track = c > 0 ? (size_t)(c - 1) : 0;
    size_t lba = physical_track * P2500_FDC_SECTORS_PER_TRACK + (r - 1);
    return lba * P2500_FDC_SECTOR_SIZE;
}

/* READ DATA ($06) and WRITE DATA ($05) in one function, because a real
 * uPD765 executes them identically apart from the direction of the bytes
 * and the write-protect check - same parameter bytes, same EOT bound, same
 * result-phase ID arithmetic. Both were traced off this machine's own
 * driver; see the command table.
 *
 * command[0]=opcode, [1]=unit/head, [2]=C, [3]=H, [4]=R, [5]=N, [6]=EOT,
 * [7]=GPL, [8]=DTL. */
static void do_read_write(P2500Fdc *fdc, bool write) {
    const char *what = write ? "WRITE DATA" : "READ DATA";
    uint8_t c = fdc->command[2];
    uint8_t head = fdc->command[3];
    uint8_t r = fdc->command[4];
    uint8_t eot = fdc->command[6];
    size_t off = sector_offset(c, r);

    const uint8_t *media = fdc->disk[fdc->unit];
    const size_t media_size = fdc->disk_size[fdc->unit];

    /* One place to build the 7-byte result phase, so the failure paths
     * cannot drift from each other or from the success path. */
    #define FAIL_RW(st0, st1, res_c, res_r)                                   \
        do {                                                                  \
            uint8_t res_[7] = {(st0), (st1), 0x00, (res_c), head, (res_r),    \
                               fdc->command[5]};                              \
            set_result(fdc, res_, 7);                                         \
            fire_interrupt(fdc);                                              \
            return;                                                           \
        } while (0)

    if (!media) {
        /* No media at all. A real uPD765 terminates a read or write to a
         * not-ready drive with IC=01 (abnormal) and the NR bit set - ST0
         * bit 3, per the datasheet's "this flag is set when the FDD is in
         * the not-ready state and a Read or Write command is issued".
         *
         * Reporting a plain $40 here instead made the machine unbootable in
         * a way real hardware is not: the IPL's boot-attempt chain at $0333
         * gives up on a nonzero request status and prints its own banner,
         * but it never got that far, so a diskless machine sat in the
         * $06C6 busy-wait forever with a blank screen. */
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "%s unit %u C=%u R=%u: drive not ready",
                       what, fdc->unit, c, r);
        FAIL_RW(0x48, 0x00, c, r);
    }
    if (write && !fdc->disk_rw[fdc->unit]) {
        /* Write-protected media: the disk is mounted read-only, which is
         * this emulator's model of the tab being in place. ST1 bit 1 is
         * NW, "not writable", with IC=01 in ST0 - the datasheet's own
         * answer, and the one the P2219 manual's SESAM initialisation
         * expects when it tells the operator to remove the tab. */
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "%s unit %u C=%u R=%u: media is write-protected",
                       what, fdc->unit, c, r);
        FAIL_RW(0x40, 0x02, c, r);
    }
    if (off + P2500_FDC_SECTOR_SIZE > media_size) {
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "%s C=%u R=%u out of range (off=0x%zx)",
                       what, c, r, off);
        FAIL_RW(0x40, 0x00, c, r);
    }

    /* A multi-sector transfer is bounded by EOT, the last sector number on
     * the track - not by the end of the disk image. Handing the DMA the
     * whole remaining span let a transfer that outlived the track walk
     * silently into the next one; a real uPD765 stops at EOT and says so.
     * Every read and write this project's firmware issues carries EOT=$10
     * (= 16 sectors, one track), so the bound below is the one real
     * hardware applies. */
    if (eot < r) {
        /* No sectors in the range at all. A driver asking for this has a
         * bug; report it as the end of the cylinder rather than silently
         * transferring something. */
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "%s C=%u R=%u: EOT=%u is below R, "
                               "nothing to transfer", what, c, r, eot);
        FAIL_RW(0x40, 0x80, c, r);
    }

    if (fdc->command[0] & 0x80) {
        /* MT: continue onto the second head after EOT. These images are
         * single-sided and the .raw layout has no second side to continue
         * into, so the transfer stays bounded by EOT either way - but say
         * so rather than quietly ignoring the bit. */
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "%s C=%u R=%u: MT set but the media "
                               "is single-sided; stopping at EOT=%u", what, c, r, eot);
    }

    size_t span = (size_t)(eot - r + 1) * P2500_FDC_SECTOR_SIZE;
    size_t avail = media_size - off;
    bool short_media = span > avail;
    if (short_media) span = avail;

    /* Whether this terminates normally or at the end of the cylinder
     * depends on whether the DMA wanted more than the track had left, and
     * that has to be asked before the transfer. */
    size_t wanted = 0;
    size_t moved = 0;

    if (fdc->ram && fdc->dma) {
        wanted = write ? p2500_dma_offered_bytes(fdc->dma)
                       : p2500_dma_requested_bytes(fdc->dma);
        if (write) {
            /* Staged rather than written straight through, for two
             * reasons: the DMA's own count decides how much of the span
             * actually moves, and a silent replay must take the bytes off
             * the DMA (and let it raise end-of-block) without touching the
             * disk a second time - see suppress_writes. */
            uint8_t staged[P2500_FDC_SECTORS_PER_TRACK * P2500_FDC_SECTOR_SIZE];
            size_t room = span < sizeof staged ? span : sizeof staged;
            moved = p2500_dma_fetch(fdc->dma, fdc->ram, staged, room);
            if (!fdc->suppress_writes)
                memcpy(fdc->disk_rw[fdc->unit] + off, staged, moved);
        } else {
            moved = p2500_dma_deliver(fdc->dma, fdc->ram, media + off, span);
        }
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_TRACE, "fdc", "%s C=%u R=%u -> disk offset 0x%zx, "
                               "%zu of %zu bytes to EOT=%u %s DMA", what, c, r, off, moved, span, eot,
                       write ? "taken from" : "handed to");
    } else if (fdc->verbose) {
        p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "%s C=%u R=%u -> sector addressed but no "
                           "RAM/DMA wired up, dropped", what, c, r);
    }

    if (write && moved && !fdc->suppress_writes) {
        /* Whole sectors only, as the controller counts them. The front end
         * reads this to know the buffer no longer matches the file it came
         * from - see p2500_fdc_attach_writable. */
        fdc->sectors_written[fdc->unit] +=
            (unsigned long)((moved + P2500_FDC_SECTOR_SIZE - 1) / P2500_FDC_SECTOR_SIZE);
    }

    /* The DMA stops on a byte boundary; the FDC counts whole sectors. */
    size_t sectors = (moved + P2500_FDC_SECTOR_SIZE - 1) / P2500_FDC_SECTOR_SIZE;
    uint8_t last = (uint8_t)(r + (sectors ? sectors - 1 : 0));

    /* Result-phase ID, per the datasheet's multi-sector table: below EOT
     * the cylinder stands and R advances; at EOT the cylinder advances and
     * R goes back to 1. */
    uint8_t res_c = c, res_r;
    if (last >= eot) {
        res_c = (uint8_t)(c + 1);
        res_r = 1;
    } else {
        res_r = (uint8_t)(last + 1);
    }

    uint8_t st0 = 0x00, st1 = 0x00;
    if (moved == 0 && span > 0) {
        /* The command addressed real sectors and not one byte moved, which
         * on this machine means the DMA was not programmed to serve it -
         * not enabled, or enabled for the other direction. A real uPD765
         * whose data path never answers terminates with OR, the overrun
         * bit: ST1 bit 4, "the FDC was not serviced in time". Saying that
         * is the point. Before writes existed this fell through as a
         * normal termination with an empty transfer, which is precisely
         * the silent drop that hid the read path's missing DMA for a
         * whole phase. */
        st0 = 0x40;
        st1 = 0x10; /* OR - overrun */
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "%s C=%u R=%u: nothing moved - the DMA is not "
                               "programmed for this direction; reporting overrun", what, c, r);
    } else if (short_media) {
        /* The track claims sectors the image does not carry. */
        st0 = 0x40;
        st1 = 0x04; /* ND - no data, sector not found */
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "%s C=%u R=%u: track runs past the end "
                               "of the image, %zu bytes short", what, c, r,
                       (size_t)((eot - r + 1) * P2500_FDC_SECTOR_SIZE) - avail);
    } else if (wanted > span) {
        /* The requester wanted more than this track held, so the transfer
         * ended on EOT rather than on the DMA's terminal count. That is
         * the uPD765's "end of cylinder": abnormal termination with EN
         * set, and the condition this code used to paper over by reading
         * on into the next track. */
        st0 = 0x40;
        st1 = 0x80; /* EN - end of cylinder */
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "%s C=%u R=%u: requester wanted %zu bytes, "
                               "track had %zu to EOT=%u - terminating at end of cylinder",
                       what, c, r, wanted, span, eot);
    }

    uint8_t res[7] = {st0, st1, 0x00, res_c, head, res_r, fdc->command[5]};
    set_result(fdc, res, 7);
    fire_interrupt(fdc);
    #undef FAIL_RW
}

/* FORMAT TRACK ($0D). Unlike every other command modelled here, this one is
 * NOT traced off this machine: nothing in the surviving software corpus
 * formats a disk, because CONFIG and CPYDSK live on the double-stepped half
 * dumps. So the shape below is the datasheet's and nothing else, and it is
 * exercised by tests/fdc_write.c rather than by any guest program. Said
 * plainly rather than left for a reader to discover: if a real P2500
 * formatter ever turns up and disagrees, the datasheet loses.
 *
 * command[0]=opcode, [1]=unit/head, [2]=N, [3]=SC, [4]=GPL, [5]=D (filler).
 * The execution phase takes four bytes per sector from the DMA - C,H,R,N -
 * and writes a whole sector of the filler byte for each. The result phase
 * is a read/write's seven bytes, with the ID bytes meaningless. */
static void do_format_track(P2500Fdc *fdc) {
    uint8_t head = fdc->command[1] & 0x04 ? 1 : 0;
    uint8_t n = fdc->command[2];
    uint8_t sc = fdc->command[3];
    uint8_t filler = fdc->command[5];

    #define FAIL_FMT(st0, st1)                                                \
        do {                                                                  \
            uint8_t res_[7] = {(st0), (st1), 0x00, 0x00, head, 0x00, n};      \
            set_result(fdc, res_, 7);                                         \
            fire_interrupt(fdc);                                              \
            return;                                                           \
        } while (0)

    if (!fdc->disk[fdc->unit]) {
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "FORMAT TRACK unit %u: drive not ready", fdc->unit);
        FAIL_FMT(0x48, 0x00);
    }
    if (!fdc->disk_rw[fdc->unit]) {
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "FORMAT TRACK unit %u: media is write-protected", fdc->unit);
        FAIL_FMT(0x40, 0x02);
    }
    if (!fdc->ram || !fdc->dma) {
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "FORMAT TRACK unit %u: no RAM/DMA wired up", fdc->unit);
        FAIL_FMT(0x40, 0x00);
    }

    /* The ID field list comes through the DMA exactly as a write's data
     * does, so a formatter that programmed the DMA for a read, or for the
     * wrong length, fails here rather than formatting something. */
    uint8_t ids[4 * 256];
    size_t want = (size_t)sc * 4;
    if (want > sizeof ids) want = sizeof ids;
    size_t got = p2500_dma_fetch(fdc->dma, fdc->ram, ids, want);
    if (got < want) {
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "FORMAT TRACK unit %u: DMA supplied %zu of %zu "
                               "ID bytes for SC=%u", fdc->unit, got, want, sc);
        FAIL_FMT(0x40, 0x00);
    }

    unsigned formatted = 0;
    uint8_t last_c = 0, last_r = 0;
    for (unsigned i = 0; i + 4 <= got; i += 4) {
        uint8_t c = ids[i], r = ids[i + 2];
        size_t off = sector_offset(c, r);
        if (r == 0 || off + P2500_FDC_SECTOR_SIZE > fdc->disk_size[fdc->unit]) {
            if (fdc->verbose)
                p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "FORMAT TRACK: ID C=%u R=%u is not on this "
                                   "media (off=0x%zx)", c, r, off);
            FAIL_FMT(0x40, 0x04); /* ND - no data */
        }
        if (!fdc->suppress_writes)
            memset(fdc->disk_rw[fdc->unit] + off, filler, P2500_FDC_SECTOR_SIZE);
        last_c = c;
        last_r = r;
        formatted++;
    }
    if (!fdc->suppress_writes) fdc->sectors_written[fdc->unit] += formatted;
    if (fdc->verbose)
        p2500_logf(fdc->log, P2500_LOG_TRACE, "fdc", "FORMAT TRACK unit %u: %u sectors filled with $%02X "
                           "(N=%u SC=%u GPL=$%02X)", fdc->unit, formatted, filler, n, sc, fdc->command[4]);

    uint8_t res[7] = {0x00, 0x00, 0x00, last_c, head, last_r, n};
    set_result(fdc, res, 7);
    fire_interrupt(fdc);
    #undef FAIL_FMT
}

static void finish_command(P2500Fdc *fdc) {
    uint8_t opcode = fdc->command[0] & 0x1F;
    const CmdInfo *info = find_command(fdc->command[0]);
    /* Every command that touches a drive carries US1/US0 in bits 0-1 of its
     * second byte (bit 2 is the head). SENSE INTERRUPT STATUS has no second
     * byte and reports on whatever was last selected, so leave it alone. */
    if (fdc->command_len > 1 && opcode != 0x08)
        fdc->unit = fdc->command[1] & 0x03;
    if (fdc->verbose) {
        /* Assembled rather than streamed: a log sink takes whole messages,
         * so the argument bytes have to be one string, not one call each. */
        char args[3 * sizeof fdc->command + 1];
        size_t at = 0;
        for (size_t i = 1; i < fdc->command_len && at + 4 < sizeof args; i++)
            at += (size_t)snprintf(args + at, sizeof args - at, "$%02X ", fdc->command[i]);
        args[at] = '\0';
        p2500_logf(fdc->log, P2500_LOG_TRACE, "fdc", "command %s (opcode $%02X) args=[%s]",
                   info ? info->name : "UNKNOWN", opcode, args);
    }

    /* Once a real disk operation is under way, stop synthesizing startup
     * interrupts - they exist only to unblock the
     * driver's post-reset drain loop, not to stand in for real completions. */
    if (opcode == 0x07 || opcode == 0x0F || opcode == 0x06 || opcode == 0x05 ||
        opcode == 0x0D)
        fdc->real_operation_started = true;

    switch (opcode) {
    case 0x07: /* RECALIBRATE */
        fdc->cylinder = 0;
        fdc->last_seek_ok = true;
        fdc->phase = P2500_FDC_IDLE;
        fdc->result_len = 0;
        fdc->seek_int_pending = true;
        fire_interrupt(fdc);
        break;
    case 0x0F: /* SEEK */
        fdc->cylinder = fdc->command_len > 2 ? fdc->command[2] : 0;
        fdc->last_seek_ok = true;
        fdc->phase = P2500_FDC_IDLE;
        fdc->result_len = 0;
        fdc->seek_int_pending = true;
        fire_interrupt(fdc);
        break;
    case 0x08: /* SENSE INTERRUPT STATUS */
        /* Real uPD765: SENSE INTERRUPT STATUS reports (and clears) a
         * Recalibrate/Seek completion that's actually pending - ST0 +
         * PCN, 2 result bytes. Issued with nothing pending (this ROM
         * does exactly that once, defensively, right after SPECIFY),
         * it returns Invalid Command, 1 byte, not a fabricated "Seek
         * End". */
        if (fdc->seek_int_pending) {
            uint8_t st0 = fdc->last_seek_ok ? 0x20 : 0x40;
            uint8_t res[2] = {st0, fdc->cylinder};
            set_result(fdc, res, 2);
            fdc->seek_int_pending = false;
            if (fdc->verbose) p2500_logf(fdc->log, P2500_LOG_TRACE, "fdc", "SENSE INTERRUPT STATUS -> ST0=$%02X PCN=$%02X (2 bytes)", st0, fdc->cylinder);
        } else {
            uint8_t res[1] = {0x80};
            set_result(fdc, res, 1);
            if (fdc->verbose) p2500_logf(fdc->log, P2500_LOG_TRACE, "fdc", "SENSE INTERRUPT STATUS -> Invalid Command $80 (1 byte)");
        }
        break;
    case 0x04: { /* SENSE DRIVE STATUS */
        /* ST3: bit 5 RDY, bit 4 TRK0 at cylinder 0, and bit 6 WP for media
         * mounted read-only - the write-protect tab, which is the status a
         * driver is entitled to consult before it ever issues a write, and
         * which this ROM's own drivers do consult (one SENSE DRIVE STATUS
         * precedes every traced READ DATA and WRITE DATA). */
        uint8_t st3 = (fdc->cylinder == 0) ? 0x28 : 0x20;
        if (fdc->disk[fdc->unit] && !fdc->disk_rw[fdc->unit]) st3 |= 0x40;
        uint8_t res[1] = {st3};
        set_result(fdc, res, 1);
        break;
    }
    case 0x03: /* SPECIFY */
        fdc->phase = P2500_FDC_IDLE;
        fdc->result_len = 0;
        break;
    case 0x05: /* WRITE DATA */
        do_read_write(fdc, true);
        break;
    case 0x06: /* READ DATA */
        do_read_write(fdc, false);
        break;
    case 0x0D: /* FORMAT TRACK */
        do_format_track(fdc);
        break;
    default: {
        uint8_t res[1] = {0x80}; /* abnormal termination, invalid command */
        set_result(fdc, res, 1);
        break;
    }
    }
}

uint8_t p2500_fdc_read_data(P2500Fdc *fdc) {
    if (fdc->phase != P2500_FDC_RESULT || fdc->result_pos >= fdc->result_len) {
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "read past end of result phase");
        return 0x00;
    }
    uint8_t v = fdc->result[fdc->result_pos++];
    if (fdc->verbose)
        p2500_logf(fdc->log, P2500_LOG_TRACE, "fdc", "read result byte %zu/%zu = $%02X", fdc->result_pos, fdc->result_len, v);
    if (fdc->result_pos >= fdc->result_len) {
        fdc->phase = P2500_FDC_IDLE;
        /* Real hardware clears /INT once the host has read through the
         * result phase - the same read that hands back SENSE INTERRUPT
         * STATUS's ST0/PCN for the seek/recalibrate case, or the READ
         * DATA/etc. status bytes otherwise. */
        fdc->int_line = false;
    }
    return v;
}

void p2500_fdc_write_data(P2500Fdc *fdc, uint8_t value) {
    if (fdc->phase == P2500_FDC_IDLE) {
        const CmdInfo *info = find_command(value);
        fdc->command[0] = value;
        fdc->command_len = 1;
        fdc->command_expected = info ? info->total_len : 1;
        if (fdc->command_expected <= 1) {
            finish_command(fdc);
        } else {
            fdc->phase = P2500_FDC_COMMAND;
        }
    } else if (fdc->phase == P2500_FDC_COMMAND) {
        if (fdc->command_len < sizeof(fdc->command))
            fdc->command[fdc->command_len++] = value;
        if (fdc->command_len >= fdc->command_expected)
            finish_command(fdc);
    } else if (fdc->verbose) {
        p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "unexpected write $%02X while phase=%d", value, fdc->phase);
    }
}
