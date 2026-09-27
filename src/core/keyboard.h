#ifndef P2500_KEYBOARD_H
#define P2500_KEYBOARD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Keyboard input (port $06) and serial transmit (port $04).
 *
 * Port $06 is the keyboard, and it really is byte-wide: CBIOS's CTC
 * channel-3 ISR ($ED2D, read out of a live RAM dump) does a single
 * IN A,($06) per interrupt and pushes the whole byte into a 32-byte
 * circular buffer at $ED8F, with no bit-shift or accumulate anywhere. So
 * an external controller hands this port a finished byte and strobes CTC
 * channel 3's CLK/TRG to announce it - see machine.c's
 * advance_keyboard_strobe.
 *
 * The bit-banging that TODO.md's HWTEST-derived note refers to is a
 * *different* path, now identified: the serial port. CBIOS bit-bangs that
 * one against CTC channels 0 and 1, transmitting on port $04 and sampling
 * port $05 bit 7 on receive (see ctc.h). It is not this port.
 *
 * Modeled here as a queue of bytes to "type", delivered one per port $06
 * read, with $FF ("nothing plugged in") the rest of the time.
 *
 * `start_after_tstates` exists because a real person cannot type before
 * the machine is ready, and this emulator has no way to know when that is.
 * Keystrokes delivered too early are genuinely lost: CBIOS's console
 * driver zeroes its ring-buffer header as the last step of its own
 * initialisation (the loop at $ED02), so anything already queued is
 * discarded. Holding the first keystroke back reproduces "the user waited
 * for the prompt" without the model needing to know what a prompt is.
 *
 * Port $04 writes are captured verbatim - a view of the serial line, which
 * for a machine with video output is not where CONOUT goes (see
 * --dump-vram).
 */

typedef struct {
    const uint8_t *queue;
    size_t queue_len;
    size_t pos;
    /* Per-byte earliest delivery time, in T-states from reset, parallel to
     * `queue`. Per-byte rather than one queue-wide start so a session can be
     * scripted against events the guest drives - type a command, wait for it
     * to finish, swap the disk, then type again (see --type-at/--swap-at).
     * NULL means "no constraint". */
    const unsigned long *release_at;
    bool verbose;
} P2500Keyboard;

void p2500_keyboard_init(P2500Keyboard *kb, const uint8_t *queue, size_t queue_len);
/* True once `elapsed_tstates` has passed start_after_tstates and there is
 * still an undelivered byte. */
bool p2500_keyboard_byte_waiting(const P2500Keyboard *kb, unsigned long elapsed_tstates);
uint8_t p2500_keyboard_in(P2500Keyboard *kb);  /* port $06 */

typedef struct {
    bool verbose; /* echo each transmitted byte to stdout as it's sent */
} P2500Serial;

void p2500_serial_init(P2500Serial *s);
void p2500_serial_out(P2500Serial *s, uint8_t value); /* port $04 */

#endif
