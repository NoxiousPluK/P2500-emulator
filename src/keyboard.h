#ifndef P2500_KEYBOARD_H
#define P2500_KEYBOARD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Console serial input (port $06) and output (port $04) - see TODO.md
 * T16/ISSUE tracking. CBIOS's own ISR (traced live, not guessed - see
 * TODO.md) reads port $06 as a single complete byte per interrupt and
 * pushes it straight into a 32-byte circular buffer with no bit-shift/
 * accumulate logic anywhere in that routine; whatever "bit-banging"
 * TODO.md's HWTEST-derived note refers to is therefore either external
 * hardware (a discrete UART/shift-register feeding this port a finished
 * byte) or a different code path this project hasn't traced. Modeled
 * here at the byte level to match what CBIOS's ISR actually does: a
 * queue of bytes to "type", delivered one per port $06 read, with $FF
 * ("this is what an always-unplugged input already looked like, and
 * doesn't get treated as real data" - see p2500_keyboard_in) the rest of
 * the time.
 *
 * Port $04 (TX) writes are captured verbatim - this is this project's
 * only way to see what the machine is trying to print, since there is
 * no live video output yet (TODO.md) and CONOUT's own memory-mapped
 * video path hasn't been exercised in any traced run.
 */

typedef struct {
    const uint8_t *queue;
    size_t queue_len;
    size_t pos;
    bool verbose;
} P2500Keyboard;

void p2500_keyboard_init(P2500Keyboard *kb, const uint8_t *queue, size_t queue_len);
uint8_t p2500_keyboard_in(P2500Keyboard *kb);  /* port $06 */

typedef struct {
    bool verbose; /* echo each transmitted byte to stdout as it's sent */
} P2500Serial;

void p2500_serial_init(P2500Serial *s);
void p2500_serial_out(P2500Serial *s, uint8_t value); /* port $04 */

#endif
