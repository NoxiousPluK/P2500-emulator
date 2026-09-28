#include "dma.h"
#include <stdio.h>
#include <string.h>

static const char *role_name(P2500DmaByteRole role) {
    switch (role) {
    case P2500_DMA_NEXT_BASE: return "base";
    case P2500_DMA_NEXT_A_ADDR_LO: return "PortA addr lo";
    case P2500_DMA_NEXT_A_ADDR_HI: return "PortA addr hi";
    case P2500_DMA_NEXT_BLOCKLEN_LO: return "block length lo";
    case P2500_DMA_NEXT_BLOCKLEN_HI: return "block length hi";
    case P2500_DMA_NEXT_WR1_TIMING: return "WR1 timing byte";
    case P2500_DMA_NEXT_WR2_TIMING: return "WR2 timing byte";
    case P2500_DMA_NEXT_WR3_MASK: return "WR3 mask byte";
    case P2500_DMA_NEXT_WR3_MATCH: return "WR3 match byte";
    case P2500_DMA_NEXT_B_ADDR_LO: return "PortB addr lo";
    case P2500_DMA_NEXT_B_ADDR_HI: return "PortB addr hi";
    case P2500_DMA_NEXT_INT_CTRL: return "interrupt control byte";
    case P2500_DMA_NEXT_PULSE_CTRL: return "pulse control byte";
    case P2500_DMA_NEXT_VECTOR: return "interrupt vector";
    }
    return "?";
}

void p2500_dma_init(P2500Dma *dma) {
    /* Re-initialising a device must not silently take its diagnostics
     * away with it: a caller that swaps a keystroke queue in, as the
     * CLI does, would otherwise lose every message the device had to
     * make from then on - a silent drop, which is the failure mode
     * this project has paid for most often (TODO.md T34). */
    const P2500Log *log = dma->log;
    memset(dma, 0, sizeof(*dma));
    dma->log = log;
}

static void queue_push(P2500Dma *dma, P2500DmaByteRole role) {
    if (dma->queue_len >= P2500_DMA_QUEUE_CAP) {
        if (dma->verbose)
            p2500_logf(dma->log, P2500_LOG_WARN, "dma", "follow-byte queue overflow, dropping role %s",
                       role_name(role));
        return;
    }
    dma->queue[dma->queue_len++] = role;
}

/* Pops and returns the front of the queue, shifting the rest down. Small
 * and rare enough (at most a handful of entries) that O(n) is fine. */
static P2500DmaByteRole queue_pop(P2500Dma *dma) {
    P2500DmaByteRole role = dma->queue[0];
    for (int i = 1; i < dma->queue_len; i++) dma->queue[i - 1] = dma->queue[i];
    dma->queue_len--;
    return role;
}

/* Any command that resets the chip or disables its interrupts also clears
 * a pending/in-service end-of-block interrupt, releasing the DMA's place in
 * the IM2 daisy chain (see intctl.h). This is how the IPL's handlers get
 * away with never executing RETI: the floppy driver's own register streams
 * send WR6 $A3 ("reset and disable interrupts") constantly. */
static void notify_int_reset(P2500Dma *dma) {
    if (dma->on_int_reset) dma->on_int_reset(dma->interrupt_userdata);
}

static void execute_command(P2500Dma *dma, uint8_t value) {
    switch (value) {
    case 0xC3: /* Reset */
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "RESET");
        dma->loaded = false;
        dma->interrupts_enabled = false;
        dma->dma_enabled = false;
        notify_int_reset(dma);
        break;
    case 0xCF: /* Load */
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "LOAD");
        dma->loaded = true;
        break;
    case 0xAB: /* Enable interrupts */
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "ENABLE INTERRUPTS");
        dma->interrupts_enabled = true;
        break;
    case 0xAF: /* Disable interrupts */
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "DISABLE INTERRUPTS");
        dma->interrupts_enabled = false;
        notify_int_reset(dma);
        break;
    case 0x87: /* Enable DMA */
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "ENABLE DMA");
        dma->dma_enabled = true;
        break;
    case 0x83: /* Disable DMA */
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "DISABLE DMA");
        dma->dma_enabled = false;
        break;
    case 0xA3: /* Reset and Disable Interrupts */
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "RESET AND DISABLE INTERRUPTS");
        dma->interrupts_enabled = false;
        notify_int_reset(dma);
        break;
    case 0xC7: /* Reset Port A Timing */
    case 0xC8: /* Reset Port B Timing */
    case 0xD3: /* Continue */
    case 0xB7: /* Enable after RETI */
    case 0xBF: /* Read Status Byte */
    case 0x8B: /* Reinitialize Status Byte */
    case 0xA7: /* Initialize Read Sequence */
    case 0xB3: /* Force Ready */
    case 0xBB: /* Read Mask Follows */
        /* Recognized (per the Z80 Family CPU Peripherals User Manual's
         * WR6 command table) but not modeled - nothing this project has
         * traced ever sends these, and none of them affect the fields
         * this emulator's READ DATA path actually needs. */
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_WARN, "dma", "WR6 command $%02X (recognized, not modeled)", value);
        break;
    default:
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_WARN, "dma", "unrecognized WR6 command $%02X", value);
        break;
    }
}

