/*
 * p2500-gui - SDL3 + Dear ImGui front-end.
 *
 * The only C++ in the tree, and it stays that way: libp2500.a is C11 with
 * no dependencies and nothing outside src/gui/ may include ImGui.
 *
 * Uses SDL3's callback app model (SDL_AppInit/Iterate/Event) rather than a
 * hand-rolled main loop: it is the shape SDL3 is designed around and it is
 * what makes an Emscripten build nearly free.
 *
 * Single-threaded on purpose. The machine runs about ten times faster than
 * real time, so one frame's worth of emulation per presented frame has
 * comfortable headroom, and one thread is what lets the debugger panels be
 * written without a single lock.
 *
 * Pacing: one video field is P2500_TSTATES_PER_FRAME T-states, which
 * is the same event as the machine's own 50 Hz clock strobe. That makes one
 * field per iteration the right unit of work - but it says nothing about
 * how often an iteration happens, and SDL_AppIterate is called as fast as
 * the host allows. Without a throttle the guest would run at whatever rate
 * that is, tens of times too fast. The two clocks match because
 * pace_field() makes them, against the wall clock - so the speed
 * does not depend on the monitor's refresh rate either.
 *
 * The speed setting does not change that period. It changes how much
 * emulated time one iteration covers: half a field at 0.5x, two fields at
 * 2x. Keeping the period fixed is what holds the picture at 50 Hz however
 * slowly the guest is running, and it is also the only way up - the
 * presented frame rate is capped by vsync, so a 4x speed cannot come from
 * presenting four times as often.
 *
 * The menu bar is drawn by ImGui rather than being a native one: the
 * native options cover Windows and macOS, and on
 * Wayland there is no way to attach a GTK menu to an SDL window nor a
 * global menu to export to.
 */

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#include "core/machine.h"
#include "core/debug.h"
#include "core/sysinfo.h"
#include "core/history.h"
#include "core/video.h"
#include "core/version.h"

#include "icon.h"
#include "prefs.h"
#include "panels.h"

#define SCALE_DEFAULT 2
#define MAX_PUSHES 8

/* Height reserved for the menu bar when sizing the default window, before
 * ImGui has measured the real one. */
#define MENU_RESERVE 24

/* The emulated screen is 640x288 but the real machine drove a 4:3 monitor,
 * so its dots are not square: fitting 640x288 into 4:3 gives a pixel aspect
 * of (640/288)/(4/3) = 1.667. That is the user-visible definition of "the
 * image is 4:3" and is what ASPECT_43 draws.
 *
 * It assumes the active area fills the glass, which it does not. The CRTC
 * raster is 98 char-times x 311 scanlines (TODO.md T29), of which 80x288 is
 * active - 82% of the raster width and 93% of its height - so the true dot
 * aspect is 1.89. Deliberately not modelled: filling the raster would put a
 * permanent border inside the render area, and usability on a modern display
 * was chosen over that. */
#define DISPLAY_ASPECT_W 4
#define DISPLAY_ASPECT_H 3

/* How far a snapped 4:3 rect may stray from 4:3 before snapping is simply
 * not applied. The exact integer answer is the dot pair 3:5 - (640*3)/(288*5)
 * is 4:3 to the pixel - but that only fits a window 1920 dots wide, so
 * smaller windows have to settle for the nearest pair that does fit. 2x3 is
 * the realistic one and is 1/9 out, which sets the floor for this number.
 * Below the tolerance the exact (unsnapped) rect is drawn instead: the user
 * asked for 4:3, and snapping is a refinement on top of it rather than a
 * licence to abandon it. */
#define SNAP_ASPECT_TOLERANCE 0.12f

/* Bound on the integer prescale multiple, so a huge window cannot ask for an
 * unreasonable intermediate texture. 8 covers 640x288 -> 5120x2304. */
#define PRESCALE_MAX 8

/* How long a resize has to stand still before the new size is remembered.
 * The window geometry is the first tier-one setting that is not a
 * value picked from a menu: dragging an edge produces a resize event per
 * frame, and writing the file on each one would be hundreds of writes for
 * one gesture. So the size is held pending and only becomes the setting
 * once it has stopped changing, after which the ordinary write-on-change
 * path in prefs_sync() does the rest - a resize that is still in progress
 * when the emulator is closed is simply not remembered, which is the right
 * answer for a gesture that was never finished. */
#define WINDOW_SETTLE_MS 400

/* Pixel aspect (ASPECT_43 | ASPECT_SQUARE) and the interface theme
 * (THEME_DARK | THEME_PAPER) are declared in prefs.h, along with every
 * accepted spelling of them: the settings file, P2500_DISPLAY and the flags
 * all have to agree, and only one of the three is in this file. 4:3 is the
 * default - it is what the machine looked like, and what anyone who used
 * one remembers. */

/* Named rather than an anonymous typedef: it holds C++ members with
 * default initialisers now, which an anonymous struct cannot carry across a
 * typedef without tripping -Wnon-c-typedef-for-linkage. */
struct App {
    P2500Machine m;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *screen;
    /* Integer-multiple intermediate for the two-pass scaler; NULL until a
     * non-integer destination rect first needs one. */
    SDL_Texture *prescale;
    int prescale_w, prescale_h;
    int aspect;                  /* ASPECT_43 | ASPECT_SQUARE */
    P2500PhosphorId phosphor;    /* screen colours, from the core's table */
    int theme;                   /* THEME_DARK | THEME_PAPER (the UI, not the screen) */
    bool snap;                   /* snap the screen rect to whole dots */
    P2500VideoInfo info;
    uint32_t *fb;
    uint8_t *disk[P2500_FDC_MAX_DRIVES];
    char disk_name[P2500_FDC_MAX_DRIVES][64]; /* basename, for the drive lamps */
    /* What the image IS rather than what it is called - the label of the
     * `# match-disk` rule that recognised it, empty when no shipped rule
     * does. Answered at attach time, before the guest has read a sector. */
    char disk_id[P2500_FDC_MAX_DRIVES][96];
    unsigned long frames;
    unsigned long frame_limit;   /* 0 = run until the user quits */
    const char *shot_path;       /* emulated screen only, for the CLI comparison */
    const char *win_shot_path;   /* the whole composited window, UI included */
    /* The window size as a SETTING: the last size that stood still for
     * WINDOW_SETTLE_MS, which is what prefs_sync() writes. `pend_at` is 0
     * when no resize is in flight; SDL_GetTicks() is never 0 by the time a
     * window exists, so that sentinel is safe. */
    int win_w, win_h;
    int pend_w, pend_h;
    Uint64 pend_at;
    /* Scripted keystrokes, identical in effect to a real keypress - they go
     * through p2500_keyboard_push like SDL_EVENT_TEXT_INPUT does. Present so
     * the front-end can be driven to a real application with no display,
     * which is what makes a GUI-vs-CLI render comparison possible. */
    struct { unsigned long at_ms; const char *text; bool done; } push[MAX_PUSHES];
    int pushes;
    bool paused;
    /* Speed: a multiplier on how fast emulated time runs against the
     * wall clock. 1 is the real machine; 0 means unlimited, i.e. whatever
     * this host manages. */
    float speed = 1.0f;
    double tstate_carry = 0.0;    /* the fraction of a field owed below 1x */
    /* What the machine is ACTUALLY managing, measured rather than assumed:
     * a set speed the host cannot reach is a claim, not a reading. */
    float speed_measured = 0.0f;
    Uint64 speed_ns = 0;          /* start of the measurement window */
    unsigned long speed_cyc = 0;
    /* Where the speed list's entries landed, reported to the log the first
     * time it opens so a headless run can click one. */
    float speed_item_x = 0.0f, speed_item_y[8] = {0};
    bool speed_popup_logged = false;
    /* The machine boots with capitals lock ENGAGED: $E34C in the shipped
     * CBIOS image is $20, and CONIN XORs alphabetic input with it ($E4B7),
     * so an unshifted key produces a capital. The manual documents a
     * capitals-lock key that toggles it, but that key is handled by the
     * keyboard controller, which this emulator does not model - so the
     * equivalent is offered here, at the boundary where we synthesise the
     * keystrokes. Checked means the machine behaves as shipped. */
    bool caps_lock;
    bool quit;
    /* Watches, counters and breakpoints, shared with the CLI.
     * Empty by default, which is what lets the run loop keep its
     * whole-frame fast path until the user actually asks for something. */
    P2500Debug dbg;
    /* --symbols files, kept so they can be re-applied: a reset throws the
     * symbol table away (the next disk's CBIOS lands elsewhere), but names
     * the user supplied by hand are not the machine's to forget. */
    static const int MAX_SYMFILES = 4;
    const char *symfile[MAX_SYMFILES] = {0};
    int symfiles = 0;
    /* Auto-identification: with no --symbols given, watch for a build
     * whose fingerprint one of the shipped files claims, and say so once.
     * Re-armed by a reset or a change of media, because the answer can
     * change - loading P25K_G after P25K_B is a different BIOS. */
    /* How many drive letters the RUNNING BIOS maps, read out of its own DPH
     * chain (core/sysinfo.h). -1 = not knowable yet: before CP/M is up, or
     * on a guest that is not CP/M at all, there is no chain to read and the
     * drive lamps must not pretend otherwise. Refreshed on the same cadence
     * as the symbol re-check rather than per frame, because the scan is a
     * sweep of BIOS space and the answer changes only at a warm boot. */
    int bios_drive_letters = -1;
    bool symbols_auto = true;
    char symbols_auto_from[256] = {0};
    /* Execution history, so the debugger can step BACKWARDS - a ring of
     * whole-machine snapshots plus a silent replay (core/history.h). Heap
     * allocated because it is ~7 MB, and cleared whenever the past stops
     * being replayable: a reset, or a change of media. */
    P2500History *hist;
    P2500Panels panels;
    bool has_ui;                 /* false only if ImGui failed to initialise */
    float menu_h;                /* measured each frame, offsets the screen */
    /* Scripted pointer, for the same reason --push-at exists: the lamps in
     * the menu bar are controls now, and a control that can only be checked
     * by a person looking at it is a control nothing guards. -1 disables. */
    /* A scripted pointer: move to a place, optionally click, at a given
     * frame. Several steps, because the interesting questions are about
     * sequences - clicking a field and then clicking away from it. */
    struct { unsigned long at_frame; int x, y, button; } mouse[MAX_PUSHES];
    int mouse_steps = 0;
    int mouse_x = -1, mouse_y = -1;   /* where the script has left it */
    bool ui_had_keyboard = false;     /* to log capture/release transitions */
    bool text_input_was = true;       /* SDL_StartTextInput() runs at init */
    /* Characters typed into whatever ImGui widget has focus - the keyboard
     * half of --mouse, and the only way to drive an address field headlessly. */
    struct { unsigned long at_frame; const char *text; } ui_type[MAX_PUSHES];
    int ui_types = 0;
    /* Scripted window resizes, the same kind of test hook as --mouse and
     * --ui-type: SDL_SetWindowSize produces a real SDL_EVENT_WINDOW_RESIZED,
     * so the settle timer and the write path are driven by the event they
     * will see in a user's hands rather than by a back door into the
     * setting. Without it nothing headless can resize a window at all. */
    struct { unsigned long at_frame; int w, h; } resize[MAX_PUSHES];
    int resizes = 0;
    int watches_was = 0;
    Uint64 next_field_ns;        /* when the next 50 Hz field is due */
    Uint64 start_ns;
    bool force_pacing;           /* --paced: pace even a scripted run */
    SDL_AtomicInt disk_pending;  /* set by the file-dialog callback */
    char disk_path[1024];
    unsigned pending_unit;       /* which drive the dialog was opened for */
    /* Writes land in the mounted image's own buffer and never in the file
     * it was read from - several of this project's images are the only
     * copies there are, so persisting one is an explicit File-menu action.
     * A bit set here mounts that drive with its write-protect tab in
     * place, which the guest sees as WP in SENSE DRIVE STATUS. */
    unsigned write_protect;
    SDL_AtomicInt save_pending;
    char save_path[1024];
    unsigned saving_unit;
    char status[160];            /* last action, shown at the right of the bar */

    /* The preferences file (prefs.h). Resolved once at startup; prefs_on is
     * false under P2500_CONFIG=none, which is how the test suite keeps a
     * developer's saved settings out of a check. prefs_on_disk is what was
     * last written, so a frame in which nothing changed writes nothing. */
    char prefs_path[1024] = {0};
    bool prefs_on = false;
    P2500Prefs prefs_on_disk;

    /* Cartridge slot (port $0F) - SESAM keys are its best-documented use,
     * but the slot is generic, so this also accepts an arbitrary cartridge
     * ROM file. `cart_buf` is kept even while ejected, so the right-click
     * toggle can restore it without the user retyping/reloading. */
    uint8_t *cart_buf;
    size_t cart_buf_len;
    char cart_name[64];           /* basename if loaded from a file; empty = manual key */
    bool cart_inserted;
    bool show_cartridge;          /* Cartridge window open flag */
    bool show_about;              /* About window open flag */
    uint8_t cart_edit_hi, cart_edit_lo; /* live-typed serial bytes, not yet inserted */
    SDL_AtomicInt cart_pending;    /* set by the cartridge file-dialog callback */
    char cart_path[1024];
};

/* The screen colours live in the core (src/core/video.c) so this and
 * p2500-emu share one definition; `phosphor` selects which preset. */

static void set_status(App *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(app->status, sizeof app->status, fmt, ap);
    va_end(ap);
}

static uint8_t *read_whole_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *out_size = (size_t)n;
    return buf;
}

/*
 * Keyboard encoding. Printable characters come from
 * SDL_EVENT_TEXT_INPUT rather than from keysyms, because this machine's
 * keyboard is a multilingual European one and a naive keysym mapping gets
 * the shifted and accented characters wrong. Only the keys that have no
 * text representation are mapped from keysyms here.
 *
 * The cursor keys are the ones CBIOS's own table at $E274 decodes onto the
 * WordStar diamond - read out of a live RAM dump, not guessed.
 */
static int key_to_byte(SDL_Keycode key, SDL_Keymod mod)
{
    if (mod & SDL_KMOD_CTRL) {
        if (key >= SDLK_A && key <= SDLK_Z) return (key - SDLK_A) + 1; /* ^A..^Z */
        if (key == SDLK_LEFTBRACKET) return 0x1B;
    }
    switch (key) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0D;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB:       return 0x09;
    case SDLK_ESCAPE:    return 0x1B;
    case SDLK_DELETE:    return 0x7F;
    case SDLK_UP:        return 0x8B;
    case SDLK_LEFT:      return 0x87;
    case SDLK_RIGHT:     return 0x89;
    case SDLK_DOWN:      return 0x85;
    default: return -1;
    }
}

static void write_ppm(const App *app)
{
    FILE *f = fopen(app->shot_path, "wb");
    if (!f) { SDL_Log("cannot write %s", app->shot_path); return; }
    fprintf(f, "P6\n%d %d\n255\n", app->info.width, app->info.height);
    for (int y = 0; y < app->info.height; y++)
        for (int x = 0; x < app->info.width; x++) {
            uint32_t c = app->fb[(size_t)y * app->info.width + x];
            fputc((int)((c >> 16) & 0xFF), f);
            fputc((int)((c >> 8) & 0xFF), f);
            fputc((int)(c & 0xFF), f);
        }
    fclose(f);
    SDL_Log("wrote %dx%d screenshot to %s after %lu fields",
            app->info.width, app->info.height, app->shot_path, app->frames);
}

/* The whole composited window, menu bar included - read back off the
 * renderer so it works under the dummy video driver, which is what lets the
 * UI be checked with no display. */
static void write_window_ppm(App *app)
{
    SDL_Surface *s = SDL_RenderReadPixels(app->renderer, NULL);
    if (!s) { SDL_Log("RenderReadPixels: %s", SDL_GetError()); return; }
    SDL_Surface *rgb = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGB24);
    SDL_DestroySurface(s);
    if (!rgb) { SDL_Log("ConvertSurface: %s", SDL_GetError()); return; }
    FILE *f = fopen(app->win_shot_path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", rgb->w, rgb->h);
        for (int y = 0; y < rgb->h; y++)
            fwrite((uint8_t *)rgb->pixels + (size_t)y * rgb->pitch, 1, (size_t)rgb->w * 3, f);
        fclose(f);
        SDL_Log("wrote %dx%d window capture to %s", rgb->w, rgb->h, app->win_shot_path);
    }
    SDL_DestroySurface(rgb);
}

/* Menu "Screenshot": a BMP of the emulated screen only, no menu bar in it.
 * BMP because SDL can write it with no extra dependency and every platform
 * opens it; the --screenshot flag stays PPM because the test suite parses
 * that header. */
static void save_screenshot(App *app)
{
    char name[128];
    time_t t = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    strftime(name, sizeof name, "p2500-%Y%m%d-%H%M%S.bmp", &tm_buf);

    SDL_Surface *s = SDL_CreateSurfaceFrom(app->info.width, app->info.height,
                                           SDL_PIXELFORMAT_ARGB8888, app->fb,
                                           app->info.width * (int)sizeof(uint32_t));
    if (!s) { set_status(app, "screenshot failed: %s", SDL_GetError()); return; }
    if (SDL_SaveBMP(s, name)) set_status(app, "saved %s", name);
    else set_status(app, "could not save %s: %s", name, SDL_GetError());
    SDL_DestroySurface(s);
}

/* Fingerprint an image against the symbol files' `# match-disk` rules, so
 * a lamp can say what is in the drive and not just which file was opened.
 * Silence is a real answer here: most images nobody ships a rule for. */
static void identify_disk(App *app, unsigned unit, const uint8_t *buf, size_t n)
{
    app->disk_id[unit][0] = '\0';
    p2500_debug_identify_disk(buf, n, "symbols", app->disk_id[unit],
                              sizeof app->disk_id[unit], NULL, 0);
}

/* SDL may run this on another thread, so it only parks the path and raises a
 * flag; the actual load happens in SDL_AppIterate. */
static void SDLCALL disk_chosen(void *userdata, const char *const *filelist, int)
{
    App *app = (App *)userdata;
    if (!filelist || !filelist[0]) return; /* cancelled, or the dialog failed */
    SDL_strlcpy(app->disk_path, filelist[0], sizeof app->disk_path);
    SDL_SetAtomicInt(&app->disk_pending, 1);
}

