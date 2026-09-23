#include "keyboard.h"
#include <stdio.h>
#include <string.h>

void p2500_keyboard_init(P2500Keyboard *kb, const uint8_t *queue, size_t queue_len) {
    memset(kb, 0, sizeof(*kb));
    kb->queue = queue;
    kb->queue_len = queue_len;
}

uint8_t p2500_keyboard_in(P2500Keyboard *kb) {
    if (kb->queue && kb->pos < kb->queue_len) {
        uint8_t v = kb->queue[kb->pos++];
        if (kb->verbose)
            fprintf(stderr, "[kbd] delivered byte %zu/%zu = $%02X\n", kb->pos, kb->queue_len, v);
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
