#include "dma.h"
#include <stdio.h>
#include <string.h>

void p2500_dma_init(P2500Dma *dma) {
    memset(dma, 0, sizeof(*dma));
}

static void decode_direction(P2500Dma *dma) {
    uint8_t b2 = dma->template_bytes[2], b3 = dma->template_bytes[3];
    if (b2 == 0x11 && b3 == 0x6D) dma->direction = P2500_DMA_DIR_IO_TO_MEMORY;
    else if (b2 == 0xCF && b3 == 0x69) dma->direction = P2500_DMA_DIR_MEMORY_TO_IO;
    else if (dma->verbose)
        fprintf(stderr, "[dma] unrecognized direction bytes $%02X $%02X\n", b2, b3);
}

static void decode_block_length(P2500Dma *dma) {
    uint16_t raw = (uint16_t)(dma->template_bytes[5] | (dma->template_bytes[6] << 8));
    dma->block_length = (uint16_t)(raw + 1); /* WR0 programs count-1 */
    if (dma->verbose)
        fprintf(stderr, "[dma] block length = %u bytes\n", dma->block_length);
}

static void decode_port_b_addr(P2500Dma *dma) {
    dma->port_b_addr = (uint16_t)(dma->template_bytes[12] | (dma->template_bytes[13] << 8));
    if (dma->verbose)
        fprintf(stderr, "[dma] Port B (RAM) address = $%04X\n", dma->port_b_addr);
}

static void decode_vector(P2500Dma *dma) {
    dma->vector = dma->template_bytes[15];
    if (dma->verbose)
        fprintf(stderr, "[dma] interrupt vector = $%02X\n", dma->vector);
}

static void execute_command(P2500Dma *dma, uint8_t value) {
    switch (value) {
    case 0xC3: /* Reset */
        if (dma->verbose) fprintf(stderr, "[dma] RESET\n");
        dma->loaded = false;
        dma->interrupts_enabled = false;
        dma->dma_enabled = false;
        break;
    case 0xCF: /* Load */
        if (dma->verbose) fprintf(stderr, "[dma] LOAD\n");
        dma->loaded = true;
        break;
    case 0xAB: /* Enable interrupts */
        if (dma->verbose) fprintf(stderr, "[dma] ENABLE INTERRUPTS\n");
        dma->interrupts_enabled = true;
        break;
    case 0xAF: /* Disable interrupts */
        if (dma->verbose) fprintf(stderr, "[dma] DISABLE INTERRUPTS\n");
        dma->interrupts_enabled = false;
        break;
    case 0x87: /* Enable DMA */
        if (dma->verbose) fprintf(stderr, "[dma] ENABLE DMA\n");
        dma->dma_enabled = true;
        break;
    case 0x83: /* Disable DMA */
        if (dma->verbose) fprintf(stderr, "[dma] DISABLE DMA\n");
        dma->dma_enabled = false;
        break;
    default:
        if (dma->verbose) fprintf(stderr, "[dma] unrecognized WR6 command $%02X\n", value);
        break;
    }
}

void p2500_dma_write(P2500Dma *dma, uint8_t value) {
    if (dma->verbose)
        fprintf(stderr, "[dma] write $%02X at pos %d\n", value, dma->pos);
    /* Kept as permanent --verbose-io diagnostics, not a throwaway debug
     * print - this is what pinned down TODO.md ISSUE-2 (the READ DATA
     * DMA-program stream starting 3 bytes late) and is the cleanest way
     * to see a future template misalignment happen live again. */
    if (dma->pos == 0) {
        switch (value) {
        case 0xC3: case 0xCF: case 0xAB: case 0xAF: case 0x87: case 0x83:
            execute_command(dma, value);
            return;
        default:
            break;
        }
        dma->template_bytes[0] = value; /* WR0 command byte */
        dma->pos = 1;
        return;
    }

    dma->template_bytes[dma->pos] = value;

    if (dma->pos >= 17) {
        /* trailer: always the WR6 Load/EnableInterrupts/EnableDMA sequence
         * in every observed template */
        execute_command(dma, value);
        dma->pos = (dma->pos + 1) % 20;
        return;
    }

    switch (dma->pos) {
    case 3: decode_direction(dma); break;
    case 6: decode_block_length(dma); break;
    case 13: decode_port_b_addr(dma); break;
    case 15: decode_vector(dma); break;
    default: break;
    }

    dma->pos++;
}

void p2500_dma_deliver(P2500Dma *dma, uint8_t *ram, const uint8_t *src, size_t len) {
    if (!dma->dma_enabled || dma->direction != P2500_DMA_DIR_IO_TO_MEMORY) {
        if (dma->verbose)
            fprintf(stderr, "[dma] deliver requested but DMA not enabled for READ - dropped\n");
        return;
    }
    if (len > dma->block_length) len = dma->block_length;
    size_t room = (size_t)(0x10000 - dma->port_b_addr);
    if (len > room) len = room;

    memcpy(&ram[dma->port_b_addr], src, len);
    if (dma->verbose)
        fprintf(stderr, "[dma] delivered %zu bytes to RAM $%04X\n", len, dma->port_b_addr);

    if (dma->interrupts_enabled && dma->on_interrupt)
        dma->on_interrupt(dma->interrupt_userdata, dma->vector);
}