static void mount_disk(App *app, unsigned unit, const char *path)
{
    if (unit >= P2500_FDC_MAX_DRIVES) return;
    size_t n = 0;
    uint8_t *buf = read_whole_file(path, &n);
    if (!buf) { set_status(app, "could not read %s", path); return; }
    if (app->disk[unit] && app->m.fdc.sectors_written[unit])
        SDL_Log("drive %c: replaced with %lu written sector(s) unsaved",
                'A' + (int)unit, app->m.fdc.sectors_written[unit]);
    free(app->disk[unit]);
    app->disk[unit] = buf;
    if (app->write_protect & (1u << unit))
        p2500_fdc_attach(&app->m.fdc, unit, buf, n);
    else
        p2500_fdc_attach_writable(&app->m.fdc, unit, buf, n);
    app->m.fdc.sectors_written[unit] = 0;
    /* The snapshots describe a machine that was reading the old disk, and
     * one of them holds a pointer into the buffer just freed. */
    p2500_history_clear(app->hist);
    const char *base = SDL_strrchr(path, '/');
    SDL_strlcpy(app->disk_name[unit], base ? base + 1 : path, sizeof app->disk_name[unit]);
    identify_disk(app, unit, buf, n);
    /* A new disk in A: may be a different BIOS generation, and the names
     * for the old one would then be wrong. Nothing is cleared here - the
     * running machine still holds the old CBIOS until it warm-boots - but
     * the detector is allowed to notice when that changes. */
    if (unit == 0) app->symbols_auto_from[0] = '\0';
    set_status(app, "%c: %s", 'A' + (int)unit, app->disk_name[unit]);
}

static void eject_disk(App *app, unsigned unit)
{
    if (unit >= P2500_FDC_MAX_DRIVES || !app->disk[unit]) return;
    /* An eject throws away whatever the guest wrote, since nothing was
     * written back to a file. Saying so is the point: a silent drop is
     * worse than a refusal, and this is the only notice the user gets. */
    const unsigned long written = app->m.fdc.sectors_written[unit];
    p2500_fdc_attach(&app->m.fdc, unit, NULL, 0);
    app->m.fdc.sectors_written[unit] = 0;
    p2500_history_clear(app->hist);
    free(app->disk[unit]);
    app->disk[unit] = NULL;
    app->disk_name[unit][0] = '\0';
    app->disk_id[unit][0] = '\0';
    if (written)
        set_status(app, "%c: empty - %lu written sector(s) discarded",
                   'A' + (int)unit, written);
    else
        set_status(app, "%c: empty", 'A' + (int)unit);
    SDL_Log("drive %c: ejected (%lu written sector(s) discarded)",
            'A' + (int)unit, written);
}

/* SDL may run this on another thread, so it only parks the path and raises a
 * flag; the actual load happens in SDL_AppIterate. Mirrors disk_chosen(). */
static void SDLCALL cartridge_chosen(void *userdata, const char *const *filelist, int)
{
    App *app = (App *)userdata;
    if (!filelist || !filelist[0]) return; /* cancelled, or the dialog failed */
    SDL_strlcpy(app->cart_path, filelist[0], sizeof app->cart_path);
    SDL_SetAtomicInt(&app->cart_pending, 1);
}

static void mount_cartridge(App *app, const char *path)
{
    size_t n = 0;
    uint8_t *buf = read_whole_file(path, &n);
    if (!buf) { set_status(app, "could not read %s", path); return; }
    free(app->cart_buf);
    app->cart_buf = buf;
    app->cart_buf_len = n;
    p2500_cartridge_init(&app->m.cartridge, buf, n);
    p2500_history_clear(app->hist);
    const char *base = SDL_strrchr(path, '/');
    SDL_strlcpy(app->cart_name, base ? base + 1 : path, sizeof app->cart_name);
    app->cart_inserted = true;
    set_status(app, "cartridge: %s", app->cart_name);
}

/* The $01 presence byte is fixed by the confirmed protocol - not something
 * a real key's owner chooses - so only the two serial bytes are editable. */
static void insert_manual_key(App *app)
{
    uint8_t *buf = (uint8_t *)malloc(3);
    if (!buf) return;
    buf[0] = 0x01;
    buf[1] = app->cart_edit_hi;
    buf[2] = app->cart_edit_lo;
    free(app->cart_buf);
    app->cart_buf = buf;
    app->cart_buf_len = 3;
    p2500_cartridge_init(&app->m.cartridge, buf, 3);
    app->cart_name[0] = '\0';
    app->cart_inserted = true;
    set_status(app, "cartridge: ID 0x%02X%02X", app->cart_edit_hi, app->cart_edit_lo);
}

/* Ejecting deliberately keeps cart_buf, so the right-click toggle (and the
 * Cartridge window's own Eject/re-insert) can restore the same key or ROM
 * without the user retyping or reloading it. */
static void eject_cartridge(App *app)
{
    if (!app->cart_inserted) return;
    app->cart_inserted = false;
    p2500_cartridge_init(&app->m.cartridge, NULL, 0);
    set_status(app, "cartridge: empty");
    SDL_Log("cartridge: ejected");
}

static void toggle_cartridge(App *app)
{
    if (app->cart_inserted) { eject_cartridge(app); return; }
    if (!app->cart_buf) return;
    /* Re-running init also resets the internal read position, matching the
     * real protocol's own "freshly inserted" state. */
    p2500_cartridge_init(&app->m.cartridge, app->cart_buf, app->cart_buf_len);
    app->cart_inserted = true;
    if (app->cart_name[0]) set_status(app, "cartridge: %s", app->cart_name);
    else set_status(app, "cartridge: ID 0x%02X%02X", app->cart_buf[1], app->cart_buf[2]);
}

/* Where a file dialog should start looking.
 *
 * Both the development tree and a published release keep disks/ and roms/
 * beside the executable (publish.sh copies them), so "next to the binary" is
 * the useful default, with the working directory as a fallback for an
 * unusual layout. Returning NULL means "let the platform decide", which is
 * what SDL does when given no default location - the right answer when
 * neither directory is there, rather than opening somewhere arbitrary.
 *
 * The caller owns the buffer on purpose. A static one looks tidier and is a
 * trap: two calls in one expression alias, and C does not define which runs
 * first, so `f(dir("roms"), dir("disks"))` silently passes the same path
 * twice. That is not hypothetical - it happened while testing this. */
static const char *resource_dir(const char *name, char *buf, size_t n)
{
    SDL_PathInfo info;
    const char *base = SDL_GetBasePath();
    if (base) {
        SDL_snprintf(buf, n, "%s%s", base, name);
        if (SDL_GetPathInfo(buf, &info) && info.type == SDL_PATHTYPE_DIRECTORY)
            return buf;
    }
    SDL_snprintf(buf, n, "%s", name);
    if (SDL_GetPathInfo(buf, &info) && info.type == SDL_PATHTYPE_DIRECTORY)
        return buf;
    return NULL;
}

static void open_cartridge_dialog(App *app)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Cartridge images", "rom;bin" },
        { "All files", "*" },
    };
    char dir[1024];
    SDL_ShowOpenFileDialog(cartridge_chosen, app, app->window,
                           filters, SDL_arraysize(filters),
                           resource_dir("roms", dir, sizeof dir), false);
}

/* One place, because three things reach it: the File menu, F12, and the
 * run/pause lamp. Resuming clears any breakpoint stop, or Run would trip
 * straight back over the address it is standing on. */
/* Load every --symbols file into the debug table, in the order given.
 * Called once at startup and again after each reset, which clears the
 * table: the CP/M names are the machine's to forget, the user's are not. */
/* Try the shipped symbol files against what is actually in memory. Costs
 * a CRC over a kilobyte per candidate, so it runs a few times a second
 * rather than per frame, and stops once something matches. */
static void try_auto_symbols(App *app)
{
    if (!app->symbols_auto) return;
    /* What fits the machine as it stands - which changes as it boots: the
     * IPL's own names are right at reset and wrong once CBIOS has banked
     * the EPROM away and built its table over the IPL's RAM block. */
    char chosen[256] = {0};
    p2500_debug_auto_symbols(NULL, &app->m, "symbols", chosen, sizeof chosen);
    if (!chosen[0] || !strcmp(chosen, app->symbols_auto_from)) return;
    if (app->symbols_auto_from[0]) p2500_debug_clear_symbols(&app->dbg);
    int added = p2500_debug_auto_symbols(&app->dbg, &app->m, "symbols",
                                         chosen, sizeof chosen);
    if (added <= 0) return;
    SDL_strlcpy(app->symbols_auto_from, chosen, sizeof app->symbols_auto_from);
    p2500_panels_log(app->panels, P2500_LOG_INFO, "symbols",
                     "%d name(s) from %s - this build's own bytes match it",
                     added, chosen);
    /* Also to the terminal: a headless run is how the test suite sees it. */
    SDL_Log("symbols: %d name(s) from %s", added, chosen);
    set_status(app, "symbols: %s", chosen);
}

static int load_symbol_files(App *app)
{
    int total = 0;
    for (int i = 0; i < app->symfiles; i++) {
        char err[256] = {0};
        int trunc = 0;
        int n = p2500_debug_load_symbols(&app->dbg, app->symfile[i], &trunc,
                                         err, sizeof err);
        if (n < 0) { SDL_Log("%s", err); continue; }
        SDL_Log("loaded %d symbol(s) from %s%s", n, app->symfile[i],
                trunc ? " (some names truncated)" : "");
        total += n;
    }
    return total;
}

static void toggle_pause(App *app)
{
    app->paused = !app->paused;
    if (!app->paused) p2500_debug_resume(&app->dbg);
    set_status(app, app->paused ? "paused" : "running");
    SDL_Log("run state: %s", app->paused ? "paused" : "running");
}

/*
 * Speed.
 *
 * Halves and doubles rather than a slider: the useful speeds are a handful
 * of ratios you want to name and come back to ("run it at half speed and
 * watch the bounce"), and a slider invites hunting for a value that reads
 * 1.00x when 1x is the one setting that must be exact.
 *
 * 0 is unlimited and sorts last.
 */
static const float SPEED_STEPS[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 0.0f };

/* %g rather than %.2f so the labels come out 0.25x, 0.5x, 1x - no trailing
 * zeroes to read past, and 1x stays two characters wide. */
static void speed_label(float s, char *out, size_t n)
{
    if (s == 0.0f) SDL_strlcpy(out, "unlimited", n);
    else SDL_snprintf(out, n, "%gx", (double)s);
}

static void set_speed(App *app, float s)
{
    app->speed = s;
    app->tstate_carry = 0.0;
    app->next_field_ns = 0;       /* restart the pacing clock */
    app->speed_ns = 0;            /* and the measurement window */
    app->speed_measured = 0.0f;
    char label[16];
    speed_label(s, label, sizeof label);
    set_status(app, "speed: %s", label);
    SDL_Log("speed: %s", label);
}

/*
 * Measure what emulated time is really doing against the wall clock.
 *
 * Taken from the CPU's own T-state count rather than from a count of
 * iterations, so it does not care how the budget was split up, and it stays
 * right when a breakpoint cuts an iteration short. The window is a quarter
 * second: long enough that one slow frame does not swing the reading,
 * short enough to respond while you watch it.
 */
static void measure_speed(App *app)
{
    const Uint64 now = SDL_GetTicksNS();
    if (app->speed_ns == 0) {
        app->speed_ns = now;
        app->speed_cyc = app->m.cpu.cyc;
        return;
    }
    const Uint64 dt = now - app->speed_ns;
    if (dt < 250000000ull) return;
    const double emulated = (double)(app->m.cpu.cyc - app->speed_cyc) / (double)P2500_CPU_HZ;
    app->speed_measured = (float)(emulated / ((double)dt / 1e9));
    app->speed_ns = now;
    app->speed_cyc = app->m.cpu.cyc;
}

/*
 * What the status cell shows.
 *
 * The set value, normally: it is the value the user chose and it should read
 * back unchanged. The measured value when that is the only honest answer -
 * under "unlimited" there is no set value, and when the host cannot keep up
 * the set value is a claim the machine is not meeting, so both are shown.
 */
static void speed_reading(const App *app, char *out, size_t n)
{
    if (app->speed == 0.0f) {
        if (app->paused || app->speed_measured <= 0.0f) SDL_strlcpy(out, "max", n);
        else SDL_snprintf(out, n, "~%.1fx", (double)app->speed_measured);
    } else if (app->paused || app->speed_measured <= 0.0f) {
        SDL_snprintf(out, n, "%gx", (double)app->speed);
    } else if (app->speed_measured < app->speed * 0.85f) {
        SDL_snprintf(out, n, "%gx (%.1f)", (double)app->speed, (double)app->speed_measured);
    } else {
        SDL_snprintf(out, n, "%gx", (double)app->speed);
    }
}

/* Shared by the Machine menu and the status cell's right-click, so the two
 * lists cannot drift apart. */
static void speed_menu_items(App *app)
{
    char label[16];
    for (size_t i = 0; i < SDL_arraysize(SPEED_STEPS); i++) {
        const float s = SPEED_STEPS[i];
        if (s == 0.0f) ImGui::Separator();
        speed_label(s, label, sizeof label);
        if (ImGui::MenuItem(label, s == 0.0f ? "F11" : NULL, app->speed == s))
            set_speed(app, s);
        if (i < SDL_arraysize(app->speed_item_y)) {
            const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            app->speed_item_x = (mn.x + mx.x) * 0.5f;
            app->speed_item_y[i] = (mn.y + mx.y) * 0.5f;
        }
    }
}

/* What the cartridge cell/window shows. Filenames are truncated to a fixed
 * width so the reserved toolbar cell - and everything left of it - never
 * moves; the full name is always in the tooltip instead. */
#define CART_NAME_DISPLAY_MAX 14
static void cartridge_reading(const App *app, char *out, size_t n)
{
    if (!app->cart_inserted) { SDL_strlcpy(out, "empty", n); return; }
    if (app->cart_name[0]) {
        if (strlen(app->cart_name) > CART_NAME_DISPLAY_MAX)
            SDL_snprintf(out, n, "%.*s...", CART_NAME_DISPLAY_MAX - 3, app->cart_name);
        else
            SDL_strlcpy(out, app->cart_name, n);
        return;
    }
    const uint8_t *s = app->m.cartridge.stream;
    if (s && app->m.cartridge.stream_len >= 3)
        SDL_snprintf(out, n, "ID 0x%02X%02X", s[1], s[2]);
    else
        SDL_strlcpy(out, "ID", n);
}

/* CBIOS supports A:, B: and C:, so the dialog is opened per
 * drive and the chosen unit travels with the pending path. */
static void open_disk_dialog(App *app, unsigned unit)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Disk images", "raw;img;dsk;imd" },
        { "All files", "*" },
    };
    app->pending_unit = unit;
    char dir[1024];
    SDL_ShowOpenFileDialog(disk_chosen, app, app->window,
                           filters, SDL_arraysize(filters),
                           resource_dir("disks", dir, sizeof dir), false);
}

/* Another thread, same discipline as disk_chosen(): park the path, raise a
 * flag, and let SDL_AppIterate do the file I/O. */
static void SDLCALL save_chosen(void *userdata, const char *const *filelist, int)
{
    App *app = (App *)userdata;
    if (!filelist || !filelist[0]) return; /* cancelled, or the dialog failed */
    SDL_strlcpy(app->save_path, filelist[0], sizeof app->save_path);
    SDL_SetAtomicInt(&app->save_pending, 1);
}

/* Writing a mounted image out is always "save as": the machine never
 * writes back over the file a disk came from, so there is no in-place save
 * to offer and no way for a guest program to damage an original. */
static void open_save_dialog(App *app, unsigned unit)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Disk images", "raw;img;dsk" },
        { "All files", "*" },
    };
    if (unit >= P2500_FDC_MAX_DRIVES || !app->disk[unit]) return;
    app->saving_unit = unit;
    SDL_ShowSaveFileDialog(save_chosen, app, app->window,
                           filters, SDL_arraysize(filters), app->disk_name[unit]);
}

static void save_disk(App *app, unsigned unit, const char *path)
{
    if (unit >= P2500_FDC_MAX_DRIVES || !app->disk[unit]) return;
    const size_t n = app->m.fdc.disk_size[unit];
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io || SDL_WriteIO(io, app->disk[unit], n) != n) {
        if (io) SDL_CloseIO(io);
        set_status(app, "could not write %s: %s", path, SDL_GetError());
        return;
    }
    SDL_CloseIO(io);
    const char *base = SDL_strrchr(path, '/');
    set_status(app, "%c: saved to %s (%lu sector(s) written)", 'A' + (int)unit,
               base ? base + 1 : path, app->m.fdc.sectors_written[unit]);
    SDL_Log("drive %c: %llu bytes written to %s", 'A' + (int)unit, (unsigned long long)n, path);
}

/* The interface theme: the chrome around the emulated screen, which is not
 * an emulated artifact and so is free to be whatever reads best.
 *
 * Two explicit tables rather than one palette with colours derived from it.
 * These values were hand-tuned, not computed, and pretending otherwise would
 * mean a formula that moves the shipped look to fit itself - DARK below is a
 * verbatim transcription of what the front-end has always drawn.
 *
 * PAPER exists for the same reason the white phosphor does: dark-on-light is
 * what some people can actually read. It is deliberately a *separate* setting
 * from the screen's phosphor, so either can be changed without the other.
 * The enum itself is in prefs.h - see the note by ASPECT_43. */

struct ThemeColour { ImGuiCol_ idx; ImVec4 c; };

/* Green on near-black, so the bar reads as part of the machine's bezel
 * rather than as a debug overlay. */
