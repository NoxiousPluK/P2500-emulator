#include "keyboard.h"
#include <stdio.h>
#include <string.h>

void p2500_keyboard_init(P2500Keyboard *kb, const uint8_t *queue, size_t queue_len) {
    /* Re-initialising a device must not silently take its diagnostics
     * away with it: a caller that swaps a keystroke queue in, as the
     * CLI does, would otherwise lose every message the device had to
     * make from then on - a silent drop, which is a failure mode worth
     * guarding against explicitly. */
    const P2500Log *log = kb->log;
    memset(kb, 0, sizeof(*kb));
    kb->log = log;
    kb->queue = queue;
    kb->queue_len = queue_len;
}

static bool scripted_byte_due(const P2500Keyboard *kb, unsigned long elapsed_tstates) {
    if (!kb->queue || kb->pos >= kb->queue_len) return false;
    if (kb->release_at && elapsed_tstates < kb->release_at[kb->pos]) return false;
    return true;
}

bool p2500_keyboard_byte_waiting(const P2500Keyboard *kb, unsigned long elapsed_tstates) {
    return scripted_byte_due(kb, elapsed_tstates) || kb->ring_head != kb->ring_tail;
}

void p2500_keyboard_push(P2500Keyboard *kb, uint8_t byte) {
    uint8_t next = (uint8_t)((kb->ring_head + 1) % sizeof(kb->ring));
    if (next == kb->ring_tail) {
        if (kb->verbose) p2500_logf(kb->log, P2500_LOG_WARN, "kbd", "ring full, dropped $%02X", byte);
        return;
    }
    kb->ring[kb->ring_head] = byte;
    kb->ring_head = next;
}

uint8_t p2500_keyboard_in(P2500Keyboard *kb) {
    if (kb->queue && kb->pos < kb->queue_len) {
        uint8_t v = kb->queue[kb->pos++];
        if (kb->verbose)
            p2500_logf(kb->log, P2500_LOG_TRACE, "kbd", "delivered byte %zu/%zu = $%02X", kb->pos, kb->queue_len, v);
        return v;
    }
    if (kb->ring_head != kb->ring_tail) {
        uint8_t v = kb->ring[kb->ring_tail];
        kb->ring_tail = (uint8_t)((kb->ring_tail + 1) % sizeof(kb->ring));
        if (kb->verbose) p2500_logf(kb->log, P2500_LOG_TRACE, "kbd", "live key $%02X", v);
        return v;
    }
    return 0xFF; /* idle - matches the "nothing plugged in" default this port already had */
}

void p2500_serial_init(P2500Serial *s) {
    /* Re-initialising a device must not silently take its diagnostics
     * away with it: a caller that swaps a keystroke queue in, as the
     * CLI does, would otherwise lose every message the device had to
     * make from then on - a silent drop, which is a failure mode worth
     * guarding against explicitly. */
    const P2500Log *log = s->log;
    memset(s, 0, sizeof(*s));
    s->log = log;
}

void p2500_serial_out(P2500Serial *s, uint8_t value) {
    if (!s->verbose) return;
    if (value >= 0x20 && value < 0x7F)
        p2500_logf(s->log, P2500_LOG_TRACE, "tx", "'%c' ($%02X)", (char)value, value);
    else
        p2500_logf(s->log, P2500_LOG_TRACE, "tx", "$%02X", value);
}
