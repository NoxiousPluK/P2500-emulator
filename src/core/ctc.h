#ifndef P2500_CTC_H
#define P2500_CTC_H

#include <stdint.h>

#include "log.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Z80A-CTC (Z8430) model for the CPU card's ports $00-$03 - four
 * independently programmable counter/timer channels, one per port
 * (channel = port & 3, i.e. CS1/CS0 tied to A1/A0). The chip itself is
 * confirmed from real silicon: a Z8430A sits on the CPU card next to the
 * Z8400A (see ../P2500-general-findings.md).
 *
 * Modeled from "Zilog Z80 Family CPU Peripherals User Manual"
 * (UM008101-0601), chapter "Counter/Timer Channels": control-word bit
 * layout (Table 2), interrupt vector formation (Table 3), channel select
 * convention (Table 1).
 *
 * TIMING IS REAL, NOT SCALED (TODO.md T18). The down-counter is driven by
 * T-states from the CPU core's own cycle counter through the actual
 * prescaler (divide by 16 or 256), so a channel's period is
 * prescaler x time_constant T-states exactly as on hardware. There is no
 * tick multiplier and no "close enough" constant.
 *
 * That this is right is checkable rather than asserted: CBIOS's baud-rate
 * table at $F546 is seven {control, time constant} pairs, and read through
 * this model at the confirmed 4 MHz they come out as
 *   /256 x 208 = 75.1   /256 x 142 = 110.0  /256 x 104 = 150.2
 *   /256 x  52 = 300.5  /256 x  26 = 601.0  /16  x 208 = 1201.9
 *   /16  x 104 = 2403.8 baud
 * i.e. the standard rates 75/110/150/300/600/1200/2400, each within 0.3%.
 * Fold a prescaler wrong, or tick per instruction instead of per T-state,
 * and that table decodes to nothing.
 *
 * WHAT EACH CHANNEL IS WIRED TO (TODO.md T19). Established by reading
 * CBIOS's own four ISRs out of a live RAM dump (tools/disasm_ram.sh), via
 * its IM2 table at $FF90 = $FF0C/$FF15/$FF03/$FEFA, which trampoline to
 * $F597/$F669/$F37F/$ED2A respectively:
 *
 *   ch0  TIMER, control (baud table byte)|$07, tc from the table. Its
 *        interrupt is enabled only while a byte is being transmitted
 *        ($F57F). Handler $F597 is the serial TRANSMIT bit clock - it
 *        checks the port $05 bit 6 handshake and shifts bits out of
 *        port $04. TIMER mode takes no CLK/TRG input at all.
 *   ch1  COUNTER, $C7 (falling edge), tc=1 - so it fires on the first
 *        falling edge of its CLK/TRG pin, which is the serial RECEIVE
 *        line's start bit. Handler $F669 then reprograms *channel 1
 *        itself* as a TIMER ($87 | table byte) with half a bit time, then
 *        full bit times, sampling port $05 bit 7 into a shift register at
 *        $F73D once per bit, and finally restores $C7/tc=1 to wait for the
 *        next start bit. So ch1's CLK/TRG is the RXD line.
 *   ch2  COUNTER, $D5 (rising edge), tc=1. Handler $F37F -> $F382
 *        increments a 24-bit tick counter at $F436 and runs deferred
 *        callbacks: this is the real-time clock tick. Its CLK/TRG source
 *        is an unidentified periodic signal - see machine.h.
 *   ch3  COUNTER, $C5 (falling edge), tc=1. Handler $ED2A -> $ED2D reads
 *        a whole byte from port $06 into a circular buffer, so ch3's
 *        CLK/TRG is the keyboard controller's "byte ready" strobe.
 *
 * This replaces the earlier model in which channel 0's ZC/TO was chained
 * into channels 1-3's CLK/TRG. That chain is not what the hardware does:
 * it cannot be, because ch0's rate is whatever baud rate the user
 * configured, ch1 reprograms itself out of COUNTER mode mid-byte, and ch2
 * counts the opposite clock edge from ch1 and ch3. A COUNTER-mode channel
 * whose real source this emulator cannot supply simply never ticks - it is
 * not fed a substitute pulse.
 */

#define P2500_CTC_CHANNELS 4

typedef enum {
    P2500_CTC_WAIT_CONTROL,
    P2500_CTC_WAIT_TIME_CONSTANT,
} P2500CtcState;

typedef void (*P2500CtcInterruptCallback)(void *userdata, int channel, uint8_t vector);
/* Called when a channel is software-reset or has its interrupt disabled:
 * that releases the channel's place in the IM2 daisy chain (see
 * intctl.h). */
typedef void (*P2500CtcResetCallback)(void *userdata, int channel);

typedef struct {
    P2500CtcState state[P2500_CTC_CHANNELS];

    bool int_enabled[P2500_CTC_CHANNELS];
    bool counter_mode[P2500_CTC_CHANNELS];   /* false = TIMER, true = COUNTER */
    bool prescaler_256[P2500_CTC_CHANNELS];  /* false = /16, true = /256; TIMER mode only */
    bool rising_edge[P2500_CTC_CHANNELS];    /* which CLK/TRG edge COUNTER mode counts */
    bool auto_trigger[P2500_CTC_CHANNELS];   /* true = start as soon as the time constant loads */
    bool started[P2500_CTC_CHANNELS];        /* has a time constant been loaded at least once */

    uint8_t time_constant[P2500_CTC_CHANNELS];
    uint16_t counter[P2500_CTC_CHANNELS];    /* live down-counter, 1..256 */
    uint32_t prescale_acc[P2500_CTC_CHANNELS]; /* T-states toward the next decrement */
    bool clk_trg[P2500_CTC_CHANNELS];        /* modeled CLK/TRG pin level */

    uint8_t vector_base; /* shared per-chip; loadable from channel 0 only */
    bool vector_set;

    /* Diagnostics sink, pointed at the machine's own by p2500_init().
     * NULL is fine - the messages are simply discarded (TODO.md T34). */
    const P2500Log *log;
    P2500CtcInterruptCallback on_interrupt;
    P2500CtcResetCallback on_reset;
    void *interrupt_userdata;
    bool verbose;
} P2500Ctc;

void p2500_ctc_init(P2500Ctc *ctc);
void p2500_ctc_write(P2500Ctc *ctc, int channel, uint8_t value); /* ports $00-$03 out */
uint8_t p2500_ctc_read(P2500Ctc *ctc, int channel);              /* ports $00-$03 in */

/* Advance every TIMER-mode channel by `tstates` system clock cycles,
 * through its prescaler. COUNTER-mode channels are untouched - they only
 * move when their CLK/TRG pin does. */
void p2500_ctc_tick(P2500Ctc *ctc, uint32_t tstates);

/* Drive one channel's CLK/TRG pin. A COUNTER-mode channel counts down on
 * whichever edge its control word selected. */
void p2500_ctc_set_clk_trg(P2500Ctc *ctc, int channel, bool level);

#ifdef __cplusplus
}
#endif

#endif
