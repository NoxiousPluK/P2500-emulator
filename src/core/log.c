#include "log.h"

#include <stdio.h>

void p2500_logf(const P2500Log *log, P2500LogLevel level, const char *category,
                const char *fmt, ...)
{
    if (!log || !log->fn) return;
    /* One line, one stack buffer. The longest message in the core is the
     * DMA's interrupt-control decode at about 120 characters; 512 leaves
     * room without putting a formatter on the heap in the middle of an
     * emulated instruction. */
    char message[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    log->fn(log->userdata, level, category, message);
}