/* Decodes a fresh base register byte at the top level: identifies which
 * WR group it belongs to (see dma.h's big comment for the exact bit
 * patterns and their manual citation), applies whatever of its bits are
 * immediate data, and queues whichever follow-up bytes its pointer bits
 * request, in the group's documented order. */
static void decode_base_register(P2500Dma *dma, uint8_t value) {
    if ((value & 0x83) == 0x83) { /* WR6: D7,D1,D0 = 1,1,1 */
        execute_command(dma, value);
        return;
    }
    if ((value & 0x80) == 0x00 && (value & 0x03) != 0x00) { /* WR0 */
        dma->direction = (value & 0x04) ? P2500_DMA_DIR_IO_TO_MEMORY
                                         : P2500_DMA_DIR_MEMORY_TO_IO;
        if (dma->verbose)
            p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "WR0 base $%02X: direction=%s", value,
                       dma->direction == P2500_DMA_DIR_IO_TO_MEMORY ? "IO->memory (READ)"
                                                                     : "memory->IO (WRITE)");
        if (value & 0x08) queue_push(dma, P2500_DMA_NEXT_A_ADDR_LO);
        if (value & 0x10) queue_push(dma, P2500_DMA_NEXT_A_ADDR_HI);
        if (value & 0x20) queue_push(dma, P2500_DMA_NEXT_BLOCKLEN_LO);
        if (value & 0x40) queue_push(dma, P2500_DMA_NEXT_BLOCKLEN_HI);
        return;
    }
    if ((value & 0x87) == 0x04) { /* WR1: D7,D2,D1,D0 = 0,1,0,0 */
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "WR1 base $%02X (Port A device/timing)", value);
        if (value & 0x40) queue_push(dma, P2500_DMA_NEXT_WR1_TIMING);
        return;
    }
    if ((value & 0x87) == 0x00) { /* WR2: D7,D2,D1,D0 = 0,0,0,0 */
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "WR2 base $%02X (Port B device/timing)", value);
        if (value & 0x40) queue_push(dma, P2500_DMA_NEXT_WR2_TIMING);
        return;
    }
    if ((value & 0x83) == 0x81) { /* WR4: D7,D1,D0 = 1,0,1 */
        static const char *modes[4] = {"Byte", "Continuous", "Burst", "Do Not Program"};
        if (dma->verbose)
            p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "WR4 base $%02X (mode=%s)", value, modes[(value >> 5) & 3]);
        if ((value & 0x0C) == 0x0C) { /* D2 and D3 both set */
            queue_push(dma, P2500_DMA_NEXT_B_ADDR_LO);
            queue_push(dma, P2500_DMA_NEXT_B_ADDR_HI);
        }
        if (value & 0x10) queue_push(dma, P2500_DMA_NEXT_INT_CTRL);
        return;
    }
    if ((value & 0xC7) == 0x82) { /* WR5: D7,D6,D2,D1,D0 = 1,0,0,1,0 */
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "WR5 base $%02X (Ready/CE/EOB behavior)", value);
        return;
    }
    if ((value & 0x83) == 0x80) { /* WR3: D7,D1,D0 = 1,0,0 - see dma.h note */
        if (dma->verbose)
            p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "WR3 base $%02X (stop-on-match=%d, int-enable=%d, dma-enable=%d)",
                       value, (value >> 2) & 1, (value >> 5) & 1, (value >> 6) & 1);
        if (value & 0x20) dma->interrupts_enabled = true;
        if (value & 0x40) dma->dma_enabled = true;
        if (value & 0x08) queue_push(dma, P2500_DMA_NEXT_WR3_MASK);
        if (value & 0x10) queue_push(dma, P2500_DMA_NEXT_WR3_MATCH);
        return;
    }
    if (dma->verbose)
        p2500_logf(dma->log, P2500_LOG_WARN, "dma", "unrecognized base register byte $%02X", value);
}

