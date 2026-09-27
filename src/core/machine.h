#ifndef P2500_MACHINE_H
#define P2500_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vendor/superzazu_z80/z80.h"
#include "fdc.h"
#include "sesam.h"
#include "pio.h"
#include "dma.h"
#include "ctc.h"
#include "keyboard.h"
#include "intctl.h"

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
 *   $04      serial TX data bit (write; CBIOS bit-bangs it against CTC
 *            channel 0 - see ctc.h)
 *   $06      keyboard byte (read-only, one whole byte per CTC channel-3
 *            strobe - see keyboard.h)
 *   $05      write: bank select ($07 normal, $0F EPROM-out/RAM-in, $00
 *            video RAM window at $8000-$BFFF - the video-bank distinction
 *            isn't modeled yet, see TODO.md T1 "Extra")
 *            read:  serial input lines. Bit 7 is RXD, sampled once per bit
 *            cell by the CTC channel-1 receive ISR; bit 6 is a transmit
 *            handshake/ready input the channel-0 transmit ISR waits on
 *            before shifting a byte out. See TODO.md T20 and ctc.h.
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

/* The video card carries its own DRAM bank, and the port $05 latch gates
 * $8000-$BFFF between it and main DRAM (TODO.md T1 "Extra", and
 * ../Tracing/P2500-predicted-wiring-from-firmware.md C3: "latch bits 0-2
 * gate the $8000-$BFFF window between main DRAM and the video card's DRAM,
 * all-clear = video").
 *
 * The four values ever written agree with that reading exactly:
 *   $07  bits 0-2 set   -> main DRAM, EPROM in    (IPL normal)
 *   $0F  bits 0-2 set   -> main DRAM, EPROM out   (CP/M normal, 617x/run)
 *   $00  bits 0-2 clear -> video DRAM, EPROM in   (IPL banner + VRAM clear)
 *   $08  bits 0-2 clear -> video DRAM, EPROM out  (CBIOS console output)
 *
 * Modelling the two as one array happened to work for the IPL and for CP/M,
 * because neither keeps anything it cares about at $8000-$BFFF. It does not
 * work for the UCSD p-System, which loads its bootstrap straight into that
 * range - so its "screen" was main RAM being rendered as if it were video. */
#define P2500_VRAM_SIZE 0x4000
#define P2500_VRAM_BASE 0x8000
#define P2500_BANK_VIDEO_MASK 0x07 /* all clear selects the video card's DRAM */

/* The video card's DRAM is 16K words x 12 bits, not a flat 16 KB byte bank
 * (TODO.md T27). Four independent facts agree:
 *
 *   - The card carries exactly 12 MB8116E (16 Kbit x 1) parts, i.e. 12
 *     one-bit planes 16K deep (../Actual P2500 hardware/P2500 Video Card/).
 *   - The $8000-$BFFF window is 16 KB, which is 16K addresses - one per
 *     word. Eight of the twelve planes are the byte the CPU sees.
 *   - The P2219 CP/M manual documents exactly four screen attributes:
 *     underline, reverse, flash and low intensity. Four planes, four
 *     attributes.
 *   - The same manual's graphics mode is 512x256 addressable dots, which is
 *     131,072 bits = exactly the 16 KB the eight character-code planes hold.
 *
 * So each cell is an 8-bit character code plus a 4-bit attribute nibble.
 * The nibble plane is modelled here, but NOTHING WRITES IT: how the CPU
 * reaches it is not derivable from anything this project holds, because no
 * software this project holds ever sets an attribute. Every OUT ($05) in
 * the whole disk corpus writes $00, $07, $08 or $0F and nothing else, and
 * the live CP/M system's bank shadow at $EB14 only ever takes $08 (video
 * in) and $0F (video out). The selector is out-of-band and port $05's six
 * unused bits-0-2 combinations are the obvious candidate, but that is a
 * guess and this emulator does not make it. See p2500_bank_is_unknown(). */
#define P2500_VRAM_ATTR_MASK 0x0F

/* Z8400A (Z80A) at 4 MHz - confirmed from the real CPU card's silicon, see
 * ../P2500-general-findings.md. Everything time-based in this emulator is
 * derived from this one number and the CPU core's T-state counter; there
 * are no independent timing constants. */
#define P2500_CPU_HZ 4000000u

/* CTC channel 2's CLK/TRG source: a periodic external strobe that CBIOS's
 * channel-2 ISR turns into the 24-bit real-time tick counter at $F436 (see
 * ctc.h). WHAT ACTUALLY DRIVES IT IS NOT KNOWN - on a Philips machine the
 * two candidates are a 50 Hz mains-derived pulse and the video card's frame
 * rate, and both are 50 Hz, which is why that is the value used here. It is
 * an assumption, named in one place, and on ROADMAP.md's hardware
 * measurement list; nothing else in the model depends on it. */
#define P2500_CLOCK_TICK_HZ 50u

/* How often a queued --type byte is offered to CTC channel 3 (see
 * advance_keyboard_strobe in machine.c). This models the *user*, not the
 * keyboard: on real hardware a keypress that arrives before CBIOS has armed
 * channel 3 is simply lost, and the person presses the key again. Offering
 * a pending byte at a steady human-plausible rate reproduces that without
 * needing to know when the machine started listening. */
#define P2500_KEYSTROKE_HZ 100u

/* Which bits of port $05 read back as what (TODO.md T20). */
#define P2500_PORT05_RXD_BIT 7
#define P2500_PORT05_TX_READY_BIT 6

/* Which CTC channel each external strobe is wired to - see ctc.h for the
 * evidence behind each one. */
#define P2500_CTC_SERIAL_RX_CHANNEL 1
#define P2500_CTC_CLOCK_TICK_CHANNEL 2
#define P2500_CTC_KEYBOARD_CHANNEL 3

/* Which Z80A-PIO Port A bit the FDD card's uPD765 INT line is wired to -
 * not yet confirmed by hardware tracing (TODO.md T4), kept as a single
 * named constant so it can be flipped in one place. */
#define P2500_FDC_PIO_PORT P2500_PIO_PORT_A
#define P2500_FDC_PIO_BIT 0

typedef struct {
    z80 cpu;
    uint8_t ram[P2500_RAM_SIZE];
    uint8_t eprom[P2500_EPROM_SIZE];
    uint8_t vram[P2500_VRAM_SIZE]; /* the video card's own DRAM, see above */
    uint8_t vram_attr[P2500_VRAM_SIZE]; /* 4-bit attribute plane; see above */
    uint8_t bank; /* last value written to port $05 */
    unsigned long unknown_bank_writes; /* OUT ($05) values we cannot decode */

    P2500Fdc fdc;
    P2500Sesam sesam;
    P2500Pio pio;
    P2500Dma dma;
    P2500Ctc ctc;
    P2500Keyboard keyboard;
    P2500Serial serial;

    /* MC6845 CRTC: 18 8-bit registers R0-R17, selected by $08, read/written
     * via $09. It really is 18, not 16 (TODO.md T26): masking the index to
     * 4 bits aliased R16/R17, the light-pen registers, onto R0/R1, the
     * horizontal total and displayed counts that the geometry depends on. */
    uint8_t crtc_regs[18];
    uint8_t crtc_index;

    /* IM2 daisy chain (TODO.md T17). `int_offered` is the source whose
     * vector is currently sitting in the CPU core's single pending slot, or
     * -1; the core clearing int_pending is how we learn it was taken. */
    P2500IntCtl intctl;
    int int_offered;

    /* Serial input lines read back on port $05 (TODO.md T20). Idle high:
     * RXD marking, and the transmit handshake asserting "ready", which is
     * what an unplugged line with a pull-up looks like. */
    bool serial_rxd;
    bool serial_tx_ready;

    /* CTC channel 2's assumed 50 Hz strobe (see P2500_CLOCK_TICK_HZ). */
    uint32_t clock_strobe_phase;
    bool clock_strobe_level;

    /* CTC channel 3's keyboard "byte ready" strobe. */
    uint32_t keyboard_strobe_phase;

    bool verbose_unknown_ports;
    unsigned long total_instructions;
} P2500Machine;

void p2500_init(P2500Machine *m);
bool p2500_load_rom(P2500Machine *m, const char *path);
void p2500_step(P2500Machine *m);
/* Bank-aware read of `addr`, matching what the CPU core itself would see -
 * for diagnostics that want to look at low memory without a live z80. */
uint8_t p2500_peek(const P2500Machine *m, uint16_t addr);
/* True while port $05's latch maps the video card's DRAM into
 * $8000-$BFFF instead of main DRAM. */
bool p2500_video_window_selected(const P2500Machine *m);

/* True for any port-$05 value this emulator cannot account for. Bits 0-2
 * select the $8000-$BFFF window and only "all set" (main DRAM) and "all
 * clear" (video DRAM) have ever been observed; the other six combinations
 * are unaccounted for and are where the video card's attribute plane most
 * likely lives (TODO.md T27). Such a write is currently routed to main DRAM,
 * which would be silently wrong - so it is counted and logged instead. */
bool p2500_bank_is_unknown(const P2500Machine *m);

#endif
