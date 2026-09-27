#ifndef P2500_SESAM_H
#define P2500_SESAM_H

#include <stdint.h>
#include <stddef.h>

/*
 * SESAM port ($0F) model - the copy-protection dongle / bootable-cartridge
 * slot. Protocol confirmed both from community reverse-engineering of the
 * real Philips SESAM dongle (Information from the internet/
 * 20260920-Research/findings.md in the parent research project) and from
 * this project's own ROM-level decode (ROM Dumps/CPU-Card-Boot-EPROM/
 * emulation/findings.md): OUT 15,1 (power on) / delay / OUT 15,3 (reset
 * internal read-address counter) / repeated IN 15 (auto-incrementing,
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
} P2500Sesam;

void p2500_sesam_init(P2500Sesam *s, const uint8_t *stream, size_t stream_len);
uint8_t p2500_sesam_in(P2500Sesam *s);
void p2500_sesam_out(P2500Sesam *s, uint8_t value);

#endif
