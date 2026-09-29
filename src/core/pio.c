#include "pio.h"
#include <stdio.h>
#include <string.h>

void p2500_pio_init(P2500Pio *pio) {
    /* Re-initialising a device must not silently take its diagnostics
     * away with it: a caller that swaps a keystroke queue in, as the
     * CLI does, would otherwise lose every message the device had to
     * make from then on - a silent drop, which is a failure mode worth
     * guarding against explicitly. */
    const P2500Log *log = pio->log;
    memset(pio, 0, sizeof(*pio));
    pio->log = log;
}

static void evaluate_interrupt(P2500Pio *pio, int port) {
    if (pio->mode[port] != 3 || !pio->int_enabled[port]) {
        pio->condition_was_true[port] = false;
        return;
    }
    uint8_t monitored = (uint8_t)~pio->monitor_mask[port]; /* 1 = monitored */
    uint8_t sensed = pio->input_latch[port];
    uint8_t active = pio->active_high[port] ? sensed : (uint8_t)~sensed;
    uint8_t hits = (uint8_t)(active & monitored);

    bool condition = pio->and_mode[port] ? (monitored != 0 && hits == monitored)
                                          : (hits != 0);

    if (condition && !pio->condition_was_true[port] &&
        pio->vector_set[port] && pio->on_interrupt) {
        if (pio->verbose)
            p2500_logf(pio->log, P2500_LOG_TRACE, "pio", "port %c interrupt condition met, vector=$%02X",
                       port == P2500_PIO_PORT_A ? 'A' : 'B', pio->vector[port]);
        pio->on_interrupt(pio->interrupt_userdata, pio->vector[port]);
    }
    pio->condition_was_true[port] = condition;
}

void p2500_pio_write_control(P2500Pio *pio, int port, uint8_t value) {
    if (port < 0 || port >= P2500_PIO_PORTS) return;
    char label = port == P2500_PIO_PORT_A ? 'A' : 'B';

    switch (pio->state[port]) {
    case P2500_PIO_WAIT_IO_MASK:
        pio->io_mask[port] = value;
        pio->state[port] = P2500_PIO_WAIT_CONTROL;
        if (pio->verbose)
            p2500_logf(pio->log, P2500_LOG_TRACE, "pio", "port %c I/O mask = $%02X", label, value);
        return;
    case P2500_PIO_WAIT_MONITOR_MASK:
        pio->monitor_mask[port] = value;
        pio->state[port] = P2500_PIO_WAIT_CONTROL;
        if (pio->verbose)
            p2500_logf(pio->log, P2500_LOG_TRACE, "pio", "port %c monitor mask = $%02X", label, value);
        evaluate_interrupt(pio, port);
        return;
    case P2500_PIO_WAIT_CONTROL:
        break;
    }

    if ((value & 0x0F) == 0x0F) { /* mode control word */
        pio->mode[port] = (uint8_t)((value >> 6) & 0x03);
        if (pio->verbose)
            p2500_logf(pio->log, P2500_LOG_TRACE, "pio", "port %c mode = %d", label, pio->mode[port]);
        if (pio->mode[port] == 3) pio->state[port] = P2500_PIO_WAIT_IO_MASK;
        return;
    }
    if ((value & 0x0F) == 0x03) { /* interrupt enable/disable short form */
        pio->int_enabled[port] = (value & 0x80) != 0;
        if (pio->verbose)
            p2500_logf(pio->log, P2500_LOG_TRACE, "pio", "port %c interrupt %s", label,
                       pio->int_enabled[port] ? "enabled" : "disabled");
        if (!pio->int_enabled[port] && pio->on_int_reset)
            pio->on_int_reset(pio->interrupt_userdata, port);
        evaluate_interrupt(pio, port);
        return;
    }
    if ((value & 0x0F) == 0x07) { /* interrupt control word */
        pio->int_enabled[port] = (value & 0x80) != 0;
        pio->and_mode[port] = (value & 0x40) != 0;
        pio->active_high[port] = (value & 0x20) != 0;
        bool mask_follows = (value & 0x10) != 0;
        if (pio->verbose)
            p2500_logf(pio->log, P2500_LOG_TRACE, "pio", "port %c interrupt control: enabled=%d and=%d "
                               "active_high=%d mask_follows=%d", label,
                       pio->int_enabled[port], pio->and_mode[port], pio->active_high[port],
                       mask_follows);
        if (!pio->int_enabled[port] && pio->on_int_reset)
            pio->on_int_reset(pio->interrupt_userdata, port);
        if (mask_follows) pio->state[port] = P2500_PIO_WAIT_MONITOR_MASK;
        else evaluate_interrupt(pio, port);
        return;
    }
    if ((value & 0x01) == 0) { /* interrupt vector byte */
        pio->vector[port] = value;
        pio->vector_set[port] = true;
        if (pio->verbose)
            p2500_logf(pio->log, P2500_LOG_TRACE, "pio", "port %c vector = $%02X", label, value);
        return;
    }
    if (pio->verbose)
        p2500_logf(pio->log, P2500_LOG_WARN, "pio", "port %c unrecognized control byte $%02X", label, value);
}

void p2500_pio_write_data(P2500Pio *pio, int port, uint8_t value) {
    if (port < 0 || port >= P2500_PIO_PORTS) return;
    pio->output_latch[port] = value;
}

uint8_t p2500_pio_read_data(P2500Pio *pio, int port) {
    if (port < 0 || port >= P2500_PIO_PORTS) return 0xFF;
    if (pio->mode[port] == 3) {
        uint8_t out_bits = (uint8_t)~pio->io_mask[port];
        return (uint8_t)((pio->output_latch[port] & out_bits) |
                          (pio->input_latch[port] & pio->io_mask[port]));
    }
    return pio->output_latch[port];
}

void p2500_pio_set_input_bit(P2500Pio *pio, int port, int bit, bool level) {
    if (port < 0 || port >= P2500_PIO_PORTS || bit < 0 || bit > 7) return;
    uint8_t mask = (uint8_t)(1 << bit);
    if (level) pio->input_latch[port] |= mask;
    else pio->input_latch[port] &= (uint8_t)~mask;
    evaluate_interrupt(pio, port);
}
