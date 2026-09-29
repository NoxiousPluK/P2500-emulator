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
    {0x06, "READ DATA", 9},
    {0x07, "RECALIBRATE", 2},
    {0x08, "SENSE INTERRUPT STATUS", 1},
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
     * command's parameter writes). EXM (bit 5, "execution phase, non-DMA
     * transfer in progress") is not modeled: this FDC's READ DATA hands
     * its whole block to the DMA in one memcpy (dma.c) rather than
     * byte-at-a-time through port $15, so there is no CPU-visible window
     * where EXM would read 1. */
    uint8_t cb = (fdc->phase != P2500_FDC_IDLE) ? 1 : 0;
    return (uint8_t)((rqm << 7) | (dio << 6) | (cb << 4));
}

static void fire_interrupt(P2500Fdc *fdc) {
    fdc->int_line = true;
    if (fdc->on_interrupt) fdc->on_interrupt(fdc->interrupt_userdata);
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

static void do_read_data(P2500Fdc *fdc) {
    /* command[0]=opcode, [1]=unit/head, [2]=C, [3]=H, [4]=R, [5]=N, ... */
    uint8_t c = fdc->command[2];
    uint8_t r = fdc->command[4];
    /* The P2500/P2000M disk format writes the on-disk track ID one higher
     * than the physical track. The ROM issues C matching what's recorded
     * on the disk (the logical ID), so the physical track - and this flat
     * .raw dump's own track order - is C-1, not C. C=1,R=1 (the IPL's
     * first read) resolves to byte offset 0, the boot sector. */
    size_t physical_track = c > 0 ? (size_t)(c - 1) : 0;
    size_t lba = physical_track * P2500_FDC_SECTORS_PER_TRACK + (r - 1);
    size_t off = lba * P2500_FDC_SECTOR_SIZE;

    const uint8_t *media = fdc->disk[fdc->unit];
    const size_t media_size = fdc->disk_size[fdc->unit];
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
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "READ DATA unit %u C=%u R=%u: drive not ready",
                       fdc->unit, c, r);
        uint8_t res[7] = {0x48, 0x00, 0x00, c, fdc->command[3], r, fdc->command[5]};
        set_result(fdc, res, 7);
        fire_interrupt(fdc);
        return;
    }
    if (off + P2500_FDC_SECTOR_SIZE > media_size) {
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "READ DATA C=%u R=%u out of range (off=0x%zx)",
                       c, r, off);
        uint8_t res[7] = {0x40, 0x00, 0x00, c, fdc->command[3], r, fdc->command[5]};
        set_result(fdc, res, 7);
        fire_interrupt(fdc);
        return;
    }

    if (fdc->ram && fdc->dma) {
        size_t avail = media_size - off;
        p2500_dma_deliver(fdc->dma, fdc->ram, media + off, avail);
        if (fdc->verbose)
            p2500_logf(fdc->log, P2500_LOG_TRACE, "fdc", "READ DATA C=%u R=%u -> disk offset 0x%zx, "
                               "handed to DMA", c, r, off);
    } else if (fdc->verbose) {
        p2500_logf(fdc->log, P2500_LOG_WARN, "fdc", "READ DATA C=%u R=%u -> sector ready but no "
                           "RAM/DMA wired up, dropped", c, r);
    }

    uint8_t res[7] = {0x00, 0x00, 0x00, c, fdc->command[3], (uint8_t)(r + 1), fdc->command[5]};
    set_result(fdc, res, 7);
    fire_interrupt(fdc);
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
    if (opcode == 0x07 || opcode == 0x0F || opcode == 0x06)
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
        uint8_t st3 = (fdc->cylinder == 0) ? 0x28 : 0x20;
        uint8_t res[1] = {st3};
        set_result(fdc, res, 1);
        break;
    }
    case 0x03: /* SPECIFY */
        fdc->phase = P2500_FDC_IDLE;
        fdc->result_len = 0;
        break;
    case 0x06: /* READ DATA */
        do_read_data(fdc);
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
