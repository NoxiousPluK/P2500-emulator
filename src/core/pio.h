#ifndef P2500_PIO_H
#define P2500_PIO_H

#include <stdint.h>

#include "log.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Z80A-PIO (Z8420A) model for the FDD card's ports $10 (Port A data) /
 * $11 (Port B data) / $12 (Port A control) / $13 (Port B control).
 *
 * Only mode 3 (bit control) is exercised by any traced ROM code, so that's
 * the only mode this model makes functional; modes 0-2 just record the
 * mode number.
 *
 * Follow-byte state machine, from the ROM's own init sequence at
 * $045A-$0473 ($FF,$03,vector,$37,$FC on port A / $FF,$A1 on port B):
 *   - after a mode-3 word (low nibble $F, D7:D6 = mode): next byte is the
 *     I/O-select mask (1 = input bit, 0 = output bit)
 *   - after an interrupt control word (low nibble $7) with D4 set: next
 *     byte is the interrupt monitor mask (0 = monitored, 1 = masked off -
 *     confirmed by $FC monitoring only PA0/PA1)
 *   - a byte with bit0 clear, seen outside those two follow states, is the
 *     interrupt vector
 *   - a byte with low nibble $3 outside those states is the interrupt
 *     enable/disable short form the floppy driver uses constantly around
 *     critical sections (D7=1 enable / D7=0 disable, e.g. $F3/$73)
 */

#define P2500_PIO_PORTS 2
#define P2500_PIO_PORT_A 0
#define P2500_PIO_PORT_B 1

typedef enum {
    P2500_PIO_WAIT_CONTROL,
    P2500_PIO_WAIT_IO_MASK,
    P2500_PIO_WAIT_MONITOR_MASK
} P2500PioState;

typedef void (*P2500PioInterruptCallback)(void *userdata, uint8_t vector);

typedef struct {
    P2500PioState state[P2500_PIO_PORTS];
    uint8_t mode[P2500_PIO_PORTS];        /* 0-3; only 3 (bit control) is functional */
    uint8_t io_mask[P2500_PIO_PORTS];     /* mode 3 only: 1 = input bit, 0 = output bit */
    uint8_t output_latch[P2500_PIO_PORTS];
    uint8_t input_latch[P2500_PIO_PORTS]; /* driven by machine.c, e.g. FDC INT -> PA0 */

    uint8_t vector[P2500_PIO_PORTS];
    bool vector_set[P2500_PIO_PORTS];

    bool int_enabled[P2500_PIO_PORTS];
    bool and_mode[P2500_PIO_PORTS];        /* false = OR, true = AND */
    bool active_high[P2500_PIO_PORTS];
    uint8_t monitor_mask[P2500_PIO_PORTS]; /* 0 = monitored bit, 1 = masked off */
    bool condition_was_true[P2500_PIO_PORTS]; /* edge detection so a held level fires once */

    /* Diagnostics sink, pointed at the machine's own by p2500_init().
     * NULL is fine - the messages are simply discarded. */
    const P2500Log *log;
    P2500PioInterruptCallback on_interrupt;
    /* Called with the port number when that port's interrupts are
     * disabled by a control word. On a Z80-PIO that clears any pending or
     * in-service interrupt for the port, releasing its place in the IM2
     * daisy chain (see intctl.h) - which is how the IPL's handlers, which
     * never execute RETI, stay unstuck: the floppy driver writes $73/$F3
     * around every critical section. */
    void (*on_int_reset)(void *userdata, int port);
    void *interrupt_userdata;
    bool verbose;
} P2500Pio;

void p2500_pio_init(P2500Pio *pio);
void p2500_pio_write_control(P2500Pio *pio, int port, uint8_t value); /* $12/$13 */
void p2500_pio_write_data(P2500Pio *pio, int port, uint8_t value);    /* $10/$11 out */
uint8_t p2500_pio_read_data(P2500Pio *pio, int port);                 /* $10/$11 in */

/* Drive one input bit (e.g. the FDC's INT line into PA0/PA1).
 * Evaluates the interrupt condition and fires on a low->true
 * transition if the port is in mode 3 with interrupts enabled and a
 * vector has been programmed. */
void p2500_pio_set_input_bit(P2500Pio *pio, int port, int bit, bool level);

#ifdef __cplusplus
}
#endif

#endif
