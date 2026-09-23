#ifndef P2500_CTC_H
#define P2500_CTC_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Z80A-CTC (Z8430) model for the CPU card's ports $00-$03 - four
 * independently programmable counter/timer channels, one per port
 * (channel = port & 3, i.e. CS1/CS0 tied to A1/A0 - the standard Z80
 * system wiring documented for this chip; not separately confirmed on
 * this specific board, but there is no other sane way to wire four
 * same-purpose ports).
 *
 * This chip is NOT the one this project spent most of its early history
 * chasing at ports $10-$13 (that was a misidentification - see TODO.md
 * "1. Ports $10-$13 are a Z80A-PIO, not a Z80-CTC" and the deleted
 * ctc.{c,h} this file replaces). This is the *real* CTC, confirmed at
 * $00-$03 by HWTEST V100 (see TODO.md T16) and needed once CP/M's CBIOS
 * (SYSPBI.PHI) starts running - it bit-bangs the keyboard/printer serial
 * lines (port $04 TX, port $06 RX, 9600-8N-2) against this chip's
 * timing, and programs $00-$03 directly (confirmed live via
 * --verbose-io: repeated OUT to $00-$03 once CBIOS starts).
 *
 * Modeled from "Zilog Z80 Family CPU Peripherals User Manual"
 * (UM008101-0601), chapter "Counter/Timer Channels" - control-word bit
 * layout (Table 2), interrupt vector formation (Table 3), channel
 * select convention (Table 1) all taken directly from there, not
 * inferred from ROM behavior the way this project had to for the DMA.
 *
 * What's modeled: control word (interrupt enable, timer/counter mode,
 * prescaler value, clock edge, time-trigger, time-constant-follows,
 * software reset, control-vs-vector), the interrupt vector register
 * (shared per-chip, channel identifier bits auto-inserted per Table 3),
 * TIMER mode's down-counter with auto-reload and ZC/TO interrupt.
 *
 * What's NOT modeled: COUNTER mode's external CLK/TRG pulse counting
 * (nothing in this emulator generates such pulses - the P2500's own
 * source, presumably a UART-adjacent baud clock, isn't identified yet);
 * real prescaler-accurate timing (there's no cycle-accurate clock in
 * this emulator, just one z80_step() per host call) - p2500_ctc_tick(),
 * called once per instruction from p2500_step(), decrements active
 * TIMER-mode channels by one time-constant-scaled unit per call, close
 * enough to let CTC-driven delay loops and interrupts terminate rather
 * than exact real-world baud timing.
 */

#define P2500_CTC_CHANNELS 4

typedef enum {
    P2500_CTC_WAIT_CONTROL,
    P2500_CTC_WAIT_TIME_CONSTANT,
} P2500CtcState;

typedef void (*P2500CtcInterruptCallback)(void *userdata, uint8_t vector);

typedef struct {
    P2500CtcState state[P2500_CTC_CHANNELS];

    bool int_enabled[P2500_CTC_CHANNELS];
    bool counter_mode[P2500_CTC_CHANNELS];   /* false = TIMER, true = COUNTER */
    bool prescaler_256[P2500_CTC_CHANNELS];  /* false = /16, true = /256 */
    bool rising_edge[P2500_CTC_CHANNELS];
    bool auto_trigger[P2500_CTC_CHANNELS];   /* true = trigger as soon as time constant loads */
    bool started[P2500_CTC_CHANNELS];        /* has a time constant been loaded at least once */

    uint8_t time_constant[P2500_CTC_CHANNELS];
    uint16_t counter[P2500_CTC_CHANNELS];    /* current down-counter, prescaler-scaled */

    uint8_t vector_base; /* shared per-chip: top 5 bits from the last vector write */

    P2500CtcInterruptCallback on_interrupt;
    void *interrupt_userdata;
    bool verbose;
} P2500Ctc;

void p2500_ctc_init(P2500Ctc *ctc);
void p2500_ctc_write(P2500Ctc *ctc, int channel, uint8_t value); /* ports $00-$03 out */
uint8_t p2500_ctc_read(P2500Ctc *ctc, int channel);              /* ports $00-$03 in */

/* Advances all active TIMER-mode channels by one step's worth of ticks;
 * call once per p2500_step(). Fires the interrupt callback (edge, once
 * per zero-crossing) on any channel whose down-counter reaches zero with
 * interrupts enabled, then auto-reloads it from its time constant. */
void p2500_ctc_tick(P2500Ctc *ctc);

#endif
