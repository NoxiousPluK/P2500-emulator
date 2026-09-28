#ifndef P2500_GUI_PANELS_H
#define P2500_GUI_PANELS_H

/*
 * The debugger panels (TODO.md T39). Separate from main.cpp because the
 * front-end's job - window, pacing, input, screen - is already a file's
 * worth, and because the panels only ever read the machine plus the shared
 * P2500Debug: keeping them in their own translation unit is what makes that
 * visible at a glance.
 *
 * Everything the panels show comes out of core/debug.h, so the GUI cannot
 * drift from what `p2500-emu --state` and `--disasm` report.
 */

#include "core/machine.h"
#include "core/debug.h"

struct P2500Panels {
    bool show_devices = false;
    bool show_memory = false;
    bool show_disasm = false;
    bool show_log = false;

    /* Memory viewer. The three planes are genuinely different address
     * spaces, not three views of one: the CPU view is bank-aware
     * (p2500_peek), while the character and attribute planes are the video
     * card's own 16K, which the CPU only ever sees a window of. */
    int mem_plane = 0; /* 0 CPU, 1 video characters, 2 video attributes */
    int mem_goto = -1; /* >= 0 for one frame after a jump is requested */
    char mem_entry[8] = "";
    bool mem_follow_pc = false;

    /* Disassembly. */
    bool disasm_follow_pc = true;
    uint16_t disasm_addr = 0x0000;
    char disasm_entry[8] = "";
    char break_entry[8] = "";
    char watch_entry[12] = "";

    /* Event log: watch hits and breakpoint stops, i.e. whatever the shared
     * debug code reports. A ring, so a long run cannot grow without bound.
     * Not the port/IRQ log yet - that needs the core's 63 stderr writes
     * behind a callback first (TODO.md T34). */
    static const int LOG_CAP = 400;
    static const int LOG_LEN = 176;
    char log[LOG_CAP][LOG_LEN] = {};
    int log_head = 0;   /* next slot to write */
    int log_count = 0;
    bool log_autoscroll = true;
};

/* What the user asked the run loop to do this frame. The panels never step
 * the machine themselves - pacing belongs to the frame loop. */
struct P2500PanelActions {
    unsigned long steps = 0; /* instructions to execute, honouring breakpoints */
    bool run_to_cursor = false;
    bool reset_baseline = false; /* watches need re-baselining */
};

void p2500_panels_log(P2500Panels &p, const char *fmt, ...);

/* The contents of the View menu - drawn by main.cpp inside its menu bar so
 * the bar stays one thing in one place. */
void p2500_panels_menu(P2500Panels &p);

/* Draw every visible panel. `paused` is the front-end's own run state;
 * `toggle_pause` is set when a panel's Run/Pause button was pressed. */
P2500PanelActions p2500_panels_draw(P2500Panels &p, P2500Machine &m,
                                    P2500Debug &dbg, bool paused,
                                    bool *toggle_pause);

#endif
