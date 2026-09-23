#ifndef P2500_MACHINE_H
#define P2500_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include "vendor/superzazu_z80/z80.h"
#include "fdc.h"
#include "sesam.h"
#include "pio.h"
#include "dma.h"
#include "ctc.h"

/*
 * P2500 CPU-card machine model. Port $05 bank-switches the low 4KB between
 * the IPL EPROM and RAM (see TODO.md T1): the EPROM image lives in its own
 * 4KB array and is selected on read only; writes to $0000-$0FFF always
 * land in the 64K RAM image, so the ROM's own RAM test (which banks the
 * EPROM out first) works without erasing it.
 *
 * Port dispatch, reverse-engineered from the ROM itself (see ../TODO.md):
 *   $00-$03  Z80A-CTC, one channel per port (HWTEST V100; TODO.md T16) -
 *            not touched by the IPL, needed once CP/M's CBIOS starts
 *            bit-banging the keyboard/printer serial lines against it
 *   $05      bank select ($07 normal, $0F EPROM-out/RAM-in, $00 video RAM
 *            window at $8000-$BFFF - the video-bank distinction isn't
 *            modeled yet, see TODO.md T1 "Extra")
 *   $08/$09  MC6845 CRTC (index/data)
 *   $0F      SESAM port (dongle / bootable-cartridge probe)
 *   $10/$11  Z80A-PIO data registers (Port A/B)
 *   $12/$13  Z80A-PIO control registers (Port A/B)
 *   $14/$15  uPD765 FDC (main status / data)
 *   $16      Z80A-DMA register-load stream
 *   $0A      diagnostic/POST latch, logged only (TODO.md T15)
 *
 * See ../README.md and ROM Dumps/CPU-Card-Boot-EPROM/emulation/findings.md
 * (in the parent research project) for the reverse-engineering this is
 * built on.
 */

#define P2500_RAM_SIZE 0x10000
#define P2500_EPROM_SIZE 0x1000

/* Which Z80A-PIO Port A bit the FDD card's uPD765 INT line is wired to -
 * not yet confirmed by hardware tracing (TODO.md T4), kept as a single
 * named constant so it can be flipped in one place. */
#define P2500_FDC_PIO_PORT P2500_PIO_PORT_A
#define P2500_FDC_PIO_BIT 0

typedef struct {
    z80 cpu;
    uint8_t ram[P2500_RAM_SIZE];
    uint8_t eprom[P2500_EPROM_SIZE];
    uint8_t bank; /* last value written to port $05 */

    P2500Fdc fdc;
    P2500Sesam sesam;
    P2500Pio pio;
    P2500Dma dma;
    P2500Ctc ctc;

    /* MC6845 CRTC: 16 8-bit registers, selected by $08, read/written via $09 */
    uint8_t crtc_regs[16];
    uint8_t crtc_index;

    bool verbose_unknown_ports;
    unsigned long total_instructions;
} P2500Machine;

void p2500_init(P2500Machine *m);
bool p2500_load_rom(P2500Machine *m, const char *path);
void p2500_step(P2500Machine *m);
/* Bank-aware read of `addr`, matching what the CPU core itself would see -
 * for diagnostics that want to look at low memory without a live z80. */
uint8_t p2500_peek(const P2500Machine *m, uint16_t addr);

#endif
