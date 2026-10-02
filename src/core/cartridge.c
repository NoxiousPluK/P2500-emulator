#include "cartridge.h"
#include <stdio.h>
#include <string.h>

void p2500_cartridge_init(P2500Cartridge *s, const uint8_t *stream, size_t stream_len) {
    /* Re-initialising a device must not silently take its diagnostics
     * away with it: a caller that swaps a keystroke queue in, as the
     * CLI does, would otherwise lose every message the device had to
     * make from then on - a silent drop, which is a failure mode worth
     * guarding against explicitly. */
    const P2500Log *log = s->log;
    memset(s, 0, sizeof(*s));
    s->log = log;
    s->stream = stream;
    s->stream_len = stream_len;
}

uint8_t p2500_cartridge_in(P2500Cartridge *s) {
    s->reads++;
    uint8_t v = (!s->stream || s->pos >= s->stream_len) ? 0xFF : s->stream[s->pos++];
    if (s->verbose)
        p2500_logf(s->log, P2500_LOG_TRACE, "cartridge", "IN ($0F) -> $%02X (read %lu, pos %zu/%zu)",
                   v, s->reads, s->pos, s->stream_len);
    return v;
}

void p2500_cartridge_out(P2500Cartridge *s, uint8_t value) {
    s->writes++;
    if (value == 3) s->pos = 0; /* real hardware: reset internal address counter */
    /* value==1 (power on) and value==0 (power off) are no-ops in this model */
    if (s->verbose) {
        const char *what = value == 1 ? "power on" : value == 3 ? "reset address counter"
                         : value == 0 ? "power off" : "unknown";
        p2500_logf(s->log, P2500_LOG_TRACE, "cartridge", "OUT ($0F) <- $%02X (%s)", value, what);
    }
}