/* Handles one follow-up data byte per its queued role. Some roles (the
 * two-byte fields) update dma's own state directly; P2500_DMA_NEXT_INT_CTRL
 * additionally decodes its own bits to queue more follow-bytes, exactly
 * like a base register byte does - see dma.h. */
static void handle_follow_byte(P2500Dma *dma, P2500DmaByteRole role, uint8_t value) {
    switch (role) {
    case P2500_DMA_NEXT_A_ADDR_LO:
    case P2500_DMA_NEXT_A_ADDR_HI:
    case P2500_DMA_NEXT_WR1_TIMING:
    case P2500_DMA_NEXT_WR2_TIMING:
    case P2500_DMA_NEXT_WR3_MASK:
    case P2500_DMA_NEXT_WR3_MATCH:
    case P2500_DMA_NEXT_PULSE_CTRL:
        /* Parsed correctly (keeps the stream in sync) but not modeled -
         * see dma.h's "Not implemented" note. */
        if (dma->verbose)
            p2500_logf(dma->log, P2500_LOG_WARN, "dma", "%s = $%02X (not modeled)", role_name(role), value);
        break;
    case P2500_DMA_NEXT_BLOCKLEN_LO:
        dma->block_length_raw = (uint16_t)((dma->block_length_raw & 0xFF00) | value);
        dma->block_length = (uint16_t)(dma->block_length_raw + 1);
        break;
    case P2500_DMA_NEXT_BLOCKLEN_HI:
        dma->block_length_raw = (uint16_t)((dma->block_length_raw & 0x00FF) | ((uint16_t)value << 8));
        dma->block_length = (uint16_t)(dma->block_length_raw + 1);
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "block length = %u bytes", dma->block_length);
        break;
    case P2500_DMA_NEXT_B_ADDR_LO:
        dma->port_b_addr = (uint16_t)((dma->port_b_addr & 0xFF00) | value);
        break;
    case P2500_DMA_NEXT_B_ADDR_HI:
        dma->port_b_addr = (uint16_t)((dma->port_b_addr & 0x00FF) | ((uint16_t)value << 8));
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "Port B (RAM) address = $%04X", dma->port_b_addr);
        break;
    case P2500_DMA_NEXT_INT_CTRL:
        if (dma->verbose)
            p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "interrupt control byte = $%02X (on-RDY=%d on-match=%d "
                               "on-EOB=%d status-affects-vector=%d)",
                       value, (value >> 6) & 1, value & 1, (value >> 1) & 1, (value >> 5) & 1);
        if ((value & 0x0C) == 0x0C) queue_push(dma, P2500_DMA_NEXT_PULSE_CTRL);
        if (value & 0x10) queue_push(dma, P2500_DMA_NEXT_VECTOR);
        break;
    case P2500_DMA_NEXT_VECTOR:
        dma->vector = value;
        if (dma->verbose) p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "interrupt vector = $%02X", dma->vector);
        break;
    case P2500_DMA_NEXT_BASE:
        break; /* unreachable - queue never holds this role */
    }
}

void p2500_dma_write(P2500Dma *dma, uint8_t value) {
    if (dma->queue_len == 0) {
        decode_base_register(dma, value);
        return;
    }
    P2500DmaByteRole role = queue_pop(dma);
    handle_follow_byte(dma, role, value);
}

void p2500_dma_deliver(P2500Dma *dma, uint8_t *ram, const uint8_t *src, size_t len) {
    if (!dma->dma_enabled || dma->direction != P2500_DMA_DIR_IO_TO_MEMORY) {
        if (dma->verbose)
            p2500_logf(dma->log, P2500_LOG_WARN, "dma", "deliver requested but DMA not enabled for READ - dropped");
        return;
    }
    if (len > dma->block_length) len = dma->block_length;
    size_t room = (size_t)(0x10000 - dma->port_b_addr);
    if (len > room) len = room;

    memcpy(&ram[dma->port_b_addr], src, len);
    if (dma->verbose)
        p2500_logf(dma->log, P2500_LOG_TRACE, "dma", "delivered %zu bytes to RAM $%04X", len, dma->port_b_addr);

    if (dma->interrupts_enabled && dma->on_interrupt)
        dma->on_interrupt(dma->interrupt_userdata, dma->vector);
}
