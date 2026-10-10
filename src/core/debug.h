#ifndef P2500_DEBUG_H
#define P2500_DEBUG_H

#include "machine.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Debug services for both front-ends.
 *
 * The CLI grew --watch, --count and --break as private structs inside its
 * own step loop, and the GUI needs exactly the same three things. They live
 * here so there is one implementation, and so the answer to "does the panel
 * agree with the harness?" is "it is the same code" rather than an
 * inspection of two.
 *
 * Same argument for the state lines at the bottom: an ImGui table and a
 * text dump are two presentations, but the *derivations* - which CTC bit
 * means /256, what an FDC phase is called - must not be written twice.
 *
 * C11, no file-scope state, no dependency beyond libc: the same rules the
 * rest of src/core/ follows.
 */

typedef struct P2500Debug P2500Debug;

/* ---------------------------------------------------------------- disasm */

/* Disassemble the instruction at `addr` as the CPU would see it - through
 * p2500_peek, so it is bank-aware and reads RAM, EPROM or the video window
 * as currently mapped. Writes a z80dasm-style lowercase mnemonic into `buf`
 * and returns the instruction's length in bytes, always 1..4, so a caller
 * can walk forward without needing a second lookup. */
int p2500_disasm(const P2500Machine *m, uint16_t addr, char *buf, size_t buflen);

/* The same, with any 16-bit operand that has a symbol attached printed as
 * that name - `call CONIN` rather than `call $E209`. `d` may be NULL, in
 * which case this is exactly p2500_disasm(). Only full 4-digit addresses
 * are candidates, so an 8-bit immediate is never mistaken for one. */
int p2500_disasm_sym(const P2500Machine *m, const P2500Debug *d, uint16_t addr,
                     char *buf, size_t buflen);

/* ----------------------------------------- watches, counters, breakpoints */

#define P2500_DEBUG_MAX_WATCHES 16
#define P2500_DEBUG_MAX_COUNTS 16
#define P2500_DEBUG_MAX_BREAKS 16
#define P2500_DEBUG_WATCH_MAX_LEN 64

typedef struct {
    uint16_t addr;
    uint16_t len;
    uint8_t last[P2500_DEBUG_WATCH_MAX_LEN];
    bool enabled;
    /* Stop the run when this byte changes - a watchpoint, not just a log
     * line. Note what it can and cannot see: the check is a poll made
     * between instructions, so it catches every CHANGE but no write that
     * stores the value already there. Hunting "what put this here" is what
     * it is for, and that is always a change. */
    bool stop;
} P2500Watch;

typedef struct {
    uint16_t addr;
    unsigned long hits;
    unsigned long first_step;
    bool enabled;
} P2500Counter;

typedef struct {
    uint16_t addr;
    bool enabled;
} P2500Breakpoint;

/* ---------------------------------------------------------------- symbols */

/* Names for addresses, so a listing reads `call SELDSK` instead of
 * `call $E620`. Nothing here is a constant of the machine: CP/M's BIOS
 * lives wherever this disk's CBIOS put it, which is why the table is
 * derived from live memory (p2500_debug_scan_symbols) rather than written
 * down. 14 characters holds the longest name this produces,
 * `SECTRAN.body`, with room to spare. */
#define P2500_DEBUG_MAX_SYMBOLS 64
#define P2500_DEBUG_SYMBOL_LEN 14

typedef struct {
    uint16_t addr;
    char name[P2500_DEBUG_SYMBOL_LEN];
} P2500Symbol;

/* A watched byte changed. `text` is the fully formatted one-line report,
 * identical in both front-ends; the raw fields are there so a caller can
 * colour or filter it. */
typedef void (*P2500DebugWatchFn)(void *userdata, uint16_t addr,
                                  uint8_t was, uint8_t now, const char *text);

struct P2500Debug {
    P2500Watch watch[P2500_DEBUG_MAX_WATCHES];
    int watches;
    P2500Counter count[P2500_DEBUG_MAX_COUNTS];
    int counts;
    P2500Breakpoint brk[P2500_DEBUG_MAX_BREAKS];
    int breaks;
    P2500Symbol sym[P2500_DEBUG_MAX_SYMBOLS];
    int syms;

    /* The one-shot breakpoint behind run-to-cursor and step-over. Not an
     * entry in brk[]: it must not appear in the user's breakpoint list, and
     * it must disarm itself the moment it fires. `sp_floor` is what makes a
     * step-over survive recursion - the return address is reached many
     * times by a function that calls itself, but only once with the stack
     * back where the step started, so a stop is only taken at or above that
     * level. */
    uint16_t one_shot;
    bool one_shot_armed;
    bool one_shot_use_sp;
    uint16_t one_shot_sp_floor;

    P2500DebugWatchFn on_watch;
    void *userdata;

    /* Index into watch[] of the watchpoint that stopped the run, else -1,
     * and the byte within it that changed. */
    int hit_watch;
    uint16_t hit_watch_addr;

