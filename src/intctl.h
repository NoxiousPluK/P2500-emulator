#ifndef P2500_INTCTL_H
#define P2500_INTCTL_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Z80 IM2 interrupt controller - the daisy chain (TODO.md T17).
 *
 * Why this exists: the vendored CPU core (src/vendor/superzazu_z80/z80.c)
 * has a *single* pending-interrupt slot (`int_pending` / `int_data`), so
 * every z80_gen_int() call overwrote the previous one. Two devices asking
 * in the same instruction meant one of them was silently discarded - and
 * it was, systematically: CTC channel 1's handler executed 0 times against
 * 19,106 requests, and the IPL's DMA end-of-block handler ($07F3) had
 * never executed in any run of this emulator. Peripherals now hold their
 * request here until the CPU actually acknowledges it.
 *
 * What a real Z80 daisy chain does, and what this models (Z80 Family CPU
 * Peripherals User Manual UM008101, "Interrupt Priority (Daisy Chain) and
 * Nested Interrupts"):
 *
 *  - Every peripheral has IEI (in) and IEO (out) tied in a chain. A device
 *    may pull /INT low only while its IEI is high, i.e. only while no
 *    device *ahead of it in the chain* is requesting or being serviced.
 *    Position in the chain is the entire priority scheme; it is fixed by
 *    wiring, not programmable.
 *  - The device that wins the acknowledge holds IEO low - blocking
 *    everything downstream - until its handler executes RETI. Real
 *    peripherals watch the bus for the ED 4D opcode to see this, which is
 *    why RETI, uniquely, has side effects outside the CPU.
 *  - A device is also released when it is reset or has its interrupts
 *    disabled by a command written to it. This matters here: the IPL's own
 *    handlers exit via "LD SP,($FF26)" + JP and never execute RETI (see
 *    TODO.md's RAM address table), so on real hardware the FDD card's PIO
 *    and DMA must be released some other way - and they are, by the
 *    interrupt-disable control words the floppy driver writes around every
 *    critical section ($73/$F3 to the PIO, WR6 $A3 "reset and disable
 *    interrupts" to the DMA). CBIOS's own handlers do use RETI ($EBE2 is
 *    "EI / RETI"), so both eras are covered without a fallback heuristic.
 *
 * THE CHAIN ORDER, and how much of it the firmware actually pins down.
 * Within a chip the order is fixed by the chip: CTC channel 0 > 1 > 2 > 3,
 * PIO port A > port B. Between chips it is wiring - but it is not a free
 * choice, because the IPL's own handlers only work in one of the two
 * orders:
 *
 *   The DMA is upstream of (higher priority than) the PIO. Both the DMA's
 *   end-of-block handler ($07F3) and the PIO's ($080B) begin with
 *   "LD ($FF26),SP / LD SP,$FF26" - they save the caller's SP to the *same*
 *   fixed word and then run on the same tiny private stack just below it.
 *   Two consequences: they cannot nest (the inner one's LD ($FF26),SP
 *   destroys the outer one's saved SP), and being entered while SP is
 *   already $FF26 is fatal on its own - the handler's first PUSH lands on
 *   $FF24, which is where the interrupt's own return address was just
 *   pushed. That is exactly what happened with the PIO ahead of the DMA:
 *   the PIO handler calls "EI / RETI" at $0853 to release itself early and
 *   then keeps running on the private stack, the still-pending DMA
 *   interrupt is accepted at $081C, and $07F3's PUSH AF overwrites its own
 *   return address - it returns to $0042 and the machine dies in an RST 38
 *   loop. With the DMA ahead of the PIO both interrupts are serviced one
 *   after the other from the normal stack and neither ever nests.
 *   Reproduce: --break 0038, then compare the two orders.
 *
 * Where the CTC sits relative to the FDD card is still genuinely open: the
 * IPL never programs the CTC at all, so nothing from that era constrains
 * it, and it is only observable when a CTC channel and a disk interrupt
 * collide. It stays on ROADMAP.md's hardware measurement list. The order
 * below puts it last, which is the conservative reading - a CTC tick then
 * cannot pre-empt a disk handler that has released itself early with the
 * same EI/RETI trick.
 */

/* Identity only - priority is p2500_intctl_chain_order[] in intctl.c. */
typedef enum {
    P2500_INT_CTC0 = 0,
    P2500_INT_CTC1,
    P2500_INT_CTC2,
    P2500_INT_CTC3,
    P2500_INT_PIO_A,
    P2500_INT_PIO_B,
    P2500_INT_DMA,
    P2500_INT_SOURCES
} P2500IntSource;

/* The daisy chain, highest priority first. */
extern const int p2500_intctl_chain_order[P2500_INT_SOURCES];

typedef struct {
    bool requested[P2500_INT_SOURCES];
    bool under_service[P2500_INT_SOURCES];
    uint8_t vector[P2500_INT_SOURCES];
    unsigned long requests[P2500_INT_SOURCES];      /* statistics, for the harness */
    unsigned long acknowledged[P2500_INT_SOURCES];
    bool verbose;
} P2500IntCtl;

void p2500_intctl_init(P2500IntCtl *ic);
const char *p2500_intctl_name(int source);

/* A peripheral asserts /INT with the vector it will put on the bus. Held
 * until acknowledged, so nothing can overwrite it. Re-asserting while
 * already requesting is a no-op (a level, not an edge). */
void p2500_intctl_request(P2500IntCtl *ic, int source, uint8_t vector);

/* The peripheral withdraws its request (e.g. its condition went away
 * before the CPU got round to it), without having been serviced. */
void p2500_intctl_withdraw(P2500IntCtl *ic, int source);

/* The peripheral was reset, or had its interrupts disabled by a command
 * written to it: it drops both a pending request and any under-service
 * state, releasing IEO for everything downstream. */
void p2500_intctl_reset_source(P2500IntCtl *ic, int source);

/* Highest-priority source that may currently drive /INT: it is requesting
 * and nothing ahead of it in the chain is requesting or under service.
 * Returns -1 if the chain has nothing to offer. */
int p2500_intctl_pending(const P2500IntCtl *ic);

/* The CPU took `source`'s vector: it stops requesting and holds IEO low. */
void p2500_intctl_acknowledge(P2500IntCtl *ic, int source);

/* A RETI was executed. On real hardware the highest-priority device that
 * is currently under service is the one that sees it (everything ahead of
 * it is idle, so its IEI is high) and releases. */
void p2500_intctl_reti(P2500IntCtl *ic);

#endif
