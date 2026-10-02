#ifndef P2500_CARTRIDGE_H
#define P2500_CARTRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "log.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cartridge slot ($0F) model - a generic plug-in socket. Its best-documented
 * use is the SESAM copy-protection dongle, but the same slot also loads
 * small programs (the P2500's own "Maint" diagnostic plug is a real,
 * first-party example). Protocol: OUT 15,1 (power on) / delay / OUT 15,3
 * (reset internal read-address counter) / repeated IN 15 (auto-incrementing,
 * no re-addressing) / OUT 15,0 (power off).
 *
 * With no stream loaded, every read returns $FF - "nothing plugged in",
 * matching the ROM's own presence check (byte 0 must be $00 for the
 * bootable-cartridge branch; anything else, $FF included, is read as
 * "not a bootable cartridge", and the ROM continues normal floppy boot).
 */

typedef struct {
    const uint8_t *stream;
    size_t stream_len;
    size_t pos;
    unsigned long reads;  /* statistics - a protection check shows up here */
    unsigned long writes;
    /* Diagnostics sink, pointed at the machine's own by p2500_init().
     * NULL is fine - the messages are simply discarded. */
    const P2500Log *log;
    bool verbose;
} P2500Cartridge;

void p2500_cartridge_init(P2500Cartridge *s, const uint8_t *stream, size_t stream_len);
uint8_t p2500_cartridge_in(P2500Cartridge *s);
void p2500_cartridge_out(P2500Cartridge *s, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif
