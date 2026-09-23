#include "ctc.h"
#include <stdio.h>
#include <string.h>

void p2500_ctc_init(P2500Ctc *ctc) {
    memset(ctc, 0, sizeof(*ctc));
}

static uint16_t scaled_time_constant(const P2500Ctc *ctc, int ch) {
    /* Real hardware: down-counter start value = time_constant, decremented
     * once per prescaler(16 or 256) input clocks. This emulator has no
     * per-clock-cycle timing (one p2500_ctc_tick() call per instruction,
     * not per clock), so the prescaler is folded in as a tick multiplier
     * instead of a true clock divider - close enough to let CTC-driven
     * delays and interrupts resolve in a bounded number of instructions
     * without claiming real baud-rate accuracy (see ctc.h). */
    uint16_t tc = ctc->time_constant[ch] == 0 ? 256 : ctc->time_constant[ch];
    return (uint16_t)(tc * (ctc->prescaler_256[ch] ? 4 : 1));
}

static void reload(P2500Ctc *ctc, int ch) {
    ctc->counter[ch] = scaled_time_constant(ctc, ch);
}

void p2500_ctc_write(P2500Ctc *ctc, int channel, uint8_t value) {
    if (channel < 0 || channel >= P2500_CTC_CHANNELS) return;

    if (ctc->state[channel] == P2500_CTC_WAIT_TIME_CONSTANT) {
        ctc->time_constant[channel] = value;
        ctc->state[channel] = P2500_CTC_WAIT_CONTROL;
        reload(ctc, channel);
        ctc->started[channel] = true;
        if (ctc->verbose)
            fprintf(stderr, "[ctc] channel %d time constant = $%02X\n", channel, value);
        return;
    }

    if ((value & 0x01) == 0) { /* bit0=0: vector byte, not a control word */
        ctc->vector_base = (uint8_t)(value & 0xF8);
        if (ctc->verbose)
            fprintf(stderr, "[ctc] interrupt vector base = $%02X\n", ctc->vector_base);
        return;
    }

    /* Control word - see ctc.h / Z80 Family CPU Peripherals User Manual
     * Table 2 for the bit layout this mirrors exactly. */
    ctc->int_enabled[channel] = (value & 0x80) != 0;
    ctc->counter_mode[channel] = (value & 0x40) != 0;
    ctc->prescaler_256[channel] = (value & 0x20) != 0;
    ctc->rising_edge[channel] = (value & 0x10) != 0;
    ctc->auto_trigger[channel] = (value & 0x08) == 0;
    bool time_constant_follows = (value & 0x04) != 0;
    bool software_reset = (value & 0x02) != 0;

    if (ctc->verbose)
        fprintf(stderr, "[ctc] channel %d control $%02X: int=%d mode=%s presc=%d "
                        "tc_follows=%d reset=%d\n",
                channel, value, ctc->int_enabled[channel],
                ctc->counter_mode[channel] ? "COUNTER" : "TIMER",
                ctc->prescaler_256[channel] ? 256 : 16, time_constant_follows, software_reset);

    if (software_reset) {
        ctc->started[channel] = false;
        ctc->counter[channel] = 0;
    }
    if (time_constant_follows) {
        ctc->state[channel] = P2500_CTC_WAIT_TIME_CONSTANT;
    }
}

uint8_t p2500_ctc_read(P2500Ctc *ctc, int channel) {
    if (channel < 0 || channel >= P2500_CTC_CHANNELS) return 0xFF;
    /* Real hardware exposes the live down-counter value, scaled back down
     * to a single byte via the prescaler - approximate here given the
     * tick model above, but monotonically counts down to 0 like the real
     * register does, which is what polling code actually checks for. */
    uint16_t raw = ctc->counter[channel];
    return (uint8_t)(ctc->prescaler_256[channel] ? raw / 4 : raw);
}

/* Decrements one channel by one pulse. Returns true if it just crossed
 * zero (a ZC/TO pulse), having already reloaded and fired its interrupt
 * if enabled. */
static bool pulse_channel(P2500Ctc *ctc, int ch) {
    if (!ctc->started[ch] || ctc->counter[ch] == 0) return false;
    ctc->counter[ch]--;
    if (ctc->counter[ch] != 0) return false;

    reload(ctc, ch);
    if (ctc->int_enabled[ch] && ctc->on_interrupt) {
        uint8_t vector = (uint8_t)(ctc->vector_base | (ch << 1));
        if (ctc->verbose)
            fprintf(stderr, "[ctc] channel %d ZC/TO, vector=$%02X\n", ch, vector);
        ctc->on_interrupt(ctc->interrupt_userdata, vector);
    }
    return true;
}

void p2500_ctc_tick(P2500Ctc *ctc, bool channel3_rx_ready) {
    /* Channel 0's ZC/TO output pin is wired into channels 1 and 2's
     * CLK/TRG input on real hardware - the textbook Z80-CTC "one channel
     * as baud-rate generator, the rest as per-line dividers" pattern (see
     * ctc.h). Not confirmed by schematic/continuity on this board, but
     * strongly indicated by the actual control words CBIOS programs:
     * channel 0 is always TIMER mode with interrupts *disabled* (it only
     * needs to produce pulses, not interrupt the CPU itself), while
     * channels 1-3 are COUNTER mode with interrupts enabled and a time
     * constant of 1 (fire on every incoming pulse).
     *
     * Channel 3 is deliberately NOT chained the same way. Live tracing
     * (TODO.md) showed its handler is specifically the console RX byte
     * push into CBIOS's circular input buffer, with no filtering of the
     * value read from port $06 - chaining it unconditionally like 1/2
     * floods that buffer with the idle "no key" byte on every single
     * pulse (confirmed live: the buffer fills solid within the first few
     * thousand instructions and every real keystroke queued afterward is
     * silently dropped, since the buffer never has room). Real hardware
     * presumably only pulses channel 3's CLK/TRG when a keyboard
     * controller actually has a byte ready - `channel3_rx_ready` (from
     * machine.c, true exactly when a queued keystroke is waiting to be
     * delivered) stands in for that same real condition. */
    bool ch0_pulse = false;
    if (!ctc->counter_mode[0]) ch0_pulse = pulse_channel(ctc, 0);

    for (int ch = 1; ch <= 2; ch++) {
        if (ctc->counter_mode[ch]) {
            if (ch0_pulse) pulse_channel(ctc, ch);
        } else {
            pulse_channel(ctc, ch);
        }
    }

    if (ctc->counter_mode[3]) {
        if (ch0_pulse && channel3_rx_ready) pulse_channel(ctc, 3);
    } else {
        pulse_channel(ctc, 3);
    }
}
