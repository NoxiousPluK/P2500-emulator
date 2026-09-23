#include "sesam.h"

void p2500_sesam_init(P2500Sesam *s, const uint8_t *stream, size_t stream_len) {
    s->stream = stream;
    s->stream_len = stream_len;
    s->pos = 0;
}

uint8_t p2500_sesam_in(P2500Sesam *s) {
    if (!s->stream || s->pos >= s->stream_len) return 0xFF;
    return s->stream[s->pos++];
}

void p2500_sesam_out(P2500Sesam *s, uint8_t value) {
    if (value == 3) s->pos = 0; /* real hardware: reset internal address counter */
    /* value==1 (power on) and value==0 (power off) are no-ops in this model */
}
