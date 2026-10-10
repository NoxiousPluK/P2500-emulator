#ifndef P2500_HISTORY_H
#define P2500_HISTORY_H

#include "machine.h"
#include "debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Execution history: rewind, and seek to an arbitrary past instruction.
 *
 * Snapshot plus replay, which works here for two reasons that are
 * properties of the core rather than of this file:
 *
 *   - P2500Machine holds ALL mutable state and nothing in src/core/ calls
 *     rand(), time() or any other host service. The 50 Hz clock strobe and
 *     the keyboard strobe are phase counters driven off the CPU's T-state
 *     count, so a replay from a given state is bit-identical. (The CLI's
 *     --verify-rewind asserts exactly that rather than assuming it.)
 *   - m->total_instructions is monotonic, so every inter-instruction moment
 *     has a unique name to seek to.
 *
 * A snapshot is therefore a plain memcpy - but the struct is
 * NON-RELOCATABLE (fdc.ram points at m.ram, every device's log at m.log,
 * the CPU core's callbacks carry userdata = &m), so a restore must go back
 * into the SAME P2500Machine object. That is what a rewind does, and
 * p2500_history_seek() refuses a different object rather than producing a
 * machine full of pointers into a copy. Writing state to a file would need
 * the pointers rebased and is a different task.
 *
 * What the front end attached - mounted disks, the cartridge stream, the
 * scripted keystrokes, the log sink, the verbose switches - is re-imposed
 * after the restore instead of being rewound, the same way p2500_reset()
 * keeps them (see P2500Attachments). So rewinding across a disk swap leaves
 * the new disk mounted rather than resurrecting a pointer the front end has
 * already freed.
 *
 * Not covered, deliberately: disk CONTENT. P2500Fdc::disk[] is a const
 * pointer into a front-end buffer and media is immutable today, so there is
 * nothing to roll back. The moment writes land, a rewind across a write
 * needs the writes journalled alongside these snapshots - a 256 KB image
 * per slot is not an option.
 */

/* 64 slots x one per 50 Hz field is ~7 MB and 1.3 s of history. Both are
 * constants rather than a policy spread across two front-ends: the cadence
 * belongs here so the GUI and the harness rewind identically. */
#define P2500_HISTORY_SLOTS 64
#define P2500_HISTORY_INTERVAL_TSTATES P2500_TSTATES_PER_FRAME

typedef struct P2500History P2500History;

/* 7 MB does not belong in a struct a front-end holds by value, so this is
 * heap-allocated and opaque. NULL on allocation failure. */
P2500History *p2500_history_create(void);
void p2500_history_destroy(P2500History *h);

/* Throw the history away. Required after anything that makes the past
 * un-replayable rather than merely old: p2500_reset(), and mounting or
 * ejecting media (the snapshots describe a machine that was reading a
 * different disk). Safe with h == NULL. */
void p2500_history_clear(P2500History *h);

/* Record where we are, if a snapshot is due. Call it once per run-loop
 * iteration from either front-end; the cadence check is internal, so a
 * caller never decides how dense the history is. The first call after a
 * create or clear always captures. Safe with h == NULL. */
void p2500_history_mark(P2500History *h, const P2500Machine *m);

/* How many snapshots are held, and the range of instruction counts that can
 * be sought to: [oldest snapshot, the machine's current position]. span()
 * returns false while the history is empty. */
int p2500_history_depth(const P2500History *h);
bool p2500_history_span(const P2500History *h, const P2500Machine *m,
                        unsigned long *oldest, unsigned long *newest);

/* Seek to the moment just before instruction number `target` was executed:
 * restore the newest snapshot at or before it and replay forward.
 *
 * Replay is silent - the log sink is detached for the duration and
 * p2500_debug_before_step() is not called - so breakpoints do not fire and
 * counters do not double-count on the way back.
 *
 * Returns false and changes nothing when the target is older than the
 * oldest snapshot, or when `m` is not the object the snapshots were taken
 * from. A target at or after the current position is legal and is a plain
 * forward run (deterministic, and the same instructions). */
bool p2500_history_seek(P2500History *h, P2500Machine *m, unsigned long target);

/* Step back `n` instructions. Convenience over seek(). */
bool p2500_history_step_back(P2500History *h, P2500Machine *m, unsigned long n);

/*
 * There is no reverse step-OVER, and that is a finding rather than a gap.
 *
 * Both of the standard ways to define it need the caller's stack depth to
 * be recoverable at an arbitrary moment inside the callee, and in THIS
 * firmware it is not. The IPL's driver dispatch switches stacks on entry -
 * $02DA is `ld ($FE81),sp` / `ld sp,$FE81` - so inside a `call $0003` the
 * SP belongs to the dispatcher, not to the caller.
 *
 * Both rules were built and measured against it, at $1009, the instruction
 * after `call $0003` at $1006 returns:
 *
 *   - "the latest earlier boundary whose SP is no deeper than this one"
 *     landed at $0675 with SP $FF04 - 149 bytes ABOVE the caller's stack,
 *     inside the dispatcher.
 *   - counting CALL/RET/RST and accepted interrupts landed at $0639,
 *     because the callee returns 4 levels shallower than it was entered:
 *     the stack switch discards the frames the count was following.
 *
 * Tuning either rule until this one case came out right is exactly the
 * failure mode ROADMAP.md's guiding principle describes, so neither is
 * shipped. p2500_history_prev_pc() answers the same question exactly: the
 * caller's address is on screen in the listing, and "the last time the PC
 * was there" needs no model of the stack at all.
 */

/* Run backwards to the last time an enabled breakpoint in `d` was standing
 * at the PC, strictly before the current position. Returns false (changing
 * nothing) when there is no such moment inside the recorded history - which
 * includes "there are no breakpoints". Only brk[] is consulted: watches
 * and counters have side effects and a rewind must have none. */
bool p2500_history_prev_break(P2500History *h, P2500Machine *m,
                              const P2500Debug *d);

/* Run backwards to the last time the PC was `addr`, strictly before the
 * current position - "where did we last come through here?". Independent of
 * the breakpoint table, so it answers that without disturbing it. */
bool p2500_history_prev_pc(P2500History *h, P2500Machine *m, uint16_t addr);

/* A fingerprint of the entire machine, for asserting that a state reached
 * by replay is the state that was there before - which is the one
 * assumption this whole file rests on. FNV-1a over the struct's bytes,
 * including the attachment pointers, so it is comparable only within one
 * process; that is all --verify-rewind needs. */
unsigned long long p2500_history_digest(const P2500Machine *m);

#ifdef __cplusplus
}
#endif

#endif