    /* Index into brk[] of the breakpoint that stopped the run, else -1.
     * Cleared by p2500_debug_resume() so a caller that steps off the
     * breakpoint does not immediately re-trigger on the same address. */
    int hit_break;
    bool skip_one; /* set by resume: ignore breakpoints for exactly one step */
    bool hit_one_shot; /* the stop came from run-to-cursor / step-over */
    uint16_t last_pc; /* PC of the previous step - the instruction that wrote */
};

void p2500_debug_init(P2500Debug *d);

/* All three return the new entry's index, or -1 if the table is full.
 * Adding an address that is already present returns the existing index
 * rather than duplicating it. */
int p2500_debug_add_watch(P2500Debug *d, uint16_t addr, uint16_t len);
int p2500_debug_add_count(P2500Debug *d, uint16_t addr);
int p2500_debug_add_break(P2500Debug *d, uint16_t addr);

/* Remove by index, closing the gap. */
void p2500_debug_remove_watch(P2500Debug *d, int index);
void p2500_debug_remove_count(P2500Debug *d, int index);
void p2500_debug_remove_break(P2500Debug *d, int index);

/* Names. add_symbol keeps the FIRST name given for an address, so a name
 * the user supplied survives a later scan. Returns the entry's index, or -1
 * if the table is full. `p2500_debug_symbol` returns NULL when nothing is
 * named at that exact address. */
int p2500_debug_add_symbol(P2500Debug *d, uint16_t addr, const char *name);
void p2500_debug_clear_symbols(P2500Debug *d);
const char *p2500_debug_symbol(const P2500Debug *d, uint16_t addr);

/* Derive CP/M's own names from live memory: the BDOS entry at $0005, the
 * BDOS and CBIOS bodies they point at, and all 17 CBIOS jump-table slots
 * with their targets. Returns how many names were added, and 0 - changing
 * nothing - when page zero does not hold CP/M's vectors, which is the
 * normal answer before the boot finishes. Safe to call repeatedly. */
int p2500_debug_scan_symbols(P2500Debug *d, const P2500Machine *m);

/* Run until `addr` is reached, once. Replaces any armed one-shot. */
void p2500_debug_run_to(P2500Debug *d, uint16_t addr);
/* Step over the instruction at the current PC: for a CALL or RST, run
 * until it returns to the following instruction with the stack no deeper
 * than it is now; for anything else this is a plain single step, and the
 * caller can tell which by the return value (true = a one-shot was armed
 * and the machine must be run, false = just step once). */
bool p2500_debug_step_over(P2500Debug *d, const P2500Machine *m);

int p2500_debug_find_break(const P2500Debug *d, uint16_t addr); /* -1 if none */
/* Add if absent, remove if present - what clicking a disassembly line does. */
void p2500_debug_toggle_break(P2500Debug *d, uint16_t addr);

/* Baseline every watch against memory as it is now. Call once the machine
 * is loaded, and again after p2500_reset(), or the first "change" reported
 * is the whole boot process arriving at once. */
void p2500_debug_baseline(P2500Debug *d, const P2500Machine *m);

/* Call immediately before each p2500_step(). Reports watch changes through
 * on_watch, tallies counters at the current PC, and returns true when the
 * run should stop: either PC sits on an enabled breakpoint - in which case
 * the caller must stop *before* executing, so the machine state describes
 * that exact moment - or a watch marked `stop` has just seen its byte
 * change, in which case the writing instruction has already run and the
 * machine is standing on the one after it. */
bool p2500_debug_before_step(P2500Debug *d, const P2500Machine *m,
                             unsigned long step);

/* Clear a breakpoint stop and arm a one-step bypass, so "continue" from a
 * breakpoint makes progress instead of stopping on it again. */
void p2500_debug_resume(P2500Debug *d);

/* ------------------------------------------------------------ state lines */

typedef enum {
    P2500_DBG_CPU = 0,
    P2500_DBG_INT,
    P2500_DBG_CTC,
    P2500_DBG_PIO,
    P2500_DBG_DMA,
    P2500_DBG_FDC,
    P2500_DBG_CRTC,
    P2500_DBG_VIDEO,
    P2500_DBG_MISC,
    P2500_DBG_TOPICS
} P2500DebugTopic;

const char *p2500_debug_topic_name(P2500DebugTopic topic);

/* How many label/value lines this topic has right now. Not a constant:
 * the FDC's command and result phases contribute lines only while they
 * hold bytes. */
int p2500_debug_topic_lines(const P2500Machine *m, P2500DebugTopic topic);

/* Fill one line. `line` must be < p2500_debug_topic_lines(). Either buffer
 * may be NULL if the caller only wants the other. Both are always
 * NUL-terminated on return. */
void p2500_debug_line(const P2500Machine *m, P2500DebugTopic topic, int line,
                      char *label, size_t label_n, char *value, size_t value_n);

#ifdef __cplusplus
}
#endif

#endif
