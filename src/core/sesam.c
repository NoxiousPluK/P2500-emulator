#include "sesam.h"
#include <stdio.h>
#include <string.h>

void p2500_sesam_init(P2500Sesam *s, const uint8_t *stream, size_t stream_len) {
    memset(s, 0, sizeof(*s));
    s->stream = stream;
    s->stream_len = stream_len;
}

uint8_t p2500_sesam_in(P2500Sesam *s) {
    s->reads++;
    uint8_t v = (!s->stream || s->pos >= s->stream_len) ? 0xFF : s->stream[s->pos++];
    if (s->verbose)
        fprintf(stderr, "[sesam] IN ($0F) -> $%02X (read %lu, pos %zu/%zu)\n",
                v, s->reads, s->pos, s->stream_len);
    return v;
}

void p2500_sesam_out(P2500Sesam *s, uint8_t value) {
    s->writes++;
    if (value == 3) s->pos = 0; /* real hardware: reset internal address counter */
    /* value==1 (power on) and value==0 (power off) are no-ops in this model */
    if (s->verbose) {
        const char *what = value == 1 ? "power on" : value == 3 ? "reset address counter"
                         : value == 0 ? "power off" : "unknown";
        fprintf(stderr, "[sesam] OUT ($0F) <- $%02X (%s)\n", value, what);
    }
}
