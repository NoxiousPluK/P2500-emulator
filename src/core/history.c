#include <stdlib.h>
#include <string.h>

#include "history.h"

/*
 * A ring of whole-machine snapshots, each tagged with the instruction count
 * it was taken at, plus a silent replay to get between them. See history.h
 * for why that is enough here and what it deliberately does not cover.
 */

struct P2500History {
    /* Which machine these snapshots belong to. P2500Machine is
     * non-relocatable, so restoring into a different object would hand back
     * a machine whose devices point into someone else's RAM; a seek on the
     * wrong object fails instead. */
    const P2500Machine *owner;
    P2500Machine *slot;                      /* P2500_HISTORY_SLOTS of them */
    unsigned long at[P2500_HISTORY_SLOTS];   /* total_instructions per slot */
    int head;                                /* next slot to write */
    int count;                               /* slots in use, <= SLOTS */
    unsigned long last_cyc;                  /* cpu.cyc at the last capture */
    bool have_last;
};

P2500History *p2500_history_create(void) {
    P2500History *h = (P2500History *)calloc(1, sizeof *h);
    if (!h) return NULL;
    h->slot = (P2500Machine *)calloc(P2500_HISTORY_SLOTS, sizeof *h->slot);
    if (!h->slot) { free(h); return NULL; }
    return h;
}

void p2500_history_destroy(P2500History *h) {
    if (!h) return;
    free(h->slot);
    free(h);
}

void p2500_history_clear(P2500History *h) {
    if (!h) return;
    h->head = 0;
    h->count = 0;
    h->have_last = false;
    h->owner = NULL;
}

void p2500_history_mark(P2500History *h, const P2500Machine *m) {
    if (!h) return;
    /* A different machine object means the old snapshots describe something
     * this history can no longer be restored into. */
    if (h->owner && h->owner != m) p2500_history_clear(h);
    if (h->have_last) {
        /* Unsigned, so this is also right across the T-state counter's own
         * wrap. A caller that steps BACKWARDS without going through seek()
         * would look overdue rather than negative; seek() keeps the cadence
         * honest by resetting it itself. */
        if (m->cpu.cyc - h->last_cyc < P2500_HISTORY_INTERVAL_TSTATES) return;
    }
    h->owner = m;
    memcpy(&h->slot[h->head], m, sizeof *m);
    h->at[h->head] = m->total_instructions;
    h->head = (h->head + 1) % P2500_HISTORY_SLOTS;
    if (h->count < P2500_HISTORY_SLOTS) h->count++;
    h->last_cyc = m->cpu.cyc;
    h->have_last = true;
}

/* Entries occupy the `count` slots ending just before `head`, oldest
 * first: k == 0 is the oldest held, k == count-1 the newest. */
static int slot_index(const P2500History *h, int k) {
    return (h->head - h->count + k + 2 * P2500_HISTORY_SLOTS) % P2500_HISTORY_SLOTS;
}

int p2500_history_depth(const P2500History *h) { return h ? h->count : 0; }

bool p2500_history_span(const P2500History *h, const P2500Machine *m,
                        unsigned long *oldest, unsigned long *newest) {
    if (!h || h->count == 0) return false;
    if (oldest) *oldest = h->at[slot_index(h, 0)];
    if (newest) *newest = m->total_instructions;
    return true;
}

/* The newest snapshot at or before `target`, as a chronological position
 * (see slot_index), or -1. Entries are chronological, so this is a walk
 * back from the newest. */
static int pos_at_or_before(const P2500History *h, unsigned long target) {
    for (int k = h->count - 1; k >= 0; k--) {
        if (h->at[slot_index(h, k)] <= target) return k;
    }
    return -1;
}

/* Forget every snapshot newer than `keep` - which, because entries are
 * chronological, is a walk back from the newest end. */
static void drop_after(P2500History *h, unsigned long keep) {
    while (h->count > 0) {
        const int newest = slot_index(h, h->count - 1);
        if (h->at[newest] <= keep) break;
        h->head = newest;
        h->count--;
    }
}

/* Put the machine back to a snapshot, keeping what the front end attached:
 * the snapshot's own disk/cartridge/keystroke pointers and log sink are
 * discarded in favour of the live ones. */
static void restore(P2500History *h, P2500Machine *m, int idx) {
    P2500Attachments att;
    p2500_attachments_save(m, &att);
    memcpy(m, &h->slot[idx], sizeof *m);
    p2500_attachments_restore(m, &att);
}

/* Replay to `target` with the log detached, and never through
 * p2500_debug_before_step(): a rewind must have no side effects.
 *
 * Writable media is the one side effect the machine struct cannot hold -
 * it is the front end's buffer - so a replay would commit every WRITE
 * DATA it passes a second time. The FDC is told to stage the bytes and
 * drop them instead (fdc.h's suppress_writes), which leaves the disk
 * exactly as the first pass left it. Saved and put back rather than
 * cleared, so a nested replay cannot un-suppress its caller's. */
static void replay_to(P2500Machine *m, unsigned long target) {
    P2500Log log = m->log;
    const bool was_suppressing = m->fdc.suppress_writes;
    m->log.fn = NULL;
    m->log.userdata = NULL;
    m->fdc.suppress_writes = true;
    while (m->total_instructions < target) p2500_step(m);
    m->fdc.suppress_writes = was_suppressing;
    m->log = log;
}

bool p2500_history_seek(P2500History *h, P2500Machine *m, unsigned long target) {
    if (target >= m->total_instructions) {
        /* Forward: no snapshot needed, the machine is already behind the
         * target. Still silent - a seek is navigation, not a run. */
        replay_to(m, target);
        if (h) h->have_last = false; /* the cadence restarts from here */
        return true;
    }
    if (!h || h->owner != m) return false;
    const int pos = pos_at_or_before(h, target);
    if (pos < 0) return false;
    restore(h, m, slot_index(h, pos));
    replay_to(m, target);
    /* Snapshots ahead of where we now are describe a future this machine
     * may not take again - the user can type something, or step somewhere
     * else, from here - so drop them rather than keep a history that is
     * only valid if nothing happens. */
    drop_after(h, m->total_instructions);
    h->have_last = false;
    return true;
}

bool p2500_history_step_back(P2500History *h, P2500Machine *m, unsigned long n) {
    if (n == 0) return true;
    if (m->total_instructions < n) return false;
    return p2500_history_seek(h, m, m->total_instructions - n);
}

/* ------------------------------------------------------- the backward scan *
 *
 * Both "go back to the last X" operations are one forward scan from the
 * nearest snapshot, remembering the latest boundary that matched and then
 * seeking to it. Forwards, because a Z80 instruction stream cannot be
 * walked backwards - the same reason the disassembly panel lists forwards
 * only.
 *
 * There is deliberately no reverse step-OVER here, and it is not an
 * oversight: see the note in history.h.
 */

typedef enum {
    SCAN_BREAK,  /* a boundary standing on one of d's enabled breakpoints */
    SCAN_PC,     /* a boundary at `addr` */
} ScanMode;

static long scan_back(P2500History *h, P2500Machine *m, const P2500Debug *d,
                      uint16_t addr, ScanMode mode) {
    if (!h || h->owner != m) return -1;
    const unsigned long now = m->total_instructions;
    if (now == 0) return -1;
    int pos = pos_at_or_before(h, now - 1);
    if (pos < 0) return -1;

    /* restore() keeps the live attachments, the log sink included, so this
     * only has to silence the scan itself. */
    const P2500Log log = m->log;
    const bool was_suppressing = m->fdc.suppress_writes;
    long found = -1;

    /* Newest window first, then back one snapshot at a time, stopping at
     * the first window that holds a match - which is therefore the LAST
     * time it happened. Scanning the whole history in one pass would find
     * the same instruction and replay up to 64 fields to do it; this
     * usually answers from the newest window alone. */
    unsigned long end = now;
    for (; pos >= 0 && found < 0; pos--) {
        const int idx = slot_index(h, pos);
        restore(h, m, idx);
        m->log.fn = NULL;
        m->log.userdata = NULL;
        /* Set inside the loop: restore() brings the snapshot's own copy of
         * this flag with it, so it has to be re-asserted after each one. */
        m->fdc.suppress_writes = true;
        while (m->total_instructions < end) {
            if (mode == SCAN_PC) {
                if (m->cpu.pc == addr) found = (long)m->total_instructions;
            } else if (d) {
                for (int i = 0; i < d->breaks; i++)
                    if (d->brk[i].enabled && d->brk[i].addr == m->cpu.pc) {
                        found = (long)m->total_instructions;
                        break;
                    }
            }
            p2500_step(m);
        }
        end = h->at[idx];   /* the next window older than this one */
    }
    m->log = log;
    m->fdc.suppress_writes = was_suppressing;

    if (found >= 0) {
        /* Land on it. A snapshot at or before it was just restored, so this
         * cannot fail. */
        p2500_history_seek(h, m, (unsigned long)found);
    } else {
        /* Nothing matched anywhere in the history: put the machine back
         * exactly where it was. */
        restore(h, m, slot_index(h, pos_at_or_before(h, now - 1)));
        replay_to(m, now);
    }
    return found;
}

bool p2500_history_prev_break(P2500History *h, P2500Machine *m,
                              const P2500Debug *d) {
    if (!d || d->breaks == 0) return false;
    return scan_back(h, m, d, 0, SCAN_BREAK) >= 0;
}

bool p2500_history_prev_pc(P2500History *h, P2500Machine *m, uint16_t addr) {
    return scan_back(h, m, NULL, addr, SCAN_PC) >= 0;
}

unsigned long long p2500_history_digest(const P2500Machine *m) {
    const unsigned char *p = (const unsigned char *)m;
    unsigned long long hash = 1469598103934665603ULL; /* FNV-1a 64 offset */
    for (size_t i = 0; i < sizeof *m; i++) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}