static const ThemeColour THEME_DARK_COLOURS[] = {
    { ImGuiCol_Text, ImVec4(0.27f, 1.00f, 0.00f, 1.00f) },
    { ImGuiCol_TextDisabled, ImVec4(0.16f, 0.55f, 0.00f, 1.00f) },
    { ImGuiCol_WindowBg, ImVec4(0.031f, 0.047f, 0.031f, 1.00f) },
    { ImGuiCol_ChildBg, ImVec4(0.02f, 0.031f, 0.02f, 1.00f) },
    { ImGuiCol_PopupBg, ImVec4(0.031f, 0.047f, 0.031f, 1.00f) },
    { ImGuiCol_MenuBarBg, ImVec4(0.055f, 0.086f, 0.055f, 1.00f) },
    { ImGuiCol_Border, ImVec4(0.12f, 0.35f, 0.08f, 1.00f) },
    { ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f) },
    { ImGuiCol_Header, ImVec4(0.08f, 0.22f, 0.06f, 1.00f) },
    { ImGuiCol_HeaderHovered, ImVec4(0.13f, 0.34f, 0.09f, 1.00f) },
    { ImGuiCol_HeaderActive, ImVec4(0.18f, 0.46f, 0.12f, 1.00f) },
    { ImGuiCol_FrameBg, ImVec4(0.08f, 0.16f, 0.08f, 1.00f) },
    { ImGuiCol_FrameBgHovered, ImVec4(0.12f, 0.28f, 0.10f, 1.00f) },
    { ImGuiCol_FrameBgActive, ImVec4(0.16f, 0.40f, 0.12f, 1.00f) },
    { ImGuiCol_Separator, ImVec4(0.12f, 0.35f, 0.08f, 1.00f) },
    { ImGuiCol_SeparatorHovered, ImVec4(0.20f, 0.60f, 0.14f, 1.00f) },
    { ImGuiCol_SeparatorActive, ImVec4(0.27f, 1.00f, 0.00f, 1.00f) },
    { ImGuiCol_CheckMark, ImVec4(0.27f, 1.00f, 0.00f, 1.00f) },
    { ImGuiCol_TitleBg, ImVec4(0.043f, 0.071f, 0.043f, 1.00f) },
    { ImGuiCol_TitleBgActive, ImVec4(0.075f, 0.16f, 0.06f, 1.00f) },
    { ImGuiCol_TitleBgCollapsed, ImVec4(0.043f, 0.071f, 0.043f, 0.85f) },
    { ImGuiCol_Button, ImVec4(0.09f, 0.22f, 0.07f, 1.00f) },
    { ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.36f, 0.10f, 1.00f) },
    { ImGuiCol_ButtonActive, ImVec4(0.20f, 0.52f, 0.14f, 1.00f) },
    { ImGuiCol_ScrollbarBg, ImVec4(0.02f, 0.031f, 0.02f, 1.00f) },
    { ImGuiCol_ScrollbarGrab, ImVec4(0.10f, 0.26f, 0.08f, 1.00f) },
    { ImGuiCol_ScrollbarGrabHovered, ImVec4(0.16f, 0.40f, 0.12f, 1.00f) },
    { ImGuiCol_ScrollbarGrabActive, ImVec4(0.22f, 0.58f, 0.16f, 1.00f) },
    { ImGuiCol_SliderGrab, ImVec4(0.16f, 0.55f, 0.00f, 1.00f) },
    { ImGuiCol_SliderGrabActive, ImVec4(0.27f, 1.00f, 0.00f, 1.00f) },
    { ImGuiCol_ResizeGrip, ImVec4(0.12f, 0.35f, 0.08f, 0.60f) },
    { ImGuiCol_ResizeGripHovered, ImVec4(0.20f, 0.60f, 0.14f, 0.80f) },
    { ImGuiCol_ResizeGripActive, ImVec4(0.27f, 1.00f, 0.00f, 1.00f) },
    { ImGuiCol_Tab, ImVec4(0.06f, 0.13f, 0.05f, 1.00f) },
    { ImGuiCol_TabHovered, ImVec4(0.16f, 0.40f, 0.12f, 1.00f) },
    { ImGuiCol_TableHeaderBg, ImVec4(0.06f, 0.13f, 0.05f, 1.00f) },
    { ImGuiCol_TableBorderStrong, ImVec4(0.12f, 0.35f, 0.08f, 1.00f) },
    { ImGuiCol_TableBorderLight, ImVec4(0.08f, 0.20f, 0.06f, 1.00f) },
    { ImGuiCol_TableRowBg, ImVec4(0.00f, 0.00f, 0.00f, 0.00f) },
    { ImGuiCol_TableRowBgAlt, ImVec4(0.27f, 1.00f, 0.00f, 0.035f) },
    { ImGuiCol_NavCursor, ImVec4(0.27f, 1.00f, 0.00f, 1.00f) },
    { ImGuiCol_DragDropTarget, ImVec4(0.27f, 1.00f, 0.00f, 1.00f) },
};

/* Ink on paper. The accent stays a green, which keeps the checkmarks and
 * the active slider tied to the machine without lighting the whole UI up. */
static const ThemeColour THEME_PAPER_COLOURS[] = {
    { ImGuiCol_Text, ImVec4(0.11f, 0.12f, 0.11f, 1.00f) },
    /* Dark enough to read. At 0.45 this sat about 3.6:1 against the menu
     * bar, which is under the 4.5:1 normal-text threshold - and the bar is
     * exactly where it is used, for the cartridge and the unlit lamps. */
    { ImGuiCol_TextDisabled, ImVec4(0.32f, 0.34f, 0.32f, 1.00f) },
    { ImGuiCol_WindowBg, ImVec4(0.94f, 0.94f, 0.92f, 1.00f) },
    { ImGuiCol_ChildBg, ImVec4(0.90f, 0.90f, 0.88f, 1.00f) },
    { ImGuiCol_PopupBg, ImVec4(0.97f, 0.97f, 0.95f, 1.00f) },
    { ImGuiCol_MenuBarBg, ImVec4(0.88f, 0.88f, 0.86f, 1.00f) },
    { ImGuiCol_Border, ImVec4(0.70f, 0.72f, 0.68f, 1.00f) },
    { ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.00f) },
    { ImGuiCol_Header, ImVec4(0.80f, 0.84f, 0.78f, 1.00f) },
    { ImGuiCol_HeaderHovered, ImVec4(0.72f, 0.78f, 0.70f, 1.00f) },
    { ImGuiCol_HeaderActive, ImVec4(0.64f, 0.72f, 0.62f, 1.00f) },
    { ImGuiCol_FrameBg, ImVec4(0.88f, 0.89f, 0.86f, 1.00f) },
    { ImGuiCol_FrameBgHovered, ImVec4(0.82f, 0.84f, 0.80f, 1.00f) },
    { ImGuiCol_FrameBgActive, ImVec4(0.76f, 0.79f, 0.74f, 1.00f) },
    { ImGuiCol_Separator, ImVec4(0.70f, 0.72f, 0.68f, 1.00f) },
    { ImGuiCol_SeparatorHovered, ImVec4(0.55f, 0.60f, 0.52f, 1.00f) },
    { ImGuiCol_SeparatorActive, ImVec4(0.18f, 0.42f, 0.12f, 1.00f) },
    { ImGuiCol_CheckMark, ImVec4(0.18f, 0.42f, 0.12f, 1.00f) },
    { ImGuiCol_TitleBg, ImVec4(0.86f, 0.87f, 0.84f, 1.00f) },
    { ImGuiCol_TitleBgActive, ImVec4(0.78f, 0.82f, 0.76f, 1.00f) },
    { ImGuiCol_TitleBgCollapsed, ImVec4(0.86f, 0.87f, 0.84f, 0.85f) },
    { ImGuiCol_Button, ImVec4(0.85f, 0.87f, 0.83f, 1.00f) },
    { ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.82f, 0.76f, 1.00f) },
    { ImGuiCol_ButtonActive, ImVec4(0.70f, 0.76f, 0.68f, 1.00f) },
    { ImGuiCol_ScrollbarBg, ImVec4(0.90f, 0.90f, 0.88f, 1.00f) },
    { ImGuiCol_ScrollbarGrab, ImVec4(0.74f, 0.76f, 0.72f, 1.00f) },
    { ImGuiCol_ScrollbarGrabHovered, ImVec4(0.65f, 0.68f, 0.63f, 1.00f) },
    { ImGuiCol_ScrollbarGrabActive, ImVec4(0.55f, 0.60f, 0.53f, 1.00f) },
    { ImGuiCol_SliderGrab, ImVec4(0.55f, 0.60f, 0.52f, 1.00f) },
    { ImGuiCol_SliderGrabActive, ImVec4(0.18f, 0.42f, 0.12f, 1.00f) },
    { ImGuiCol_ResizeGrip, ImVec4(0.70f, 0.72f, 0.68f, 0.60f) },
    { ImGuiCol_ResizeGripHovered, ImVec4(0.55f, 0.60f, 0.52f, 0.80f) },
    { ImGuiCol_ResizeGripActive, ImVec4(0.18f, 0.42f, 0.12f, 1.00f) },
    { ImGuiCol_Tab, ImVec4(0.86f, 0.87f, 0.84f, 1.00f) },
    { ImGuiCol_TabHovered, ImVec4(0.76f, 0.80f, 0.74f, 1.00f) },
    { ImGuiCol_TableHeaderBg, ImVec4(0.86f, 0.87f, 0.84f, 1.00f) },
    { ImGuiCol_TableBorderStrong, ImVec4(0.70f, 0.72f, 0.68f, 1.00f) },
    { ImGuiCol_TableBorderLight, ImVec4(0.82f, 0.83f, 0.80f, 1.00f) },
    { ImGuiCol_TableRowBg, ImVec4(0.00f, 0.00f, 0.00f, 0.00f) },
    { ImGuiCol_TableRowBgAlt, ImVec4(0.18f, 0.42f, 0.12f, 0.035f) },
    { ImGuiCol_NavCursor, ImVec4(0.11f, 0.12f, 0.11f, 1.00f) },
    { ImGuiCol_DragDropTarget, ImVec4(0.18f, 0.42f, 0.12f, 1.00f) },
};

/* What the window is cleared to around the emulated screen.
 *
 * Dark in every theme, deliberately. It was briefly tied to the interface
 * theme, which made the surround light in Paper - and that reads wrong:
 * this area is the dead space around a picture tube, not part of the
 * chrome, so it belongs to the machine however the UI is themed.
 *
 * It must never be the screen's own background, or the viewport has no
 * visible edge and no capture can say where the screen was drawn. Kept
 * clear of every phosphor's background for that reason. */
static void letterbox_rgb(int theme, uint8_t out[3])
{
    (void)theme;
    out[0] = 4; out[1] = 4; out[2] = 4;
}

/* Square corners and roomy padding in both themes: the metrics are not what
 * the theme is about, and keeping them shared stops the two drifting into
 * different layouts - which would move every geometry the suite asserts. */
static void apply_style(int theme)
{
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = st.ChildRounding = st.FrameRounding = 0.0f;
    st.PopupRounding = st.ScrollbarRounding = st.GrabRounding = st.TabRounding = 0.0f;
    st.WindowBorderSize = st.PopupBorderSize = 1.0f;
    st.FrameBorderSize = 0.0f;
    st.FramePadding = ImVec2(10.0f, 5.0f);
    st.ItemSpacing = ImVec2(12.0f, 6.0f);
    st.WindowPadding = ImVec2(8.0f, 8.0f);

    const ThemeColour *tbl = theme == THEME_PAPER ? THEME_PAPER_COLOURS
                                                  : THEME_DARK_COLOURS;
    const size_t n = theme == THEME_PAPER ? SDL_arraysize(THEME_PAPER_COLOURS)
                                          : SDL_arraysize(THEME_DARK_COLOURS);
    /* Every slot the other theme sets, both themes set - otherwise switching
     * leaves one colour behind from the theme before it. */
    for (size_t i = 0; i < n; i++) st.Colors[tbl[i].idx] = tbl[i].c;
}

/*
 * The P2500's own capitals-lock keycap symbol, traced off the P2219
 * manual's KEYBOARD page: a U joined to an inverted U - one period of a
 * square-cornered wave, i.e. an S laid on its side.
 *
 * Drawn rather than typed because there is no Unicode character for it (the
 * nearest, U+223F SINE WAVE, means something else), and because ImGui's
 * default font only bakes U+0020-U+00FF, so even U+21EA would not render
 * without shipping a TTF. Five strokes is cheaper than a font.
 */
static void draw_caps_glyph(ImDrawList *dl, ImVec2 centre, float h, ImU32 col)
{
    const float w = h * 0.78f;
    const float t = h * 0.13f > 1.4f ? h * 0.13f : 1.4f;
    const float x0 = centre.x - w * 0.5f, x1 = centre.x, x2 = centre.x + w * 0.5f;
    const float y0 = centre.y - h * 0.5f, y1 = centre.y + h * 0.5f;
    ImVec2 pts[6] = { ImVec2(x0, y0), ImVec2(x0, y1), ImVec2(x1, y1),
                      ImVec2(x1, y0), ImVec2(x2, y0), ImVec2(x2, y1) };
    dl->AddPolyline(pts, 6, col, 0, t);
}

/*
 * Run state. Both states are real, so both are legible; brightness marks the
 * one worth noticing. Paused is the state you can forget you are in - a
 * machine that has quietly stopped looks exactly like one that has hung - so
 * the bars are bright and the running triangle is not.
 */
static void draw_run_glyph(ImDrawList *dl, ImVec2 centre, float h, bool paused, ImU32 col)
{
    const float w = h * 0.72f;
    if (paused) {
        const float bw = w * 0.32f;
        dl->AddRectFilled(ImVec2(centre.x - w * 0.5f, centre.y - h * 0.5f),
                          ImVec2(centre.x - w * 0.5f + bw, centre.y + h * 0.5f), col);
        dl->AddRectFilled(ImVec2(centre.x + w * 0.5f - bw, centre.y - h * 0.5f),
                          ImVec2(centre.x + w * 0.5f, centre.y + h * 0.5f), col);
    } else {
        dl->AddTriangleFilled(ImVec2(centre.x - w * 0.42f, centre.y - h * 0.5f),
                              ImVec2(centre.x + w * 0.58f, centre.y),
                              ImVec2(centre.x - w * 0.42f, centre.y + h * 0.5f), col);
    }
}

/* Indicators have three brightnesses, not two, because a drive lamp has
 * three states to say and only one of them is "off":
 *
 *   LAMP_OFF       nothing attached.
 *   LAMP_INERT     attached, but the running BIOS does not map that drive
 *                  letter - the guest will answer `Bdos Err On D: Select`.
 *                  Mid-bright: the medium is real, the drive is not.
 *   LAMP_ON        attached and mapped.
 *
 * Every one is DRAWN rather than hidden, so nothing in the bar ever moves
 * and each position always means the same thing. */
typedef enum { LAMP_OFF = 0, LAMP_INERT, LAMP_ON } LampLevel;

static ImU32 indicator_colour(LampLevel level)
{
    if (level == LAMP_ON) return ImGui::GetColorU32(ImGuiCol_Text);
    ImVec4 off = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    off.w = (level == LAMP_INERT) ? 1.0f : 0.45f;
    return ImGui::GetColorU32(off);
}

/*
 * A lamp is also a button.
 *
 * These are not ImGui widgets: they are drawn into the menu bar's draw list
 * at computed positions, because the bar's layout has to stay fixed as the
 * status text changes. So the click target is an InvisibleButton placed over
 * the same rectangle, and the "pressed" look is painted by hand.
 *
 * Hovering inverts the cell - bright ground, dark glyph - which is both the
 * clearest way to say "this is a control" and the one that costs no extra
 * room in a 23-pixel bar. The ground keeps the lamp's own brightness, so the
 * lit/unlit state stays readable while the pointer is over it.
 */
struct Lamp {
    ImVec2 min, max;
    bool hovered;
    bool clicked;        /* left button */
    bool alt_clicked;    /* right button */
};

static Lamp lamp_button(const char *id, float centre_x, float w,
                        float bar_y, float bar_h)
{
    const float padx = 3.0f;
    Lamp l;
    l.min = ImVec2(centre_x - w * 0.5f - padx, bar_y + 2.0f);
    l.max = ImVec2(centre_x + w * 0.5f + padx, bar_y + bar_h - 2.0f);
    ImGui::SetCursorScreenPos(l.min);
    l.clicked = ImGui::InvisibleButton(id, ImVec2(l.max.x - l.min.x, l.max.y - l.min.y),
                                       ImGuiButtonFlags_MouseButtonLeft |
                                       ImGuiButtonFlags_MouseButtonRight);
    l.hovered = ImGui::IsItemHovered();
    l.alt_clicked = l.clicked && ImGui::IsMouseReleased(ImGuiMouseButton_Right);
    if (l.alt_clicked) l.clicked = false;
    return l;
}

/* Paints the inverted ground and returns the colour the glyph must be drawn
 * in: the bar's own background when inverted, the lamp's normal ink when
 * not. */
static ImU32 lamp_paint(ImDrawList *dl, const Lamp &l, LampLevel level)
{
    if (!l.hovered) return indicator_colour(level);
    ImVec4 ground = ImGui::GetStyleColorVec4(level == LAMP_ON ? ImGuiCol_Text
                                                              : ImGuiCol_TextDisabled);
    dl->AddRectFilled(l.min, l.max, ImGui::GetColorU32(ground));
    return ImGui::GetColorU32(ImGuiCol_MenuBarBg);
}

/* A plain non-modal window, toggled by app->show_cartridge like a debugger
 * panel's own bool - opened from the Machine menu or by clicking the
 * toolbar lamp. */
static void draw_cartridge_window(App *app)
{
    if (!app->show_cartridge) return;
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Cartridge", &app->show_cartridge)) { ImGui::End(); return; }

    char reading[32];
    cartridge_reading(app, reading, sizeof reading);
    ImGui::Text("%s: %s", "Cartridge", reading);
    
    ImGui::Separator();
    ImGui::TextUnformatted("SESAM key");
    ImGui::TextDisabled("Insert serial number (byte 2 and 3):");
    int hi = app->cart_edit_hi, lo = app->cart_edit_lo;
    ImGui::PushItemWidth(60);
    if (ImGui::InputScalar("Serial hi", ImGuiDataType_U8, &hi, NULL, NULL, "%02X",
                           ImGuiInputTextFlags_CharsHexadecimal))
        app->cart_edit_hi = (uint8_t)hi;
    ImGui::SameLine();
    if (ImGui::InputScalar("Serial lo", ImGuiDataType_U8, &lo, NULL, NULL, "%02X",
                           ImGuiInputTextFlags_CharsHexadecimal))
        app->cart_edit_lo = (uint8_t)lo;
    ImGui::PopItemWidth();
    if (ImGui::Button("Insert Key")) insert_manual_key(app);

    ImGui::Separator();
    ImGui::TextUnformatted("Cartridge ROM");
    if (ImGui::Button("Load...")) open_cartridge_dialog(app);

    ImGui::Separator();
    ImGui::BeginDisabled(!app->cart_inserted);
    if (ImGui::Button("Eject")) eject_cartridge(app);
    ImGui::EndDisabled();

    ImGui::End();
}

