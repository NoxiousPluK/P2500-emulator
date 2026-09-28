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
#include "core/log.h"

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
    bool new_watch_stops = false;   /* arm newly added watches as watchpoints */
    bool mem_follow_pc = false;
    /* Where an address control ended up on screen, so a headless run can
     * click it without the suite pinning pixel columns that any layout
     * change would invalidate. -1 until the panel has been drawn. */
    struct Geom { float field_x = -1.0f, field_y = -1.0f,
                        button_x = -1.0f, button_y = -1.0f; };
    Geom goto_geom;
    Geom watch_geom;

    /* Disassembly. */
    bool disasm_follow_pc = true;
    uint16_t disasm_addr = 0x0000;
    char disasm_entry[8] = "";
    char break_entry[8] = "";
    char watch_entry[12] = "";

    /* The log: every diagnostic the core produces (TODO.md T34), plus the
     * watch hits and breakpoint stops core/debug.c reports. A ring, so a
     * long run cannot grow without bound - and one deep enough to hold a
     * whole disk operation's worth of the firehose, which is the point of
     * having it at all. */
    static const int LOG_CAP = 2048;
    static const int LOG_LEN = 176;
    char log[LOG_CAP][LOG_LEN] = {};
    unsigned char log_level[LOG_CAP] = {};
    int log_head = 0;   /* next slot to write */
    int log_count = 0;
    unsigned long log_total = 0; /* including what has scrolled out of the ring */
    bool log_autoscroll = true;
    bool log_show[3] = { true, true, true }; /* by P2500LogLevel */
    char log_filter[32] = "";
    /* The device `verbose` switches, all together. Off by default: with
     * them on the core emits a few thousand lines per emulated second, and
     * a log nobody can read is not instrumentation. */
    bool verbose_devices = false;
};

/* What the user asked the run loop to do this frame. The panels never step
 * the machine themselves - pacing belongs to the frame loop. */
struct P2500PanelActions {
    unsigned long steps = 0; /* instructions to execute, honouring breakpoints */
    bool run_to_cursor = false;
    bool reset_baseline = false; /* watches need re-baselining */
};

void p2500_panels_log(P2500Panels &p, P2500LogLevel level, const char *category,
                      const char *fmt, ...);

/* The sink to hand p2500_set_log(); `userdata` must be the P2500Panels. */
void p2500_panels_log_sink(void *userdata, P2500LogLevel level,
                           const char *category, const char *message);

/* The contents of the View menu - drawn by main.cpp inside its menu bar so
 * the bar stays one thing in one place. */
void p2500_panels_menu(P2500Panels &p);

/* Draw every visible panel. `paused` is the front-end's own run state;
 * `toggle_pause` is set when a panel's Run/Pause button was pressed. */
P2500PanelActions p2500_panels_draw(P2500Panels &p, P2500Machine &m,
                                    P2500Debug &dbg, bool paused,
                                    bool *toggle_pause);

#endif
