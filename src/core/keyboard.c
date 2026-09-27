#include "keyboard.h"
#include <stdio.h>
#include <string.h>

void p2500_keyboard_init(P2500Keyboard *kb, const uint8_t *queue, size_t queue_len) {
    memset(kb, 0, sizeof(*kb));
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
        if (kb->verbose) fprintf(stderr, "[kbd] ring full, dropped $%02X\n", byte);
        return;
    }
    kb->ring[kb->ring_head] = byte;
    kb->ring_head = next;
}

uint8_t p2500_keyboard_in(P2500Keyboard *kb) {
    if (kb->queue && kb->pos < kb->queue_len) {
        uint8_t v = kb->queue[kb->pos++];
        if (kb->verbose)
            fprintf(stderr, "[kbd] delivered byte %zu/%zu = $%02X\n", kb->pos, kb->queue_len, v);
        return v;
    }
    if (kb->ring_head != kb->ring_tail) {
        uint8_t v = kb->ring[kb->ring_tail];
        kb->ring_tail = (uint8_t)((kb->ring_tail + 1) % sizeof(kb->ring));
        if (kb->verbose) fprintf(stderr, "[kbd] live key $%02X\n", v);
        return v;
    }
    return 0xFF; /* idle - matches the "nothing plugged in" default this port already had */
}

void p2500_serial_init(P2500Serial *s) {
    memset(s, 0, sizeof(*s));
}

void p2500_serial_out(P2500Serial *s, uint8_t value) {
    if (!s->verbose) return;
    if (value >= 0x20 && value < 0x7F)
        fprintf(stderr, "[tx] '%c' ($%02X)\n", (char)value, value);
    else
        fprintf(stderr, "[tx] $%02X\n", value);
}