/* About dialog (in Help menu). */
static void draw_about_window(App *app)
{
    if (!app->show_about) return;
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("About", &app->show_about, ImGuiWindowFlags_NoResize)) { ImGui::End(); return; }

    ImGui::TextUnformatted("Philips P2000B-P2500 emulator");
    ImGui::Text("Version %s", P2500_VERSION_STR);
    ImGui::Spacing();

    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    ImGui::Text("(c) NoxiousPluK, %d", 1900 + tm_now->tm_year);

    ImGui::Spacing();
    static const char url[] = "https://noxiouspl.uk/";
    ImVec2 url_pos = ImGui::GetCursorScreenPos();
    ImVec2 url_size = ImGui::CalcTextSize(url);
    bool url_hovered = ImGui::IsMouseHoveringRect(url_pos, ImVec2(url_pos.x + url_size.x, url_pos.y + url_size.y));
    ImGui::PushStyleColor(ImGuiCol_Text, url_hovered ? IM_COL32_WHITE : ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::TextUnformatted(url);
    ImGui::PopStyleColor();
    if (url_hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemClicked()) SDL_OpenURL(url);

    ImGui::Spacing();
    if (ImGui::Button("Close")) app->show_about = false;

    ImGui::End();
}

static void draw_menu_bar(App *app)
{
    if (!ImGui::BeginMainMenuBar()) return;
    app->menu_h = ImGui::GetWindowSize().y;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Load Disk A...", "Ctrl+O")) open_disk_dialog(app, 0);
        if (ImGui::MenuItem("Load Disk B...")) open_disk_dialog(app, 1);
        if (ImGui::MenuItem("Load Disk C...")) open_disk_dialog(app, 2);
        if (ImGui::MenuItem("Load Disk D...")) open_disk_dialog(app, 3);
        ImGui::Separator();
        /* Writes go into the mounted image's buffer and nowhere else, so
         * keeping them is this menu's job. The shortcut column carries the
         * written-sector count rather than a key, which is the one thing a
         * user needs in order to decide whether to bother - and makes a
         * drive the guest has written to visible without a tooltip. */
        if (ImGui::BeginMenu("Save Disk As...")) {
            for (unsigned u = 0; u < P2500_FDC_MAX_DRIVES; u++) {
                char label[32], count[32] = "";
                SDL_snprintf(label, sizeof label, "Disk %c...", 'A' + (int)u);
                const unsigned long w = app->m.fdc.sectors_written[u];
                if (w) SDL_snprintf(count, sizeof count, "%lu sector%s", w, w == 1 ? "" : "s");
                else if (app->write_protect & (1u << u)) SDL_strlcpy(count, "write-protected", sizeof count);
                if (ImGui::MenuItem(label, count, false, app->disk[u] != NULL))
                    open_save_dialog(app, u);
            }
            ImGui::EndMenu();
        }
        /* The physical write-protect tab, per drive. Toggling it re-attaches
         * the same buffer through the other entry point, so it takes effect
         * on the next command the guest issues - as moving a real tab would,
         * the guest having no way to be told in advance. */
        if (ImGui::BeginMenu("Write-protect")) {
            for (unsigned u = 0; u < P2500_FDC_MAX_DRIVES; u++) {
                char label[32];
                SDL_snprintf(label, sizeof label, "Disk %c", 'A' + (int)u);
                bool on = (app->write_protect & (1u << u)) != 0;
                if (ImGui::MenuItem(label, NULL, on, app->disk[u] != NULL)) {
                    on = !on;
                    app->write_protect = on ? (app->write_protect | (1u << u))
                                            : (app->write_protect & ~(1u << u));
                    const size_t n = app->m.fdc.disk_size[u];
                    if (on) p2500_fdc_attach(&app->m.fdc, u, app->disk[u], n);
                    else p2500_fdc_attach_writable(&app->m.fdc, u, app->disk[u], n);
                    set_status(app, "%c: %s", 'A' + (int)u,
                               on ? "write-protected" : "writable");
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset", "Ctrl+R")) {
            p2500_reset(&app->m);
            p2500_history_clear(app->hist);
            p2500_debug_baseline(&app->dbg, &app->m);
            /* The names belonged to the CP/M that was running: a different
             * disk links its CBIOS somewhere else, so they have to be read
             * again rather than carried across. */
            p2500_debug_clear_symbols(&app->dbg);
            app->symbols_auto_from[0] = '\0';
            load_symbol_files(app);
            app->dbg.one_shot_armed = false;
            set_status(app, "reset - media still attached");
        }
        if (ImGui::MenuItem(app->paused ? "Unpause" : "Pause", "F12")) toggle_pause(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Screenshot", "F10")) save_screenshot(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) app->quit = true;
        ImGui::EndMenu();
    }

    /* After File, deliberately: File is the one menu whose x coordinate the
     * test suite hard-codes (it clicks it at x=20 to reach Save Disk As...),
     * and keeping it first leaves that valid. */
    if (ImGui::BeginMenu("View")) {
        /* Two groups, because they answer different questions: how the
         * machine's picture is shaped, and what the chrome around it looks
         * like. The screen's phosphor sits with the first - it is the
         * machine's own colour - and the interface theme with the second. */
        ImGui::SeparatorText("Screen");
        if (ImGui::MenuItem("4:3 display", NULL, app->aspect == ASPECT_43)) {
            app->aspect = ASPECT_43;
            set_status(app, "4:3 display (as the machine's monitor)");
        }
        if (ImGui::MenuItem("Square pixels", NULL, app->aspect == ASPECT_SQUARE)) {
            app->aspect = ASPECT_SQUARE;
            set_status(app, "square pixels (1:1 dots)");
        }
        if (ImGui::MenuItem("Snap to whole pixels", NULL, app->snap)) {
            app->snap = !app->snap;
            set_status(app, "whole-pixel snapping %s", app->snap ? "on" : "off");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Every dot the same size, at the cost of an exact aspect");

        ImGui::SeparatorText("Phosphor");
        for (int i = 0; i < P2500_PHOSPHOR_COUNT; i++) {
            const P2500PhosphorId id = (P2500PhosphorId)i;
            /* Green is labelled as the machine's own, so nothing learned off
             * a different one gets mistaken for a fact about the hardware. */
            char label[32];
            SDL_snprintf(label, sizeof label, "%s%s", p2500_phosphor_name(id),
                         id == P2500_PHOSPHOR_GREEN ? " (as shipped)" : "");
            label[0] = (char)SDL_toupper(label[0]);
            if (ImGui::MenuItem(label, NULL, app->phosphor == id)) {
                app->phosphor = id;
                set_status(app, "screen phosphor: %s", p2500_phosphor_name(id));
            }
        }

        ImGui::SeparatorText("Interface");
        if (ImGui::MenuItem("Dark", NULL, app->theme == THEME_DARK)) {
            app->theme = THEME_DARK;
            apply_style(app->theme);
            set_status(app, "interface theme: dark");
        }
        if (ImGui::MenuItem("Paper", NULL, app->theme == THEME_PAPER)) {
            app->theme = THEME_PAPER;
            apply_style(app->theme);
            set_status(app, "interface theme: paper");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Machine")) {
        if (ImGui::MenuItem("Capitals lock", NULL, app->caps_lock)) {
            app->caps_lock = !app->caps_lock;
            set_status(app, "capitals lock %s", app->caps_lock ? "on (as shipped)" : "off");
        }
        if (ImGui::MenuItem("Cartridge...")) app->show_cartridge = true;
        if (ImGui::BeginMenu("Speed")) {
            speed_menu_items(app);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Debug")) {
        p2500_panels_menu(app->panels);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About")) app->show_about = true;
        ImGui::EndMenu();
    }

    /* Right-aligned, laid out from the right edge inwards and in a fixed
     * order, so no indicator ever moves as the status text changes:
     *
     *   status text | speed | cartridge | A B C D | run/pause | capitals lock
     *
     * Each indicator is always drawn - bright when it applies, faint when it
     * does not - because a lamp that disappears is a lamp you cannot read
     * the absence of. */
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float bar_x = ImGui::GetWindowPos().x;
        const float bar_y = ImGui::GetWindowPos().y;
        const float bar_h = ImGui::GetWindowSize().y;
        const float width = ImGui::GetWindowWidth();
        const float mid_y = bar_y + bar_h * 0.5f;

        /* Sized off the bar, not the font: the capitals-lock keycap has to
         * read as a symbol at a glance, and its strokes merge into a blob
         * much under half the bar height. */
        const float gh = bar_h * 0.56f;
        const float caps_w = gh * 0.78f;
        const float run_w = gh * 0.72f;
        const float letter_w = ImGui::CalcTextSize("A").x;
        const float font_h = ImGui::GetFontSize();
        /* letter_gap is wider than it needs to look right: each drive letter
         * is a click target now, and three 7-pixel letters five pixels apart
         * are adjacent enough to mis-hit. */
        const float pad = 12.0f, gap = 9.0f, letter_gap = 9.0f;
        const float drive_cell = letter_w + letter_gap;
        const float drives_w = letter_w * (float)P2500_FDC_MAX_DRIVES +
                               letter_gap * (float)(P2500_FDC_MAX_DRIVES - 1);

        float x = bar_x + width - pad;

        const float caps_centre = x - caps_w * 0.5f;
        x -= caps_w + gap;
        dl->AddLine(ImVec2(x + 0.5f, bar_y + 3.0f), ImVec2(x + 0.5f, bar_y + bar_h - 3.0f),
                    ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        x -= gap;

        const float run_centre = x - run_w * 0.5f;
        x -= run_w + gap;
        dl->AddLine(ImVec2(x + 0.5f, bar_y + 3.0f), ImVec2(x + 0.5f, bar_y + bar_h - 3.0f),
                    ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        x -= gap;

        const float drives_left = x - drives_w;
        x = drives_left - gap;
        dl->AddLine(ImVec2(x + 0.5f, bar_y + 3.0f), ImVec2(x + 0.5f, bar_y + bar_h - 3.0f),
                    ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        x -= gap;

        /* The cartridge cell. Width reserved off the longest possible
         * reading - a full-length truncated filename - so, like the speed
         * cell, neither it nor anything left of it moves as the text
         * changes. */
        const float cart_w = ImGui::CalcTextSize("Cartridge: XXXXXXXXXXXXXX").x;
        const float cart_centre = x - cart_w * 0.5f;
        x -= cart_w + gap;
        dl->AddLine(ImVec2(x + 0.5f, bar_y + 3.0f), ImVec2(x + 0.5f, bar_y + bar_h - 3.0f),
                    ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        x -= gap;

        /* The speed cell. Its width is reserved off the widest string it can
         * ever hold, not the current one, so a reading that changes width -
         * "1x" to "~18.6x" - moves neither the cell nor the status text
         * beside it. */
        const float speed_w = ImGui::CalcTextSize("0.25x (9.9)").x;
        const float speed_centre = x - speed_w * 0.5f;
        x -= speed_w + gap;
        dl->AddLine(ImVec2(x + 0.5f, bar_y + 3.0f), ImVec2(x + 0.5f, bar_y + bar_h - 3.0f),
                    ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        const float status_right = x - gap;

        /* Only draw once there is room left of the menus, so a narrow window
         * clips the indicators instead of scribbling over "File". */
        if (status_right - bar_x > ImGui::GetCursorPosX()) {
            /* The status text first: it is the only one of these that uses
             * the layout cursor, and the lamps below move that cursor to
             * absolute positions. */
            if (app->status[0]) {
                const float text_w = ImGui::CalcTextSize(app->status).x;
                ImGui::SetCursorPosX(status_right - bar_x - text_w);
                ImGui::TextDisabled("%s", app->status);
            }

            /* Reported once, for the same reason the layout line is: a test
             * that hardcodes these pixel columns silently stops testing the
             * lamps the moment a menu is added or the font changes. */
            if (app->win_shot_path && app->frames == 2)
                SDL_Log("lamps: caps %.0f run %.0f drives %.0f %.0f %.0f %.0f"
                        " rows %.0f-%.0f cartridge %.0f speed %.0f",
                        caps_centre, run_centre,
                        drives_left + letter_w * 0.5f,
                        drives_left + drive_cell + letter_w * 0.5f,
                        drives_left + 2 * drive_cell + letter_w * 0.5f,
                        drives_left + 3 * drive_cell + letter_w * 0.5f,
                        bar_y + 2.0f, bar_y + bar_h - 2.0f, cart_centre, speed_centre);

            Lamp caps = lamp_button("##caps", caps_centre, caps_w, bar_y, bar_h);
            if (caps.clicked || caps.alt_clicked) {
                app->caps_lock = !app->caps_lock;
                set_status(app, "capitals lock %s",
                           app->caps_lock ? "on (as shipped)" : "off");
                SDL_Log("capitals lock: %s", app->caps_lock ? "on" : "off");
            }
            ImGui::SetItemTooltip("Capitals lock: %s\nThe disks ship with it engaged,"
                                  " so unshifted keys give capitals.",
                                  app->caps_lock ? "on" : "off");
            draw_caps_glyph(dl, ImVec2(caps_centre, mid_y), gh,
                            lamp_paint(dl, caps, app->caps_lock ? LAMP_ON : LAMP_OFF));

            Lamp run = lamp_button("##run", run_centre, run_w, bar_y, bar_h);
            if (run.clicked || run.alt_clicked) toggle_pause(app);
            ImGui::SetItemTooltip("%s  (F12)", app->paused ? "Paused - click to run"
                                                           : "Running - click to pause");
            draw_run_glyph(dl, ImVec2(run_centre, mid_y), gh * 0.86f, app->paused,
                           lamp_paint(dl, run, app->paused ? LAMP_ON : LAMP_OFF));

            /* A: to D: - one lamp per drive-select line the hardware has
             * (PIO port A bits 4-7, T14), which is four. How many of them a
             * guest BIOS maps is a different number and its own business:
             * P25K_B's CBIOS maps three drive letters, P25K_G's two, so a
             * disk in D: is attached to a drive the running CP/M refuses
             * with `Bdos Err On D: Select` - exactly what a real machine
             * does with the same system disk.
             *
             * So the lamp has three readings. Dim is an empty slot.
             * Mid-bright is media the running BIOS will not address.
             * Bright is attached and mapped.
             *
             * While bios_drive_letters is -1 - before CP/M is up, or on a
             * guest that is not CP/M - nothing is dimmed on mapping
             * grounds, because there is no DPH chain to read and a guess
             * would be worse than silence. */
            for (unsigned u = 0; u < P2500_FDC_MAX_DRIVES; u++) {
                const bool loaded = app->m.fdc.disk[u] != NULL;
                const int mapped = app->bios_drive_letters;
                const bool unmapped = loaded && mapped >= 0 && (int)u >= mapped;
                const LampLevel level = !loaded ? LAMP_OFF
                                      : unmapped ? LAMP_INERT : LAMP_ON;
                const float cx = drives_left + u * drive_cell + letter_w * 0.5f;
                char id[8];
                snprintf(id, sizeof id, "##dr%u", u);
                Lamp d = lamp_button(id, cx, letter_w, bar_y, bar_h);
                if (d.clicked) open_disk_dialog(app, u);
                if (d.alt_clicked) eject_disk(app, u);
                if (!loaded) {
                    ImGui::SetItemTooltip("%c: empty\nClick to load a disk.",
                                          'A' + (int)u);
                } else {
                    /* Built in pieces, so `at` must be clamped at every
                     * step: snprintf returns the length it WANTED, so a
                     * truncation would otherwise push `at` past the buffer
                     * and turn `sizeof tip - at` into a huge size_t. */
                    char tip[640];
                    size_t at = 0;
                    #define TIP(...) do { \
                        if (at < sizeof tip) { \
                            int w = snprintf(tip + at, sizeof tip - at, __VA_ARGS__); \
                            at = (w < 0) ? sizeof tip \
                               : (at + (size_t)w > sizeof tip ? sizeof tip \
                                                              : at + (size_t)w); \
                        } \
                    } while (0)
                    TIP("%c: %s", 'A' + (int)u, app->disk_name[u]);
                    if (app->disk_id[u][0]) TIP("\n%s", app->disk_id[u]);
                    if (unmapped)
                        TIP("\n\nThe running BIOS maps %d drive letter%s "
                            "(A:-%c:), so the guest will answer "
                            "`Bdos Err On %c: Select` for this one.\nThe "
                            "hardware has four drive-select lines; this "
                            "system disk does not use them all.",
                            mapped, mapped == 1 ? "" : "s",
                            'A' + mapped - 1, 'A' + (int)u);
                    if (app->write_protect & (1u << u))
                        TIP("\n\nWrite-protected: the guest sees WP in SENSE "
                            "DRIVE STATUS and a write terminates with NW.");
                    else if (app->m.fdc.sectors_written[u])
                        TIP("\n\nModified: %lu sector(s) written, in memory "
                            "only.\nFile > Save Disk As... keeps them; an "
                            "eject or a new disk throws them away.",
                            app->m.fdc.sectors_written[u]);
                    TIP("\n\nClick to change, right-click to eject.\n"
                        "A swapped-in disk is readable at once; CP/M marks"
                        " the drive read-only until a warm boot (Ctrl-C).");
                    #undef TIP
                    ImGui::SetItemTooltip("%s", tip);
                }
                const ImU32 ink = lamp_paint(dl, d, level);
                const char letter[2] = { (char)('A' + u), '\0' };
                dl->AddText(ImVec2(cx - letter_w * 0.5f, mid_y - font_h * 0.5f), ink, letter);
            }

            /* Cartridge slot. Lit when something is inserted. Left click
             * opens the Cartridge window; right click toggles insert/eject,
             * restoring the last key/ROM without retyping or reloading. */
            char cart_short[32];
            cartridge_reading(app, cart_short, sizeof cart_short);
            char cart_label[40];
            SDL_snprintf(cart_label, sizeof cart_label, "Cartridge: %s", cart_short);
            Lamp cart = lamp_button("##cartridge", cart_centre, cart_w, bar_y, bar_h);
            if (cart.clicked) app->show_cartridge = true;
            if (cart.alt_clicked) toggle_cartridge(app);
            ImGui::SetItemTooltip("%s\nClick to open the Cartridge window,"
                                  " right-click to %s.", cart_label,
                                  app->cart_inserted ? "eject" : "re-insert");
            const ImU32 cart_ink = lamp_paint(dl, cart, app->cart_inserted ? LAMP_ON : LAMP_OFF);
            const float cart_tw = ImGui::CalcTextSize(cart_label).x;
            dl->AddText(ImVec2(cart_centre - cart_tw * 0.5f, mid_y - font_h * 0.5f),
                        cart_ink, cart_label);

            /* Speed. Lit when the machine is NOT running at its own rate,
             * which is the state worth noticing - the same reason the run
             * lamp is bright when paused. Left click toggles the two speeds
             * anyone switches between mid-session; right click opens the
             * whole list, so the menu is not the only way to reach 0.5x. */
            char reading[24];
            speed_reading(app, reading, sizeof reading);
            Lamp sp = lamp_button("##speed", speed_centre, speed_w, bar_y, bar_h);
            if (sp.clicked) set_speed(app, app->speed == 0.0f ? 1.0f : 0.0f);
            if (sp.alt_clicked) ImGui::OpenPopup("##speedmenu");
            if (app->speed == 0.0f)
                ImGui::SetItemTooltip("Speed: unlimited, measuring %.1fx\n"
                                      "Click for 1x, right-click for the list.  (F11)",
                                      (double)app->speed_measured);
            else
                ImGui::SetItemTooltip("Speed: %gx, measuring %.1fx\n"
                                      "Click for unlimited, right-click for the list."
                                      "  (F11)",
                                      (double)app->speed, (double)app->speed_measured);
            const ImU32 sink = lamp_paint(dl, sp, app->speed != 1.0f ? LAMP_ON : LAMP_OFF);
            const float tw = ImGui::CalcTextSize(reading).x;
            dl->AddText(ImVec2(speed_centre - tw * 0.5f, mid_y - font_h * 0.5f),
                        sink, reading);
            if (ImGui::BeginPopup("##speedmenu")) {
                speed_menu_items(app);
                /* Reported once, for the same reason the lamp columns are: a
                 * suite that pins a pixel row per entry stops testing the
                 * list the moment a speed is added to it. */
                if (!app->speed_popup_logged) {
                    app->speed_popup_logged = true;
                    char line[256] = "speed menu:";
                    for (size_t i = 0; i < SDL_arraysize(SPEED_STEPS); i++) {
                        char one[48], lbl[16];
                        speed_label(SPEED_STEPS[i], lbl, sizeof lbl);
                        SDL_snprintf(one, sizeof one, " %s %.0f,%.0f", lbl,
                                     app->speed_item_x, app->speed_item_y[i]);
                        SDL_strlcat(line, one, sizeof line);
                    }
                    SDL_Log("%s", line);
                }
                ImGui::EndPopup();
            }
        }
    }
    ImGui::EndMainMenuBar();
}

/* Watch reports are formatted in core/debug.c, so the panel shows the same
 * line the CLI writes to stderr. */
static void gui_watch(void *userdata, uint16_t, uint8_t, uint8_t, const char *text)
{
    p2500_panels_log(*(P2500Panels *)userdata, P2500_LOG_INFO, "watch", "%s", text);
}

/* Both kinds of stop land here. Also logged to stderr, so a headless run can
 * assert that one fired rather than inferring it from pixels. */
static void hit_stop(App *app)
{
    app->paused = true;
    if (app->dbg.hit_watch >= 0) {
        const uint16_t a = app->dbg.hit_watch_addr;
        set_status(app, "stopped: $%04X changed", a);
        p2500_panels_log(app->panels, P2500_LOG_INFO, "brk",
                         "[step %lu] watch $%04X changed - stopped at $%04X",
                         app->m.total_instructions, a, app->m.cpu.pc);
        SDL_Log("watchpoint: $%04X changed, stopped at $%04X after %lu instructions",
                a, app->m.cpu.pc, app->m.total_instructions);
    } else if (app->dbg.hit_one_shot) {
        set_status(app, "stopped at $%04X", app->m.cpu.pc);
        p2500_panels_log(app->panels, P2500_LOG_INFO, "brk",
                         "[step %lu] ran to $%04X",
                         app->m.total_instructions, app->m.cpu.pc);
    } else {
        set_status(app, "stopped at breakpoint $%04X", app->m.cpu.pc);
        p2500_panels_log(app->panels, P2500_LOG_INFO, "brk",
                         "[step %lu] breakpoint $%04X reached",
                         app->m.total_instructions, app->m.cpu.pc);
        SDL_Log("breakpoint: stopped at $%04X after %lu instructions",
                app->m.cpu.pc, app->m.total_instructions);
    }
    app->panels.show_disasm = true;
}

/* Run for a budget of T-states. With nothing instrumented this is the
 * original whole-frame call, so the common case pays nothing for the
 * debugger existing; a single watch or breakpoint drops it to a per-
 * instruction loop, which is about an order of magnitude slower and still
 * comfortably faster than the real machine. */
static void run_machine(App *app, unsigned long tstates)
{
    /* Records a snapshot only when one is due; the cadence lives in
     * history.c, so the GUI and the harness rewind identically. */
    p2500_history_mark(app->hist, &app->m);
    if (!app->dbg.breaks && !app->dbg.watches && !app->dbg.counts &&
        !app->dbg.one_shot_armed) {
        p2500_run_tstates(&app->m, tstates);
        return;
    }
    const unsigned long start = app->m.cpu.cyc;
    while (app->m.cpu.cyc - start < tstates) {
        if (p2500_debug_before_step(&app->dbg, &app->m, app->m.total_instructions)) {
            hit_stop(app);
            return;
        }
        p2500_step(&app->m);
    }
}

/* Single-stepping starts with a resume so the first step leaves an address
 * the machine is already stopped on, rather than re-triggering on it. */
static void step_instructions(App *app, unsigned long n)
{
    p2500_history_mark(app->hist, &app->m);
    p2500_debug_resume(&app->dbg);
    for (unsigned long i = 0; i < n; i++) {
        if (p2500_debug_before_step(&app->dbg, &app->m, app->m.total_instructions)) {
            hit_stop(app);
            return;
        }
        p2500_step(&app->m);
    }
}

/* Rewind, from the disassembly panel's buttons.
 *
 * The log is NOT rewound: it is front-end state, and a transcript of what
 * happened should not un-happen. It gets a marker line instead, so a line
 * above the marker and one below it cannot be mistaken for one sequence.
 * (The breakpoint and counter tallies are inside the snapshot, so those do
 * rewind, whether or not that is wanted.)
 *
 * The screen needs nothing: the framebuffer is re-rendered from machine
 * state every frame, so it rewinds for free.
 */
static void do_rewind(App *app, const P2500PanelActions &act)
{
    const unsigned long here = app->m.total_instructions;
    const uint16_t was_pc = app->m.cpu.pc;
    bool moved = false;

    if (act.back_to_break) {
        moved = p2500_history_prev_break(app->hist, &app->m, &app->dbg);
        if (!moved) set_status(app, "no earlier breakpoint in history");
    } else {
        moved = p2500_history_step_back(app->hist, &app->m, act.back);
        if (!moved) set_status(app, "history does not reach back that far");
    }
    if (!moved) {
        p2500_panels_log(app->panels, P2500_LOG_WARN, "rewind",
                         "nothing to rewind to - %d snapshot(s) held",
                         p2500_history_depth(app->hist));
        /* Said out loud for the same reason the success is: a refusal that
         * only shows in a status line is a refusal nothing can guard. */
        SDL_Log("rewind: nothing to rewind to (%d snapshot(s) held)",
                p2500_history_depth(app->hist));
        return;
    }

    /* Standing still again is a stop, so the one-shot must not survive it:
     * a run-to-cursor armed before the rewind would fire on the way out. */
    app->dbg.one_shot_armed = false;
    p2500_debug_resume(&app->dbg);
    p2500_debug_baseline(&app->dbg, &app->m);
    set_status(app, "rewound to $%04X", app->m.cpu.pc);
    p2500_panels_log(app->panels, P2500_LOG_INFO, "rewind",
                     "---- rewound %lu instruction(s): $%04X (step %lu) -> "
                     "$%04X (step %lu) ----",
                     here - app->m.total_instructions, was_pc, here,
                     app->m.cpu.pc, app->m.total_instructions);
    /* Also to stderr, so a headless run can assert the rewind happened
     * rather than inferring it from a screenshot. */
    SDL_Log("rewind: %lu instruction(s) back to $%04X, instruction %lu (from "
            "$%04X, %lu)", here - app->m.total_instructions, app->m.cpu.pc,
            app->m.total_instructions, was_pc, here);
}

/*
 * Hold the guest to 50 fields a second against the wall clock.
 *
 * Not to the display's refresh: a 144 Hz monitor would otherwise run the
 * machine at 2.9x. Vsync is enabled as well, but only so the picture does
 * not tear - the timing comes from here. A scripted run (--frames) is
 * deliberately not paced, because a test has no reason to wait.
 */
static void pace_field(App *app)
{
    if ((app->frame_limit && !app->force_pacing) || app->speed == 0.0f) {
        app->next_field_ns = 0;
        return;
    }
    const Uint64 period = 1000000000ull / P2500_CLOCK_TICK_HZ;   /* 20 ms */
    const Uint64 now = SDL_GetTicksNS();
    if (app->next_field_ns == 0) app->next_field_ns = now;
    app->next_field_ns += period;
    if (app->next_field_ns > now) {
        SDL_DelayNS(app->next_field_ns - now);
    } else if (now - app->next_field_ns > period * 10) {
        /* Far enough behind that catching up would be a sprint rather than a
         * correction - a dragged window, a slow host. Start again from now
         * rather than running fast to reclaim time that is already gone. */
        app->next_field_ns = now;
    }
}

/* Where the emulated screen is drawn, and how the pixels get there.
 *
 * Pulled out of the frame callback because it is the one piece of the
 * front-end whose output is a claim about pixels: --shot-window reports it,
 * and the test suite holds a capture against the numbers.
 *
 * The four combinations of aspect x snap are all meaningful:
 *
 *            |  snap on                      |  snap off
 *   square   |  uniform integer scale        |  fills at the native 2.22:1
 *   4:3      |  per-axis integer multiples   |  exact 4:3  (the default)
 *
 * so snapping is one flag against two aspects rather than a four-way list.
 * Square+snap is the pre-2026-10 behaviour exactly, which is what the test
 * suite pins itself to - see P2500_DISPLAY. */
typedef struct {
    SDL_FRect dst;        /* the screen rect, in window pixels */
    int pre_mx, pre_my;   /* integer prescale multiples */
    bool two_pass;        /* false when dst is already an exact multiple */
    bool direct_linear;   /* dst is smaller than native: filter in one pass */
    int snap_x, snap_y;   /* whole-dot multiples actually used; 0 = none fit */
} ScreenLayout;

static ScreenLayout screen_layout(const App *app, int win_w, int win_h, int top)
{
    ScreenLayout L;
    SDL_zero(L);
    L.pre_mx = L.pre_my = 1;
    const int nw = app->info.width, nh = app->info.height;
    const int avail = win_h - top;
    if (nw <= 0 || nh <= 0 || win_w <= 0 || avail <= 0) {
        L.dst.y = (float)top;
        return L;
    }

    /* The largest rect of the wanted shape that fits below the menu bar. */
    float rw, rh;
    if (app->aspect == ASPECT_SQUARE) {
        const float s = SDL_min((float)win_w / (float)nw, (float)avail / (float)nh);
        rw = (float)nw * s; rh = (float)nh * s;
    } else {
        const float s = SDL_min((float)win_w / (float)DISPLAY_ASPECT_W,
                                (float)avail / (float)DISPLAY_ASPECT_H);
        rw = (float)DISPLAY_ASPECT_W * s; rh = (float)DISPLAY_ASPECT_H * s;
    }

    if (app->snap) {
        const int max_sx = (int)floorf(rw / (float)nw);
        const int max_sy = (int)floorf(rh / (float)nh);
        if (app->aspect == ASPECT_SQUARE) {
            /* One multiple for both axes, so every dot stays exactly square
             * and the rect keeps the framebuffer's own proportions. */
            int sc = SDL_min(max_sx, max_sy);
            if (sc < 1) sc = 1;
            rw = (float)(nw * sc); rh = (float)(nh * sc);
            L.snap_x = L.snap_y = sc;
        } else {
            /* 4:3 cannot be hit by flooring each axis on its own, and the
             * failure is not subtle: a 1600x900 window floors to 1x3, which
             * is a rect of aspect 0.74 - a portrait picture in 4:3 mode.
             * (It used to, and this is the bug that reading fixed.)
             *
             * Search the pairs instead and keep the largest whose aspect is
             * actually close to 4:3. The pairs are few - no window holds
             * more than a handful of whole 640x288 screens - so an exhaustive
             * search is cheaper than being clever about it. */
            const float target = (float)DISPLAY_ASPECT_W / (float)DISPLAY_ASPECT_H;
            long best_area = 0;
            for (int sy = 1; sy <= max_sy; sy++) {
                for (int sx = 1; sx <= max_sx; sx++) {
                    const float a = (float)(nw * sx) / (float)(nh * sy);
                    if (SDL_fabsf(a / target - 1.0f) > SNAP_ASPECT_TOLERANCE) continue;
                    const long area = (long)nw * sx * nh * sy;
                    if (area > best_area) { best_area = area; L.snap_x = sx; L.snap_y = sy; }
                }
            }
            /* Nothing close enough: keep the exact 4:3 rect and let the
             * filter deal with it. Reported, so "snapping did nothing" is
             * visible rather than mysterious. */
            if (best_area) {
                rw = (float)(nw * L.snap_x); rh = (float)(nh * L.snap_y);
            }
        }
    }

    L.dst.w = rw;
    L.dst.h = rh;
    L.dst.x = ((float)win_w - rw) * 0.5f;
    L.dst.y = (float)top + ((float)avail - rh) * 0.5f;
    if (L.dst.y < (float)top) L.dst.y = (float)top;

    /* The filter, and the reason it is two passes rather than one.
     *
     * Plain nearest at a non-integer scale is the defect being fixed, not a
     * theoretical worry: at 1.37x some dot columns land 1 px wide and others
     * 2 px, so glyph stems visibly alternate thick and thin across a row and
     * the whole screen shimmers while the window edge is dragged. Plain
     * linear from 640x288 straight to the window blurs an 8x12 glyph into
     * mush. Edge-interpolating filters (2xSaI, HQ2x, Scale2x) are worse than
     * either here: they are integer 2x/3x only - the case that needs no help
     * - and they round the corners of glyphs that came out of a hardware
     * character ROM as square-cornered dot columns.
     *
     * What works is sharp bilinear: nearest to an integer multiple at least
     * the target size, then one linear pass that therefore only ever
     * downscales, so each output pixel averages about two source samples.
     * Edges stay crisp and the dot widths come out even rather than lumpy.
     *
     * SDL 3.4 has this as SDL_SCALEMODE_PIXELART and it is NOT used, on
     * purpose: measured with tools/sdl_scalemode_probe.c, it degrades to
     * plain nearest on the software renderer, which is what
     * SDL_VIDEODRIVER=dummy gives the test suite. Headless captures would
     * then stop matching what a user sees on a GPU - unacceptable in a
     * project that asserts on pixels. Two blits behave identically
     * everywhere. */
    int mx = (int)ceilf(rw / (float)nw), my = (int)ceilf(rh / (float)nh);
    L.pre_mx = SDL_clamp(mx, 1, PRESCALE_MAX);
    L.pre_my = SDL_clamp(my, 1, PRESCALE_MAX);
    L.two_pass = rw != (float)(nw * L.pre_mx) || rh != (float)(nh * L.pre_my);
    /* A window smaller than the screen itself needs no prescale: both
     * multiples are already 1, so the intermediate would be a pixel-for-pixel
     * copy of the framebuffer and the linear pass can read it directly. */
    L.direct_linear = L.two_pass && L.pre_mx == 1 && L.pre_my == 1;
    return L;
}

/* The intermediate target for the second pass, grown on demand. Keyed on the
 * size it needs to be, so a mode change (graphics mode is 512x256) or a
 * resize replaces it without any explicit invalidation. */
static bool ensure_prescale(App *app, int mx, int my)
{
    const int w = app->info.width * mx, h = app->info.height * my;
    if (app->prescale && app->prescale_w == w && app->prescale_h == h) return true;
    SDL_Texture *t = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_TARGET, w, h);
    if (!t) {
        SDL_Log("prescale target %dx%d: %s - falling back to one pass", w, h,
                SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    if (app->prescale) SDL_DestroyTexture(app->prescale);
    app->prescale = t;
    app->prescale_w = w;
    app->prescale_h = h;
    return true;
}

static bool make_screen_texture(App *app)
{
    p2500_video_info(&app->m, &app->info);
    SDL_Texture *t = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       app->info.width, app->info.height);
    if (!t) return false;
    /* Nearest-neighbour: this is a character display, and smoothing an 8x12
     * glyph blurs it into mush. */
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    if (app->screen) SDL_DestroyTexture(app->screen);
    app->screen = t;
    free(app->fb);
    app->fb = (uint32_t *)calloc((size_t)app->info.width * app->info.height,
                                 sizeof(uint32_t));
    return app->fb != NULL;
}

/* Which tier-one setting is which, for the provenance array. The order is
 * the order the `settings:` line reports them in, and that line is what
 * makes the precedence rule testable rather than merely documented. */
enum { SRC_ASPECT, SRC_SNAP, SRC_PHOSPHOR, SRC_THEME, SRC_SPEED, SRC_CAPS,
       SRC_PANELS, SRC_WINDOW, SRC_COUNT };

static const char *source_name(P2500PrefSource s)
{
    switch (s) {
    case P2500_PREF_FILE: return "file";
    case P2500_PREF_ENV:  return "env";
    case P2500_PREF_FLAG: return "flag";
    default: return "default";
    }
}

/* Display settings come from FOUR places now, lowest precedence first: the
 * built-in defaults, the preferences file (prefs.h), P2500_DISPLAY in the
 * environment, and --aspect / --snap on the command line.
 *
 * The env var exists for the test suite. It has 54 p2500-gui invocations,
 * and threading two new flags through all of them is a bug factory;
 * run_tests.sh exports P2500_DISPLAY=aspect=square,snap=on once at the top
 * instead, which pins the whole suite to the pre-2026-10 geometry so no
 * existing capture moves. It sits ABOVE the preferences file precisely so a
 * developer's saved settings cannot reach the suite - and the suite also
 * sets P2500_CONFIG=none, which switches the file off entirely, because an
 * override only covers the settings it names. All of them are parsed into
 * sentinels first so "not given" stays distinguishable from "given the
 * default".
 *
 * Returns false on a value it does not recognise: a typo in an environment
 * variable that silently changed how the screen was rendered would be a
 * miserable thing to debug. */
/* P2500_DISPLAY is a comma-separated list of key=value pairs. Unknown keys
 * are rejected rather than ignored, for the same reason as above. */
static bool parse_display_env(const char *env, int *aspect, int *snap, int *phosphor,
                              int *theme, int *win_w, int *win_h, P2500PrefSource *src)
{
    char buf[256];
    SDL_strlcpy(buf, env, sizeof buf);
    bool ok = true;
    char *save = NULL;
    for (char *tok = SDL_strtok_r(buf, ",", &save); tok; tok = SDL_strtok_r(NULL, ",", &save)) {
        char *eq = SDL_strchr(tok, '=');
        if (!eq) { SDL_Log("P2500_DISPLAY: expected key=value, got '%s'", tok); ok = false; continue; }
        *eq = '\0';
        const char *key = tok, *val = eq + 1;
        if (!SDL_strcasecmp(key, "aspect")) {
            if (!p2500_parse_aspect(val, aspect)) { SDL_Log("P2500_DISPLAY aspect: '%s' is not 4:3 or square", val); ok = false; }
            else src[SRC_ASPECT] = P2500_PREF_ENV;
        } else if (!SDL_strcasecmp(key, "snap")) {
            if (!p2500_parse_onoff(val, snap)) { SDL_Log("P2500_DISPLAY snap: '%s' is not on or off", val); ok = false; }
            else src[SRC_SNAP] = P2500_PREF_ENV;
        } else if (!SDL_strcasecmp(key, "theme")) {
            if (!p2500_parse_theme(val, theme)) { SDL_Log("P2500_DISPLAY theme: '%s' is not dark or paper", val); ok = false; }
            else src[SRC_THEME] = P2500_PREF_ENV;
        } else if (!SDL_strcasecmp(key, "phosphor")) {
            P2500PhosphorId id;
            if (p2500_phosphor_by_name(val, &id)) { *phosphor = (int)id; src[SRC_PHOSPHOR] = P2500_PREF_ENV; }
            else { SDL_Log("P2500_DISPLAY phosphor: no such screen colour '%s'", val); ok = false; }
        } else if (!SDL_strcasecmp(key, "window")) {
            /* Same spelling as --window, and the same bounds as the file:
             * the env layer sits between them and must not be the one way
             * to ask for an unusable window. */
            int w = 0, h = 0;
            if (SDL_sscanf(val, "%dx%d", &w, &h) != 2) {
                SDL_Log("P2500_DISPLAY window: expected WxH, got '%s'", val);
                ok = false;
            } else if (w < P2500_WINDOW_MIN_W || h < P2500_WINDOW_MIN_H ||
                       w > P2500_WINDOW_MAX || h > P2500_WINDOW_MAX) {
                SDL_Log("P2500_DISPLAY window: %dx%d is outside %dx%d..%d", w, h,
                        P2500_WINDOW_MIN_W, P2500_WINDOW_MIN_H, P2500_WINDOW_MAX);
                ok = false;
            } else {
                *win_w = w; *win_h = h; src[SRC_WINDOW] = P2500_PREF_ENV;
            }
        } else {
            SDL_Log("P2500_DISPLAY: unknown setting '%s'", key);
            ok = false;
        }
    }
    return ok;
}

/* One usage text for both callers: --help prints it to stdout and exits 0,
 * a bad argument prints it to stderr and exits 1. */
static void usage(FILE *out)
{
    fprintf(out, "usage: p2500-gui [--rom path] [--charrom path] [--scale N]\n"
                 "                 [--disks A[,B[,C[,D]]]]\n"
                 "                 [--disk path] [--disk-b path] [--disk-c path]\n"
                 "                 [--disk-d path]\n"
                 "                 [--write-protect LETTERS]\n"
                 "                 [--window WxH] [--aspect 4:3|square] [--snap on|off]\n"
                 "                 [--phosphor green|white] [--theme dark|paper]\n"
                 "                 [--frames N] [--screenshot out.ppm] [--shot-window out.ppm]\n"
                 "                 [--push-at MS:STRING ...] [--no-caps-lock]\n"
                 "                 [--panels system,devices,memory,disasm,log] [--verbose-io]\n"
                 "                 [--paced] [--speed X|unlimited]\n"
                 "                 [--mouse [FRAME:]X,Y[,left|right] ...]\n"
                 "                 [--resize [FRAME:]WxH ...]\n"
                 "                 [--ui-type FRAME:TEXT ...]\n"
                 "                 [--break ADDR ...] [--watch ADDR ...]\n"
                 "                 [--watch-break ADDR ...]\n"
                 "                 [--symbols FILE ...] [--help]\n"
                 "\nThe screen is drawn 4:3 with smooth scaling by default, which is\n"
                 "what the machine's own monitor looked like; --aspect square gives\n"
                 "1:1 dots and --snap on holds every dot to a whole number of\n"
                 "pixels. P2500_DISPLAY=aspect=square,snap=on sets both from the\n"
                 "environment, and the flags override it. --phosphor white\n"
                 "swaps the green screen for a neutral grey one, which is not\n"
                 "what the machine had but is easier on some eyes.\n"
                 "\nA mounted disk is writable in memory and never written back to\n"
                 "the file it came from: File > Save Disk As... keeps the changes,\n"
                 "and --write-protect ab mounts those drives read-only.\n"
                 "\nThe View and Machine menus' settings are remembered in a\n"
                 "settings.ini under your config directory, and are overridden by\n"
                 "P2500_DISPLAY and by the flags above. P2500_CONFIG=none ignores\n"
                 "that file; P2500_CONFIG=PATH uses one of your choosing. Mounted\n"
                 "disks and the cartridge key are deliberately NOT remembered.\n"
                 "\nWith no --symbols, the shipped symbols/*.sym are matched against\n"
                 "the running build by fingerprint and the one that fits is loaded.\n"
                 "\nAddresses are hex. Symbol files are one `ADDR NAME` per line;\n"
                 "symbols/ipl.sym and symbols/cpm.sym ship with the emulator.\n");
}

/* The five debugger panels, in the order prefs.cpp stores them, rendered
 * for the `settings:` line: "system,log", or "-" for none. */
static const char *panel_list(const App *app)
{
    static char buf[64];
    static const char *name[5] = { "system", "devices", "memory", "disasm", "log" };
    const bool on[5] = { app->panels.show_system, app->panels.show_devices,
                         app->panels.show_memory, app->panels.show_disasm,
                         app->panels.show_log };
    buf[0] = '\0';
    for (int i = 0; i < 5; i++) {
        if (!on[i]) continue;
        if (buf[0]) SDL_strlcat(buf, ",", sizeof buf);
        SDL_strlcat(buf, name[i], sizeof buf);
    }
    if (!buf[0]) SDL_strlcpy(buf, "-", sizeof buf);
    return buf;
}

/* The tier-one settings as they stand right now. */
static void prefs_snapshot(const App *app, P2500Prefs *out)
{
    out->win_w = app->win_w;
    out->win_h = app->win_h;
    out->aspect = app->aspect;
    out->snap = app->snap ? 1 : 0;
    out->phosphor = (int)app->phosphor;
    out->theme = app->theme;
    out->speed = app->speed;
    out->caps_lock = app->caps_lock ? 1 : 0;
    out->panel[0] = app->panels.show_system;
    out->panel[1] = app->panels.show_devices;
    out->panel[2] = app->panels.show_memory;
    out->panel[3] = app->panels.show_disasm;
    out->panel[4] = app->panels.show_log;
}

static bool prefs_equal(const P2500Prefs &a, const P2500Prefs &b)
{
    if (a.aspect != b.aspect || a.snap != b.snap || a.phosphor != b.phosphor ||
        a.theme != b.theme || a.speed != b.speed || a.caps_lock != b.caps_lock ||
        a.win_w != b.win_w || a.win_h != b.win_h)
        return false;
    for (int i = 0; i < 5; i++) if (a.panel[i] != b.panel[i]) return false;
    return true;
}

/* Write the preferences if, and only if, one of them has changed since the
 * last write. Called once per frame rather than from each of the dozen
 * places that can change one: a menu item, a keyboard shortcut, a click on
 * a lamp and a panel's own close button are all the same event here, and a
 * setting added later is covered without anyone having to remember to.
 * The cost of the check is seven comparisons. */
/* Promote a resize that has stopped moving into the setting itself, which
 * then reaches the file through prefs_sync() like every other change. Kept
 * separate from prefs_sync so it still runs with the file switched off: the
 * `settings:` line and the window's own state should not depend on whether
 * anything is being written. */
static void window_settle(App *app)
{
    if (!app->pend_at) return;
    if (SDL_GetTicks() - app->pend_at < WINDOW_SETTLE_MS) return;
    app->pend_at = 0;
    if (app->pend_w < P2500_WINDOW_MIN_W || app->pend_h < P2500_WINDOW_MIN_H)
        return;                     /* below the floor: not a size worth keeping */
    app->win_w = app->pend_w;
    app->win_h = app->pend_h;
}

static void prefs_sync(App *app)
{
    if (!app->prefs_on) return;
    P2500Prefs now;
    prefs_snapshot(app, &now);
    if (prefs_equal(now, app->prefs_on_disk)) return;
    /* Recorded before the write rather than after: a file that cannot be
     * written must be complained about once, not once per frame. */
    app->prefs_on_disk = now;
    p2500_prefs_save(app->prefs_path, now);
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    const char *rom = "roms/ipl.bin";
    const char *charrom = "roms/charrom.bin";
    const char *disks[P2500_FDC_MAX_DRIVES] = {0};
    int scale = SCALE_DEFAULT;
    /* Whether --scale was typed, not just what it resolved to: an explicit
     * --scale is a flag and so must beat a remembered window size, while the
     * default value must not. */
    bool scale_given = false;
    /* An explicit window size, overriding the one --scale implies. */
    int win_w_arg = 0, win_h_arg = 0;
    /* Precedence, bottom up: defaults < the preferences file < P2500_DISPLAY
     * < these flags. -1 means "nothing has said anything about this yet", so
     * each layer can tell "not given" from "given the default", and the
     * provenance array records which layer last spoke. */
    P2500PrefSource src[SRC_COUNT];
    for (int i = 0; i < SRC_COUNT; i++) src[i] = P2500_PREF_DEFAULT;

    char cfgbuf[sizeof ((App *)0)->prefs_path];
    const char *cfgpath = p2500_prefs_path(cfgbuf, sizeof cfgbuf);
    P2500Prefs prefs;
    size_t prefs_bad = 0;
    if (cfgpath) p2500_prefs_load(cfgpath, &prefs, &prefs_bad);

    int aspect = prefs.aspect, snap = prefs.snap, phosphor = prefs.phosphor,
        theme = prefs.theme;
    if (aspect >= 0) src[SRC_ASPECT] = P2500_PREF_FILE;
    if (snap >= 0) src[SRC_SNAP] = P2500_PREF_FILE;
    if (phosphor >= 0) src[SRC_PHOSPHOR] = P2500_PREF_FILE;
    if (theme >= 0) src[SRC_THEME] = P2500_PREF_FILE;
    /* The remembered geometry, already bounds-checked by the loader. The
     * env layer writes over it below, and either is overridden by --window
     * or an explicit --scale. */
    int win_w_pref = prefs.win_w, win_h_pref = prefs.win_h;
    if (win_w_pref > 0 && win_h_pref > 0) src[SRC_WINDOW] = P2500_PREF_FILE;

    if (const char *env = SDL_getenv("P2500_DISPLAY")) {
        if (!parse_display_env(env, &aspect, &snap, &phosphor, &theme,
                               &win_w_pref, &win_h_pref, src)) {
            usage(stderr);
            return SDL_APP_FAILURE;
        }
    }
    unsigned long frame_limit = 0;
    const char *shot_path = NULL;
    const char *win_shot_path = NULL;
    /* Capitals lock is engaged as the disks ship; the file may remember it
     * off, and --no-caps-lock still wins over both. */
    bool caps_lock = prefs.caps_lock < 0 ? true : prefs.caps_lock > 0;
    if (prefs.caps_lock >= 0) src[SRC_CAPS] = P2500_PREF_FILE;
    unsigned write_protect = 0;
    bool force_pacing = false;
    float speed = prefs.speed < 0.0f ? 1.0f : prefs.speed;
    if (prefs.speed >= 0.0f) src[SRC_SPEED] = P2500_PREF_FILE;
    unsigned long push_at[MAX_PUSHES] = {0};
    const char *push_text[MAX_PUSHES] = {0};
    int npush = 0;
    const char *panels_arg = NULL;
    bool verbose_devices = false;
    struct { unsigned long at_frame; int x, y, button; } mouse_script[MAX_PUSHES];
    int mouse_steps = 0;
    struct { unsigned long at_frame; const char *text; } ui_type_script[MAX_PUSHES];
    struct { unsigned long at_frame; int w, h; } resize_script[MAX_PUSHES];
    int resize_steps = 0;
    int ui_type_steps = 0;
    uint16_t break_at[MAX_PUSHES] = {0};
    int nbreak = 0;
    uint16_t watch_at[MAX_PUSHES] = {0};
    int nwatch = 0;
    uint16_t wbreak_at[MAX_PUSHES] = {0};
    int nwbreak = 0;
    const char *symfiles[App::MAX_SYMFILES] = {0};
    int nsymfile = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout);
            return SDL_APP_SUCCESS;
        }
        else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
        else if (!strcmp(argv[i], "--charrom") && i + 1 < argc) charrom = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disks[0] = argv[++i];
        else if (!strcmp(argv[i], "--disk-b") && i + 1 < argc) disks[1] = argv[++i];
        else if (!strcmp(argv[i], "--disk-c") && i + 1 < argc) disks[2] = argv[++i];
        else if (!strcmp(argv[i], "--disk-d") && i + 1 < argc) disks[3] = argv[++i];
        else if (!strcmp(argv[i], "--write-protect") && i + 1 < argc) {
            for (const char *p = argv[++i]; *p; p++) {
                int d = (*p >= 'a' && *p <= 'z') ? *p - 'a'
                      : (*p >= 'A' && *p <= 'Z') ? *p - 'A' : -1;
                if (d < 0 || d >= P2500_FDC_MAX_DRIVES) {
                    SDL_Log("--write-protect: '%c' is not a drive letter (a-%c)",
                            *p, 'a' + P2500_FDC_MAX_DRIVES - 1);
                    return SDL_APP_FAILURE;
                }
                write_protect |= 1u << d;
            }
        }
        /* See the CLI's note: four is the hardware's number of select
         * lines, not a claim about what the guest BIOS maps (three on
         * P25K_B, two on P25K_G). */
        else if (!strcmp(argv[i], "--disks") && i + 1 < argc) {
            /* By hand, so an empty field skips a drive - see the CLI. */
            char *p = argv[++i];
            unsigned u = 0;
            for (;;) {
                char *comma = SDL_strchr(p, ',');
                if (comma) *comma = '\0';
                if (u >= P2500_FDC_MAX_DRIVES) {
                    SDL_Log("--disks: at most %d images", P2500_FDC_MAX_DRIVES);
                    return SDL_APP_FAILURE;
                }
                if (*p) disks[u] = p;
                u++;
                if (!comma) break;
                p = comma + 1;
            }
        }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) { scale = atoi(argv[++i]); scale_given = true; }
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) {
            if (SDL_sscanf(argv[++i], "%dx%d", &win_w_arg, &win_h_arg) != 2 ||
                win_w_arg < 64 || win_h_arg < 64) {
                SDL_Log("--window: expected WxH, both at least 64");
                return SDL_APP_FAILURE;
            }
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frame_limit = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--aspect") && i + 1 < argc) {
            if (!p2500_parse_aspect(argv[++i], &aspect)) {
                SDL_Log("--aspect: expected 4:3 or square, got %s", argv[i]);
                usage(stderr);
                return SDL_APP_FAILURE;
            }
            src[SRC_ASPECT] = P2500_PREF_FLAG;
        }
        else if (!strcmp(argv[i], "--snap") && i + 1 < argc) {
            if (!p2500_parse_onoff(argv[++i], &snap)) {
                SDL_Log("--snap: expected on or off, got %s", argv[i]);
                usage(stderr);
                return SDL_APP_FAILURE;
            }
            src[SRC_SNAP] = P2500_PREF_FLAG;
        }
        else if (!strcmp(argv[i], "--phosphor") && i + 1 < argc) {
            P2500PhosphorId id;
            if (!p2500_phosphor_by_name(argv[++i], &id)) {
                SDL_Log("--phosphor: no such screen colour: %s", argv[i]);
                usage(stderr);
                return SDL_APP_FAILURE;
            }
            phosphor = (int)id;
            src[SRC_PHOSPHOR] = P2500_PREF_FLAG;
        }
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) {
            if (!p2500_parse_theme(argv[++i], &theme)) {
                SDL_Log("--theme: expected dark or paper, got %s", argv[i]);
                usage(stderr);
                return SDL_APP_FAILURE;
            }
            src[SRC_THEME] = P2500_PREF_FLAG;
        }
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot_path = argv[++i];
        else if (!strcmp(argv[i], "--shot-window") && i + 1 < argc) win_shot_path = argv[++i];
        else if (!strcmp(argv[i], "--no-caps-lock")) { caps_lock = false; src[SRC_CAPS] = P2500_PREF_FLAG; }
        /* --frames runs flat out so tests do not wait; --paced puts the 50 Hz
         * throttle back, which is how the throttle itself gets tested. */
        else if (!strcmp(argv[i], "--paced")) force_pacing = true;
        /* --speed X | unlimited - the same setting the Machine > Speed menu
         * holds, so a headless run can be given one. */
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
            const char *s = argv[++i];
            speed = (!strcmp(s, "unlimited") || !strcmp(s, "max")) ? 0.0f
                                                                  : (float)atof(s);
            if (speed < 0.0f) speed = 0.0f;
            src[SRC_SPEED] = P2500_PREF_FLAG;
        }
        /* The debug flags exist so the panels can be driven - and captured -
         * with no display, the same way --frames/--shot-window already let
         * the screen be checked headlessly. */
        else if (!strcmp(argv[i], "--panels") && i + 1 < argc) { panels_arg = argv[++i]; src[SRC_PANELS] = P2500_PREF_FLAG; }
        else if (!strcmp(argv[i], "--verbose-io")) verbose_devices = true;
        else if (!strcmp(argv[i], "--ui-type") && i + 1 < argc && ui_type_steps < MAX_PUSHES) {
            char *arg = argv[++i], *colon = strchr(arg, ':');
            if (!colon) { SDL_Log("bad --ui-type, want FRAME:TEXT"); return SDL_APP_FAILURE; }
            *colon = '\0';
            ui_type_script[ui_type_steps].at_frame = strtoul(arg, NULL, 0);
            ui_type_script[ui_type_steps].text = colon + 1;
            ui_type_steps++;
        }
        else if (!strcmp(argv[i], "--resize") && i + 1 < argc && resize_steps < MAX_PUSHES) {
            /* [FRAME:]WxH, the frame defaulting to 3 like --mouse's. */
            char *arg = argv[++i], *colon = strchr(arg, ':');
            unsigned long at = 3;
            if (colon) { *colon = '\0'; at = strtoul(arg, NULL, 0); arg = colon + 1; }
            int w = 0, h = 0;
            if (SDL_sscanf(arg, "%dx%d", &w, &h) != 2 || w < 1 || h < 1) {
                SDL_Log("--resize: expected [FRAME:]WxH");
                return SDL_APP_FAILURE;
            }
            resize_script[resize_steps].at_frame = at;
            resize_script[resize_steps].w = w;
            resize_script[resize_steps].h = h;
            resize_steps++;
        }
        else if (!strcmp(argv[i], "--mouse") && i + 1 < argc && mouse_steps < MAX_PUSHES) {
            /* [FRAME:]X,Y[,left|right] - the frame is optional and defaults
             * to 3, which is the earliest the UI has laid itself out. */
            char *arg = argv[++i];
            char *colon = strchr(arg, ':');
            unsigned long at = 3;
            if (colon) { *colon = '\0'; at = strtoul(arg, NULL, 0); arg = colon + 1; }
            char *c1 = strchr(arg, ',');
            char *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
            mouse_script[mouse_steps].at_frame = at;
            mouse_script[mouse_steps].x = atoi(arg);
            mouse_script[mouse_steps].y = c1 ? atoi(c1 + 1) : 0;
            mouse_script[mouse_steps].button =
                c2 ? ((c2[1] == 'r' || c2[1] == 'R') ? 2 : 1) : 0;
            mouse_steps++;
        }
        else if (!strcmp(argv[i], "--symbols") && i + 1 < argc) {
            if (nsymfile >= App::MAX_SYMFILES) {
                SDL_Log("at most %d --symbols files", App::MAX_SYMFILES);
                return SDL_APP_FAILURE;
            }
            symfiles[nsymfile++] = argv[++i];
        }
        else if (!strcmp(argv[i], "--break") && i + 1 < argc && nbreak < MAX_PUSHES)
            break_at[nbreak++] = (uint16_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc && nwatch < MAX_PUSHES)
            watch_at[nwatch++] = (uint16_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--watch-break") && i + 1 < argc && nwbreak < MAX_PUSHES)
            wbreak_at[nwbreak++] = (uint16_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--push-at") && i + 1 < argc && npush < MAX_PUSHES) {
            char *arg = argv[++i], *colon = strchr(arg, ':');
            if (!colon) { SDL_Log("bad --push-at, want MS:STRING"); return SDL_APP_FAILURE; }
            *colon = '\0';
            push_at[npush] = strtoul(arg, NULL, 0);
            push_text[npush] = colon + 1;
            npush++;
        }
        else {
            SDL_Log("unknown or malformed argument: %s", argv[i]);
            usage(stderr);
            return SDL_APP_FAILURE;
        }
    }
    if (scale < 1 || scale > 8) scale = SCALE_DEFAULT;

    /* new, not calloc: App now holds a C++ member (P2500Panels) whose
     * default member initialisers have to actually run. */
    App *app = new App();
    *appstate = app;

    app->frame_limit = frame_limit;
    app->shot_path = shot_path;
    app->win_shot_path = win_shot_path;
    app->caps_lock = caps_lock;
    app->aspect = aspect < 0 ? ASPECT_43 : aspect;
    app->snap = snap > 0;
    app->phosphor = phosphor < 0 ? P2500_PHOSPHOR_GREEN : (P2500PhosphorId)phosphor;
    app->theme = theme < 0 ? THEME_DARK : theme;
    app->write_protect = write_protect;
    app->force_pacing = force_pacing;
    app->speed = speed;
    app->start_ns = SDL_GetTicksNS();
    app->pushes = npush;
    for (int i = 0; i < npush; i++) {
        app->push[i].at_ms = push_at[i];
        app->push[i].text = push_text[i];
    }

    p2500_init(&app->m);
    app->hist = p2500_history_create();
    if (!app->hist) {
        SDL_Log("failed to allocate the execution history ring");
        return SDL_APP_FAILURE;
    }
    if (!p2500_load_rom(&app->m, rom)) {
        SDL_Log("failed to load boot ROM from %s", rom);
        return SDL_APP_FAILURE;
    }
    if (!p2500_load_charrom(&app->m, charrom)) {
        SDL_Log("failed to load character ROM from %s", charrom);
        return SDL_APP_FAILURE;
    }
    for (unsigned u = 0; u < P2500_FDC_MAX_DRIVES; u++) {
        if (!disks[u]) continue;
        size_t n = 0;
        app->disk[u] = read_whole_file(disks[u], &n);
        if (!app->disk[u]) {
            SDL_Log("failed to load disk image %s", disks[u]);
            return SDL_APP_FAILURE;
        }
        if (write_protect & (1u << u))
            p2500_fdc_attach(&app->m.fdc, u, app->disk[u], n);
        else
            p2500_fdc_attach_writable(&app->m.fdc, u, app->disk[u], n);
        const char *base = SDL_strrchr(disks[u], '/');
        SDL_strlcpy(app->disk_name[u], base ? base + 1 : disks[u], sizeof app->disk_name[u]);
        identify_disk(app, u, app->disk[u], n);
        if (app->disk_id[u][0])
            SDL_Log("attached %c: %s (%lu bytes) - %s", 'A' + (int)u, disks[u],
                    (unsigned long)n, app->disk_id[u]);
        else
            SDL_Log("attached %c: %s (%lu bytes)", 'A' + (int)u, disks[u], (unsigned long)n);
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    p2500_video_info(&app->m, &app->info);
    char window_title[64];
    SDL_snprintf(window_title, sizeof window_title, "Philips P2500 v%s", P2500_VERSION_STR);
    /* The default window is sized so the screen rect fills it exactly, which
     * in 4:3 means growing vertically: the width stays an exact multiple of
     * the framebuffer's, so no horizontal dot detail is ever discarded, and
     * the height follows from the aspect. --scale therefore means "N window
     * pixels per dot horizontally"; under --aspect square it means what it
     * always did. At scale 2 that is 1280x984 rather than 1280x600.
     *
     * Then clamped to the display's usable bounds, dropping a scale step at a
     * time: 984 px plus a title bar does not fit a 1080p laptop, and a window
     * that opens taller than the screen is worse than a smaller one. An
     * explicit --window is honoured exactly and never clamped - the test
     * suite depends on getting the size it asked for.
     *
     * The clamp is skipped under the dummy video driver, and that is not a
     * detail: dummy reports a fixed 1024x768 stub display, so clamping to it
     * would shrink the default window headlessly - a scale step smaller than
     * what a user with a real monitor gets, and a different size from what
     * every pre-existing --shot-window capture was taken at. There is no
     * display to fit, so --scale is trusted instead. */
    /*
     * A remembered size sits between those two: it is the preferences
     * file's layer, so --window and an explicit --scale both beat it, and
     * P2500_DISPLAY window=WxH beats it from in between. Unlike --window it
     * IS bounded at both ends, which is the whole point of remembering it -
     * a geometry that reopens the emulator unusable is worse than one that
     * forgets. The floor is P2500_WINDOW_MIN_W/H and is also installed on
     * the window itself below, so a drag cannot produce one; the ceiling is
     * the display's usable bounds, so moving to a smaller monitor (or
     * unplugging the big one) gives back a window that fits rather than one
     * whose title bar is off-screen.
     */
    int win_w = win_w_arg, win_h = win_h_arg;
    P2500PrefSource win_src = P2500_PREF_FLAG;    /* --window, until shown otherwise */
    SDL_Rect usable;
    SDL_zero(usable);
    {
        const char *drv = SDL_GetCurrentVideoDriver();
        const SDL_DisplayID did = SDL_GetPrimaryDisplay();
        const bool have_display = drv && SDL_strcmp(drv, "dummy") != 0 && did != 0;
        if (!have_display || !SDL_GetDisplayUsableBounds(did, &usable))
            SDL_zero(usable);
    }
    if (!win_w || !win_h) {
        if (!scale_given && win_w_pref > 0 && win_h_pref > 0) {
            win_w = win_w_pref;
            win_h = win_h_pref;
            win_src = src[SRC_WINDOW];            /* file or env, set above */
            if (win_w < P2500_WINDOW_MIN_W) win_w = P2500_WINDOW_MIN_W;
            if (win_h < P2500_WINDOW_MIN_H) win_h = P2500_WINDOW_MIN_H;
            if (usable.w > 0 && usable.h > 0) {
                if (win_w > usable.w) win_w = usable.w;
                if (win_h > usable.h) win_h = usable.h;
            }
            if (win_w != win_w_pref || win_h != win_h_pref)
                SDL_Log("window: remembered %dx%d does not fit here - using %dx%d",
                        win_w_pref, win_h_pref, win_w, win_h);
        } else {
            win_src = scale_given ? P2500_PREF_FLAG : P2500_PREF_DEFAULT;
            for (; scale >= 1; scale--) {
                win_w = app->info.width * scale;
                win_h = (app->aspect == ASPECT_SQUARE
                         ? app->info.height * scale
                         : win_w * DISPLAY_ASPECT_H / DISPLAY_ASPECT_W) + MENU_RESERVE;
                if (scale == 1) break;
                if (usable.w <= 0 || usable.h <= 0) break;   /* unknown: trust --scale */
                if (win_w <= usable.w && win_h <= usable.h) break;
            }
            if (scale < 1) scale = 1;
        }
    }
    src[SRC_WINDOW] = win_src;
    if (!SDL_CreateWindowAndRenderer(window_title, win_w, win_h,
                                     SDL_WINDOW_RESIZABLE,
                                     &app->window, &app->renderer)) {
        SDL_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    p2500_set_window_icon(app->window);
    /* The floor, installed on the window so a DRAG cannot get under it
     * either - checking it only on load would mean the file faithfully
     * remembering a window the user had already made useless.
     *
     * It never shrinks what was asked for: --window is honoured exactly,
     * including below the floor, so the minimum drops to the requested size
     * in that case. The user still cannot drag smaller than they asked. */
    SDL_SetWindowMinimumSize(app->window,
                             win_w < P2500_WINDOW_MIN_W ? win_w : P2500_WINDOW_MIN_W,
                             win_h < P2500_WINDOW_MIN_H ? win_h : P2500_WINDOW_MIN_H);
    /* The baseline for the remembered geometry: what the window actually
     * opened at, read back rather than assumed, since a window manager is
     * free to have had its own opinion. Nothing is written until a resize
     * settles to something different. */
    if (!SDL_GetWindowSize(app->window, &app->win_w, &app->win_h)) {
        app->win_w = win_w;
        app->win_h = win_h;
    }
    /* Tear-free presentation; the actual speed is pace_field's business. */
    if (!SDL_SetRenderVSync(app->renderer, 1))
        SDL_Log("vsync unavailable (%s) - pacing still holds 50 Hz", SDL_GetError());
    if (!make_screen_texture(app)) return SDL_APP_FAILURE;

    /* ImGui runs even under the dummy video driver, so the UI itself can be
     * checked headlessly with --shot-window. */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = NULL; /* do not litter the working dir */
    apply_style(app->theme);
    app->has_ui = ImGui_ImplSDL3_InitForSDLRenderer(app->window, app->renderer) &&
                  ImGui_ImplSDLRenderer3_Init(app->renderer);
    if (!app->has_ui) SDL_Log("ImGui backend init failed; running without a UI");

    p2500_set_log(&app->m, p2500_panels_log_sink, &app->panels);
    p2500_debug_init(&app->dbg);
    app->dbg.on_watch = gui_watch;
    app->dbg.userdata = &app->panels;
    for (int b = 0; b < nbreak; b++) p2500_debug_add_break(&app->dbg, break_at[b]);
    for (int w = 0; w < nwatch; w++) p2500_debug_add_watch(&app->dbg, watch_at[w], 1);
    for (int w = 0; w < nwbreak; w++) {
        int i = p2500_debug_add_watch(&app->dbg, wbreak_at[w], 1);
        if (i >= 0) app->dbg.watch[i].stop = true;
    }
    for (int f = 0; f < nsymfile; f++) app->symfile[f] = symfiles[f];
    app->symfiles = nsymfile;
    /* Asking for files by name means you want those, not whatever the
     * fingerprints find. */
    app->symbols_auto = (nsymfile == 0);
    load_symbol_files(app);
    p2500_debug_baseline(&app->dbg, &app->m);
    /* --panels names the whole set, so it replaces whatever the file
     * remembered rather than merging with it. */
    if (panels_arg) {
        app->panels.show_system = strstr(panels_arg, "system") != NULL;
        app->panels.show_devices = strstr(panels_arg, "devices") != NULL;
        app->panels.show_memory = strstr(panels_arg, "memory") != NULL;
        app->panels.show_disasm = strstr(panels_arg, "disasm") != NULL;
        app->panels.show_log = strstr(panels_arg, "log") != NULL;
    } else {
        bool *shown[5] = { &app->panels.show_system, &app->panels.show_devices,
                           &app->panels.show_memory, &app->panels.show_disasm,
                           &app->panels.show_log };
        for (int i = 0; i < 5; i++) {
            if (prefs.panel[i] < 0) continue;
            *shown[i] = prefs.panel[i] > 0;
            src[SRC_PANELS] = P2500_PREF_FILE;
        }
    }
    app->panels.verbose_devices = verbose_devices;
    app->ui_types = ui_type_steps;
    for (int i = 0; i < ui_type_steps; i++) {
        app->ui_type[i].at_frame = ui_type_script[i].at_frame;
        app->ui_type[i].text = ui_type_script[i].text;
    }
    app->resizes = resize_steps;
    for (int i = 0; i < resize_steps; i++) {
        app->resize[i].at_frame = resize_script[i].at_frame;
        app->resize[i].w = resize_script[i].w;
        app->resize[i].h = resize_script[i].h;
    }
    app->mouse_steps = mouse_steps;
    for (int i = 0; i < mouse_steps; i++) {
        app->mouse[i].at_frame = mouse_script[i].at_frame;
        app->mouse[i].x = mouse_script[i].x;
        app->mouse[i].y = mouse_script[i].y;
        app->mouse[i].button = mouse_script[i].button;
    }

    /* Where the preferences came from and where they will go. Reported
     * unconditionally rather than only on a change, because "which of the
     * four layers set this" is exactly the question that is impossible to
     * answer afterwards from a screenshot - and because it is what makes
     * the precedence rule a check in the suite rather than a comment. */
    app->prefs_on = cfgpath != NULL;
    if (cfgpath) SDL_strlcpy(app->prefs_path, cfgpath, sizeof app->prefs_path);
    if (!app->prefs_on)
        SDL_Log("config: off (P2500_CONFIG=none)");
    else if (prefs_bad)
        /* %llu with a cast, not %zu: GCC checks SDL's own format strings
         * against the MS dialect on MinGW, which has no 'z' modifier. */
        SDL_Log("config: %s (%llu line%s not understood, and kept)", app->prefs_path,
                (unsigned long long)prefs_bad, prefs_bad == 1 ? "" : "s");
    else
        SDL_Log("config: %s", app->prefs_path);

    char speedlab[16];
    speed_label(app->speed, speedlab, sizeof speedlab);
    SDL_Log("settings: aspect %s (%s), snap %s (%s), phosphor %s (%s),"
            " theme %s (%s), speed %s (%s), caps-lock %s (%s), panels %s (%s),"
            " window %dx%d (%s)",
            p2500_aspect_name(app->aspect), source_name(src[SRC_ASPECT]),
            app->snap ? "on" : "off", source_name(src[SRC_SNAP]),
            p2500_phosphor_name(app->phosphor), source_name(src[SRC_PHOSPHOR]),
            p2500_theme_name(app->theme), source_name(src[SRC_THEME]),
            speedlab, source_name(src[SRC_SPEED]),
            app->caps_lock ? "on" : "off", source_name(src[SRC_CAPS]),
            panel_list(app), source_name(src[SRC_PANELS]),
            app->win_w, app->win_h, source_name(src[SRC_WINDOW]));

    /* The baseline for the write-on-change check below: whatever the four
     * layers resolved to is what the user is looking at, so nothing is
     * written until they change something. */
    prefs_snapshot(app, &app->prefs_on_disk);

    SDL_StartTextInput(app->window);
    set_status(app, disks[0] ? "ready" : "no disk - File > Load Disk A...");
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    App *app = (App *)appstate;
    if (app->has_ui) ImGui_ImplSDL3_ProcessEvent(event);
    const bool ui_has_keyboard = app->has_ui && ImGui::GetIO().WantCaptureKeyboard;

    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_WINDOW_RESIZED:
        /* Held pending rather than recorded: see WINDOW_SETTLE_MS. A drag
         * delivers one of these per frame, and only the last one is a size
         * the user chose. */
        app->pend_w = event->window.data1;
        app->pend_h = event->window.data2;
        app->pend_at = SDL_GetTicks();
        return SDL_APP_CONTINUE;
    case SDL_EVENT_TEXT_INPUT:
        if (ui_has_keyboard) return SDL_APP_CONTINUE;
        /* One byte per character the platform produced. Anything outside
         * 7-bit ASCII needs the $E274 dead-key table decoded first, so
         * it is dropped rather than guessed at. */
        for (const char *c = event->text.text; *c; c++) {
            unsigned char b = (unsigned char)*c;
            if (b < 0x20 || b >= 0x7F) continue;
            /* With capitals lock off we pre-invert, cancelling CBIOS's own
             * XOR, so the letter that appears is the letter that was typed. */
            if (!app->caps_lock && SDL_isalpha(b)) b ^= 0x20;
            p2500_keyboard_push(&app->m.keyboard, b);
        }
        return SDL_APP_CONTINUE;
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode k = event->key.key;
        const bool ctrl = (event->key.mod & SDL_KMOD_CTRL) != 0;
        /* Menu accelerators win over the guest, which is why Ctrl-C still
         * reaches CP/M: it is deliberately not one of them. */
        if (ctrl && k == SDLK_O) { open_disk_dialog(app, 0); return SDL_APP_CONTINUE; }
        if (ctrl && k == SDLK_R) { p2500_reset(&app->m); p2500_history_clear(app->hist); p2500_debug_baseline(&app->dbg, &app->m); p2500_debug_clear_symbols(&app->dbg); app->symbols_auto_from[0] = '\0'; load_symbol_files(app); app->dbg.one_shot_armed = false; set_status(app, "reset - media still attached"); return SDL_APP_CONTINUE; }
        if (ctrl && k == SDLK_Q) return SDL_APP_SUCCESS;
        if (k == SDLK_F10) { save_screenshot(app); return SDL_APP_CONTINUE; }
        if (k == SDLK_F12) { toggle_pause(app); return SDL_APP_CONTINUE; }
        if (k == SDLK_F11) { set_speed(app, app->speed == 0.0f ? 1.0f : 0.0f); return SDL_APP_CONTINUE; }
        if (k == SDLK_F1) { app->panels.show_devices = !app->panels.show_devices; return SDL_APP_CONTINUE; }
        if (k == SDLK_F2) { app->panels.show_memory = !app->panels.show_memory; return SDL_APP_CONTINUE; }
        if (k == SDLK_F3) { app->panels.show_disasm = !app->panels.show_disasm; return SDL_APP_CONTINUE; }
        if (k == SDLK_F4) { app->panels.show_log = !app->panels.show_log; return SDL_APP_CONTINUE; }
        if (k == SDLK_F5) { app->panels.show_system = !app->panels.show_system; return SDL_APP_CONTINUE; }
        if (ui_has_keyboard) return SDL_APP_CONTINUE;
        int b = key_to_byte(k, event->key.mod);
        if (b >= 0) p2500_keyboard_push(&app->m.keyboard, (uint8_t)b);
        return SDL_APP_CONTINUE;
    }
    default:
        return SDL_APP_CONTINUE;
    }
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    App *app = (App *)appstate;

    /* One switch, applied every frame, so toggling the menu item takes
     * effect immediately and a --verbose-io start behaves identically. */
    {
        const bool v = app->panels.verbose_devices;
        app->m.verbose_unknown_ports = v;
        app->m.fdc.verbose = v;
        app->m.pio.verbose = v;
        app->m.dma.verbose = v;
        app->m.ctc.verbose = v;
        app->m.cartridge.verbose = v;
        app->m.keyboard.verbose = v;
        app->m.intctl.verbose = v;
    }

    if (SDL_GetAtomicInt(&app->disk_pending)) {
        SDL_SetAtomicInt(&app->disk_pending, 0);
        mount_disk(app, app->pending_unit, app->disk_path);
    }

    if (SDL_GetAtomicInt(&app->save_pending)) {
        SDL_SetAtomicInt(&app->save_pending, 0);
        save_disk(app, app->saving_unit, app->save_path);
    }

    if (SDL_GetAtomicInt(&app->cart_pending)) {
        SDL_SetAtomicInt(&app->cart_pending, 0);
        mount_cartridge(app, app->cart_path);
    }

    for (int i = 0; i < app->resizes; i++)
        if (app->frames == app->resize[i].at_frame)
            SDL_SetWindowSize(app->window, app->resize[i].w, app->resize[i].h);

    for (int i = 0; i < app->pushes; i++) {
        if (app->push[i].done) continue;
        if (app->m.cpu.cyc < app->push[i].at_ms * (P2500_CPU_HZ / 1000u)) continue;
        for (const char *c = app->push[i].text; *c; c++) {
            uint8_t b = (uint8_t)*c;
            if (b == '\\' && c[1]) {
                c++;
                b = (*c == 'r') ? 0x0D : (*c == 'n') ? 0x0A : (*c == 't') ? 0x09 : (uint8_t)*c;
            }
            p2500_keyboard_push(&app->m.keyboard, b);
        }
        app->push[i].done = true;
    }

    if (!app->paused) {
        if (app->speed == 0.0f) {
            /* Unlimited: emulate for a slice of wall time rather than a
             * fixed number of fields. A fixed batch has to be guessed, and
             * both ways of guessing wrong are bad - too small and vsync caps
             * the speed at the display's refresh rate, too large and the UI
             * stops answering the mouse. A deadline saturates whatever the
             * host can manage without anyone naming a number, and the slice
             * is shorter than one displayed frame so input still lands. */
            const Uint64 deadline = SDL_GetTicksNS() + 12000000ull;   /* 12 ms */
            do {
                run_machine(app, P2500_TSTATES_PER_FRAME);
            } while (!app->paused && SDL_GetTicksNS() < deadline);
        } else {
            /* One field scaled by the speed, carrying the fraction: at 0.5x
             * that is half a field per iteration, which is what keeps the
             * display at 50 Hz while the guest runs slow. The carry matters
             * at 0.25x and below, where truncating each budget would lose a
             * measurable slice of every field. */
            const double want = (double)P2500_TSTATES_PER_FRAME * (double)app->speed
                                + app->tstate_carry;
            const unsigned long budget = (unsigned long)want;
            app->tstate_carry = want - (double)budget;
            if (budget) run_machine(app, budget);
        }
    }
    measure_speed(app);
    app->frames++;
    if ((app->frames % 15) == 0) {
        try_auto_symbols(app);
        P2500DiskMap dm;
        app->bios_drive_letters = p2500_sysinfo_disk_map(&app->m, &dm)
                                  ? dm.count : -1;
    }

    /* The CRTC can be reprogrammed by the guest - the manual's graphics mode
     * reinitialises it - so re-check the geometry rather than trusting the
     * size the window was created with. */
    P2500VideoInfo now;
    p2500_video_info(&app->m, &now);
    if (now.width != app->info.width || now.height != app->info.height)
        if (!make_screen_texture(app)) return SDL_APP_FAILURE;

    /* ~3 Hz blink, which is the usual rate for a text cursor. */
    bool blink_on = ((app->frames / 8) & 1) == 0;
    p2500_video_render(&app->m, p2500_phosphor(app->phosphor), blink_on,
                       app->fb, app->info.width);
    SDL_UpdateTexture(app->screen, NULL, app->fb,
                      app->info.width * (int)sizeof(uint32_t));

    if (app->has_ui) {
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        /* After the backend's own NewFrame, so this position is the one
         * ImGui::NewFrame() ends up with - the backend would otherwise
         * overwrite it with whatever the platform reports, which under the
         * dummy video driver is nothing useful. The click is delivered as a
         * press and a release on consecutive frames because that is what a
         * real one is; a same-frame pair never registers. */
        if (app->mouse_steps) {
            ImGuiIO &io = ImGui::GetIO();
            /* Move on the step's own frame, press on the next and release on
             * the one after: ImGui has to have hovered the item for a frame
             * before a press lands on it. */
            for (int i = 0; i < app->mouse_steps; i++)
                if (app->frames == app->mouse[i].at_frame) {
                    app->mouse_x = app->mouse[i].x;
                    app->mouse_y = app->mouse[i].y;
                }
            if (app->mouse_x >= 0)
                io.AddMousePosEvent((float)app->mouse_x, (float)app->mouse_y);
            for (int i = 0; i < app->ui_types; i++)
                if (app->frames == app->ui_type[i].at_frame)
                    for (const char *c = app->ui_type[i].text; *c; c++)
                        io.AddInputCharacter((unsigned)(unsigned char)*c);
            for (int i = 0; i < app->mouse_steps; i++) {
                if (!app->mouse[i].button) continue;
                if (app->frames == app->mouse[i].at_frame + 1)
                    io.AddMouseButtonEvent(app->mouse[i].button - 1, true);
                if (app->frames == app->mouse[i].at_frame + 2)
                    io.AddMouseButtonEvent(app->mouse[i].button - 1, false);
            }
        }
        ImGui::NewFrame();
        draw_menu_bar(app);
        draw_cartridge_window(app);
        draw_about_window(app);
        bool want_pause_toggle = false;
        P2500PanelActions act = p2500_panels_draw(app->panels, app->m, app->dbg,
                                                  app->paused, &want_pause_toggle);
        ImGui::Render();
        /* Whether the guest can be typed into at all. Logged on change,
         * because "the UI took the keyboard and never gave it back" is a
         * transition, not a state you can catch in a screenshot. */
        const bool captured = ImGui::GetIO().WantCaptureKeyboard;
        if (captured != app->ui_had_keyboard) {
            app->ui_had_keyboard = captured;
            SDL_Log("ui keyboard: %s", captured ? "captured by a panel"
                                                : "released to the P2500");
        }
        /* Re-arm the guest's keyboard. ImGui's SDL3 backend calls
         * SDL_StopTextInput() when one of its text fields loses focus, which
         * switches SDL_EVENT_TEXT_INPUT off for the whole window and leaves
         * it off - that event is where every printable key the guest
         * receives comes from, so it must be re-enabled once a panel field
         * is done with it. Checked every frame rather than on a transition:
         * the backend can stop text input at moments we do not model, and
         * asking SDL what the state actually is costs nothing. */
        if (!captured && !SDL_TextInputActive(app->window))
            SDL_StartTextInput(app->window);
        if (app->dbg.watches != app->watches_was) {
            app->watches_was = app->dbg.watches;
            SDL_Log("watches: %d", app->dbg.watches);
        }
        if (app->text_input_was != SDL_TextInputActive(app->window)) {
            app->text_input_was = !app->text_input_was;
            SDL_Log("guest text input: %s", app->text_input_was ? "on" : "off");
        }
        if (want_pause_toggle) toggle_pause(app);
        /* Run-to-cursor and step-over: the panel has armed the one-shot, so
         * all that is left is to let the machine run. Resuming here rather
         * than stepping a fixed budget is what makes them both honour
         * everything else - watches, breakpoints, the pacing. */
        if (act.resume && app->paused) toggle_pause(app);
        /* Stepping runs after the frame is composed, so the listing the user
         * clicked is the state the step started from. ~0 means "one 50 Hz
         * field", the same budget the free-running loop uses. */
        if (act.steps == ~0UL) {
            p2500_debug_resume(&app->dbg);
            run_machine(app, P2500_TSTATES_PER_FRAME);
        } else if (act.steps) {
            step_instructions(app, act.steps);
        }
        if (act.back || act.back_to_break) do_rewind(app, act);
    }

    /* Scale the screen into whatever is left below the menu bar and centre
     * it. Done by hand rather than with SDL_SetRenderLogicalPresentation
     * because that would scale the UI too. */
    int win_w = 0, win_h = 0;
    SDL_GetRenderOutputSize(app->renderer, &win_w, &win_h);
    const int top = (int)(app->menu_h + 0.5f);
    const ScreenLayout lay = screen_layout(app, win_w, win_h, top);
    const SDL_FRect dst = lay.dst;

    /* Reported so a headless run can assert the layout directly - and, now
     * that the letterbox is a different black from the screen's own
     * background, so a capture can be held against this line rather than
     * having to be trusted on its own. The capture alone still cannot carry
     * the check: the viewport fills the default window exactly, so there is
     * nothing to measure until the window is an awkward size (--window). */
    if (app->win_shot_path && app->frames == 2) {
        SDL_Log("layout: menu %d px, screen at y=%.0f x=%.0f, %.0fx%.0f",
                top, dst.y, dst.x, dst.w, dst.h);
        /* Everything the screen rect was derived from, so a capture can be
         * held against the settings that produced it rather than against
         * literals that go stale the moment a setting is switchable. */
        const uint32_t bg = p2500_phosphor(app->phosphor)->bg;
        uint8_t lb[3];
        letterbox_rgb(app->theme, lb);
        char snapped[16];
        if (!app->snap) SDL_strlcpy(snapped, "off", sizeof snapped);
        else if (lay.snap_x) SDL_snprintf(snapped, sizeof snapped, "%dx%d",
                                          lay.snap_x, lay.snap_y);
        else SDL_strlcpy(snapped, "nofit", sizeof snapped);
        SDL_Log("display: aspect %s, snap %s, snapped %s, filter %s %dx%d,"
                " phosphor %s, theme %s, letterbox %u,%u,%u, screen-bg %u,%u,%u",
                app->aspect == ASPECT_SQUARE ? "square" : "4:3",
                app->snap ? "on" : "off", snapped,
                lay.direct_linear ? "linear" : lay.two_pass ? "two-pass" : "nearest",
                lay.pre_mx, lay.pre_my,
                p2500_phosphor_name(app->phosphor), p2500_theme_name(app->theme),
                lb[0], lb[1], lb[2],
                (bg >> 16) & 0xFF, (bg >> 8) & 0xFF, bg & 0xFF);
        if (app->panels.goto_geom.field_x >= 0.0f)
            SDL_Log("memory panel: goto field %.0f,%.0f button %.0f,%.0f;"
                    " watch field %.0f,%.0f button %.0f,%.0f",
                    app->panels.goto_geom.field_x, app->panels.goto_geom.field_y,
                    app->panels.goto_geom.button_x, app->panels.goto_geom.button_y,
                    app->panels.watch_geom.field_x, app->panels.watch_geom.field_y,
                    app->panels.watch_geom.button_x, app->panels.watch_geom.button_y);
        if (app->panels.rewind_geom.button_x >= 0.0f)
            SDL_Log("disasm panel: step-back button %.0f,%.0f;"
                    " back-to-break button %.0f,%.0f",
                    app->panels.rewind_geom.button_x, app->panels.rewind_geom.button_y,
                    app->panels.rewind_geom.field_x, app->panels.rewind_geom.field_y);
    }

    uint8_t lbox[3];
    letterbox_rgb(app->theme, lbox);
    SDL_SetRenderDrawColor(app->renderer, lbox[0], lbox[1], lbox[2], 255);
    SDL_RenderClear(app->renderer);
    /* The scale mode is set every frame rather than once at creation: which
     * filter belongs on the screen texture depends on the layout, and the
     * layout changes with the window. */
    SDL_SetTextureScaleMode(app->screen, lay.direct_linear ? SDL_SCALEMODE_LINEAR
                                                           : SDL_SCALEMODE_NEAREST);
    if (lay.two_pass && !lay.direct_linear &&
        ensure_prescale(app, lay.pre_mx, lay.pre_my)) {
        SDL_SetRenderTarget(app->renderer, app->prescale);
        SDL_RenderTexture(app->renderer, app->screen, NULL, NULL);
        SDL_SetRenderTarget(app->renderer, NULL);
        SDL_RenderTexture(app->renderer, app->prescale, NULL, &dst);
    } else {
        SDL_RenderTexture(app->renderer, app->screen, NULL, &dst);
    }
    if (app->has_ui)
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), app->renderer);
    SDL_RenderPresent(app->renderer);

    /* On change, not at exit: a crash must not cost a setting, and the file
     * is a few hundred bytes. */
    window_settle(app);
    prefs_sync(app);

    pace_field(app);

    if (app->quit) return SDL_APP_SUCCESS;

    /* Headless self-check: run a fixed number of fields, save what is on
     * screen, and exit. This is what lets the front-end be tested with no
     * display - see tests/run_tests.sh. */
    if (app->frame_limit && app->frames >= app->frame_limit) {
        if (app->shot_path) write_ppm(app);
        if (app->win_shot_path) write_window_ppm(app);
        return SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult)
{
    App *app = (App *)appstate;
    if (!app) return;
    if (app->frames && app->start_ns) {
        const double secs = (double)(SDL_GetTicksNS() - app->start_ns) / 1e9;
        if (secs > 0.0) {
            SDL_Log("presented %lu frames in %.2f s (%.1f frames/s; the machine's "
                    "field rate is %u)", app->frames, secs,
                    (double)app->frames / secs, P2500_CLOCK_TICK_HZ);
            /* The line that says whether the speed setting was honoured.
             * Emulated time comes from the CPU's T-state count, so this is
             * the real ratio and not the one that was asked for. */
            const double emulated = (double)app->m.cpu.cyc / (double)P2500_CPU_HZ;
            SDL_Log("emulated %.3f s of machine time in %.2f s of wall clock (%.2fx)",
                    emulated, secs, emulated / secs);
        }
    }
    if (app->has_ui) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (app->screen) SDL_DestroyTexture(app->screen);
    if (app->renderer) SDL_DestroyRenderer(app->renderer);
    if (app->window) SDL_DestroyWindow(app->window);
    free(app->fb);
    for (unsigned u = 0; u < P2500_FDC_MAX_DRIVES; u++) free(app->disk[u]);
    free(app->cart_buf);
    p2500_history_destroy(app->hist);
    delete app;
}
