#include "ctc.h"
#include <stdio.h>
#include <string.h>

void p2500_ctc_init(P2500Ctc *ctc) {
    memset(ctc, 0, sizeof(*ctc));
    for (int ch = 0; ch < P2500_CTC_CHANNELS; ch++)
        ctc->clk_trg[ch] = true; /* CLK/TRG inputs idle high until driven */
}

/* A time constant of 0 means 256 - the down-counter is loaded with the
 * full 8-bit range (UM008101, "Time Constant Register"). */
static uint16_t reload_value(const P2500Ctc *ctc, int ch) {
    return ctc->time_constant[ch] == 0 ? 256 : ctc->time_constant[ch];
}

static void reload(P2500Ctc *ctc, int ch) {
    ctc->counter[ch] = reload_value(ctc, ch);
}

void p2500_ctc_write(P2500Ctc *ctc, int channel, uint8_t value) {
    if (channel < 0 || channel >= P2500_CTC_CHANNELS) return;

    if (ctc->state[channel] == P2500_CTC_WAIT_TIME_CONSTANT) {
        ctc->time_constant[channel] = value;
        ctc->state[channel] = P2500_CTC_WAIT_CONTROL;
        reload(ctc, channel);
        ctc->prescale_acc[channel] = 0;
        ctc->started[channel] = true;
        if (ctc->verbose)
            fprintf(stderr, "[ctc] channel %d time constant = $%02X (%u)\n",
                    channel, value, reload_value(ctc, channel));
        return;
    }

    if ((value & 0x01) == 0) {
        /* Bit 0 clear: interrupt vector. A real Z80-CTC loads its single
         * shared vector register from channel 0 only (UM008101, "Interrupt
         * Vector Register"); the channel identifier in bits 2:1 is
         * supplied by the chip, not by software. CBIOS does write it to
         * port $00 (three times, once per channel it arms), so this has
         * never mattered - but a write to any other channel would be a
         * misdecode worth seeing rather than silently accepting. */
        if (channel != 0) {
            if (ctc->verbose)
                fprintf(stderr, "[ctc] ignoring vector byte $%02X written to channel %d "
                                "(a real CTC only accepts it on channel 0)\n", value, channel);
            return;
        }
        ctc->vector_base = (uint8_t)(value & 0xF8);
        ctc->vector_set = true;
        if (ctc->verbose)
            fprintf(stderr, "[ctc] interrupt vector base = $%02X\n", ctc->vector_base);
        return;
    }

    /* Control word - UM008101 Table 2. */
    bool was_enabled = ctc->int_enabled[channel];
    ctc->int_enabled[channel] = (value & 0x80) != 0;
    ctc->counter_mode[channel] = (value & 0x40) != 0;
    ctc->prescaler_256[channel] = (value & 0x20) != 0;
    ctc->rising_edge[channel] = (value & 0x10) != 0;
    ctc->auto_trigger[channel] = (value & 0x08) == 0;
    bool time_constant_follows = (value & 0x04) != 0;
    bool software_reset = (value & 0x02) != 0;

    if (ctc->verbose)
        fprintf(stderr, "[ctc] channel %d control $%02X: int=%d mode=%s presc=%d edge=%s "
                        "tc_follows=%d reset=%d\n",
                channel, value, ctc->int_enabled[channel],
                ctc->counter_mode[channel] ? "COUNTER" : "TIMER",
                ctc->prescaler_256[channel] ? 256 : 16,
                ctc->rising_edge[channel] ? "rising" : "falling",
                time_constant_follows, software_reset);

    if (software_reset) {
        ctc->started[channel] = false;
        ctc->counter[channel] = 0;
        ctc->prescale_acc[channel] = 0;
    }
    /* A software reset, or disabling the channel's interrupt, clears any
     * pending or in-service interrupt in the chip and so releases the
     * channel's place in the daisy chain (UM008101, "Channel Control
     * Register", bit 1). */
    if ((software_reset || (was_enabled && !ctc->int_enabled[channel])) && ctc->on_reset)
        ctc->on_reset(ctc->interrupt_userdata, channel);

    if (time_constant_follows)
        ctc->state[channel] = P2500_CTC_WAIT_TIME_CONSTANT;
}

uint8_t p2500_ctc_read(P2500Ctc *ctc, int channel) {
    if (channel < 0 || channel >= P2500_CTC_CHANNELS) return 0xFF;
    /* Reading a channel returns the live down-counter. It is a true
     * 8-bit value now that the prescaler is a real clock divider rather
     * than folded into the count (TODO.md T18), so no rescaling. */
    return (uint8_t)(ctc->counter[channel] & 0xFF);
}

/* One decrement of a channel's down-counter. On a zero crossing the
 * channel reloads, raises its interrupt if enabled, and pulses ZC/TO. */
static void count_down(P2500Ctc *ctc, int ch) {
    if (!ctc->started[ch] || ctc->counter[ch] == 0) return;
    ctc->counter[ch]--;
    if (ctc->counter[ch] != 0) return;

    reload(ctc, ch);
    if (ctc->int_enabled[ch] && ctc->on_interrupt) {
        uint8_t vector = (uint8_t)(ctc->vector_base | (ch << 1));
        if (ctc->verbose)
            fprintf(stderr, "[ctc] channel %d ZC/TO, vector=$%02X\n", ch, vector);
        ctc->on_interrupt(ctc->interrupt_userdata, ch, vector);
    }
}

void p2500_ctc_tick(P2500Ctc *ctc, uint32_t tstates) {
    if (tstates == 0) return;
    for (int ch = 0; ch < P2500_CTC_CHANNELS; ch++) {
        if (ctc->counter_mode[ch] || !ctc->started[ch]) continue;
        uint32_t divisor = ctc->prescaler_256[ch] ? 256u : 16u;
        ctc->prescale_acc[ch] += tstates;
        while (ctc->prescale_acc[ch] >= divisor) {
            ctc->prescale_acc[ch] -= divisor;
            count_down(ctc, ch);
        }
    }
}

void p2500_ctc_set_clk_trg(P2500Ctc *ctc, int channel, bool level) {
    if (channel < 0 || channel >= P2500_CTC_CHANNELS) return;
    bool previous = ctc->clk_trg[channel];
    ctc->clk_trg[channel] = level;
    if (previous == level) return;
    bool edge = ctc->rising_edge[channel] ? (!previous && level) : (previous && !level);
    if (!edge) return;
    /* In TIMER mode CLK/TRG is a trigger, not a clock: with bit 3 of the
     * control word set the timer starts on this edge rather than on the
     * time-constant load. It never decrements the counter. */
    if (ctc->counter_mode[channel]) count_down(ctc, channel);
}
