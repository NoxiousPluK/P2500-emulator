/*
 * p2500-gui - SDL3 + Dear ImGui front-end (TODO.md T36-T39).
 *
 * The only C++ in the tree, and it stays that way: libp2500.a is C11 with
 * no dependencies and nothing outside src/gui/ may include ImGui.
 *
 * Uses SDL3's callback app model (SDL_AppInit/Iterate/Event) rather than a
 * hand-rolled main loop: it is the shape SDL3 is designed around and it is
 * what makes an Emscripten build (T40) nearly free.
 *
 * Single-threaded on purpose. The machine runs about ten times faster than
 * real time, so one frame's worth of emulation per presented frame has
 * comfortable headroom, and one thread is what lets the debugger panels be
 * written without a single lock.
 *
 * Pacing (T35): one video field is P2500_TSTATES_PER_FRAME T-states, which
 * is the same event as the machine's own 50 Hz clock strobe. That makes one
 * field per iteration the right unit of work - but it says nothing about
 * how often an iteration happens, and SDL_AppIterate is called as fast as
 * the host allows. Without a throttle the guest ran at whatever rate that
 * was, tens of times too fast; the comment that used to sit here claimed
 * the two clocks matched "by construction" and was simply wrong. They match
 * because pace_field() makes them, against the wall clock - so the speed
 * does not depend on the monitor's refresh rate either.
 *
 * The speed setting (T54) does not change that period. It changes how much
 * emulated time one iteration covers: half a field at 0.5x, two fields at
 * 2x. Keeping the period fixed is what holds the picture at 50 Hz however
 * slowly the guest is running, and it is also the only way up - the
 * presented frame rate is capped by vsync, so a 4x speed cannot come from
 * presenting four times as often.
 *
 * The menu bar is drawn by ImGui rather than being a native one. See
 * TODO.md T39 for why: the native options cover Windows and macOS, and on
 * Wayland there is no way to attach a GTK menu to an SDL window nor a
 * global menu to export to.
 */

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#include "core/machine.h"
#include "core/debug.h"
#include "core/video.h"

#include "panels.h"

#define SCALE_DEFAULT 2
#define MAX_PUSHES 8

/* Named rather than an anonymous typedef: it holds C++ members with
 * default initialisers now, which an anonymous struct cannot carry across a
 * typedef without tripping -Wnon-c-typedef-for-linkage. */
struct App {
    P2500Machine m;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *screen;
    P2500VideoInfo info;
    uint32_t *fb;
    uint8_t *disk[P2500_FDC_MAX_DRIVES];
    char disk_name[P2500_FDC_MAX_DRIVES][64]; /* basename, for the drive lamps */
    unsigned long frames;
    unsigned long frame_limit;   /* 0 = run until the user quits */
    const char *shot_path;       /* emulated screen only, for the CLI comparison */
    const char *win_shot_path;   /* the whole composited window, UI included */
    /* Scripted keystrokes, identical in effect to a real keypress - they go
     * through p2500_keyboard_push like SDL_EVENT_TEXT_INPUT does. Present so
     * the front-end can be driven to a real application with no display,
     * which is what makes a GUI-vs-CLI render comparison possible. */
    struct { unsigned long at_ms; const char *text; bool done; } push[MAX_PUSHES];
    int pushes;
    bool paused;
    /* Speed (T54): a multiplier on how fast emulated time runs against the
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
    /* Watches, counters and breakpoints, shared with the CLI (TODO.md T39).
     * Empty by default, which is what lets the run loop keep its
     * whole-frame fast path until the user actually asks for something. */
    P2500Debug dbg;
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
    int watches_was = 0;
    Uint64 next_field_ns;        /* when the next 50 Hz field is due */
    Uint64 start_ns;
    bool force_pacing;           /* --paced: pace even a scripted run */
    SDL_AtomicInt disk_pending;  /* set by the file-dialog callback */
    char disk_path[1024];
    unsigned pending_unit;       /* which drive the dialog was opened for */
    char status[160];            /* last action, shown at the right of the bar */
};

static const P2500Palette PALETTE = {
    0xFF46FF00u,     /* fg: green phosphor, matching tools/render_vram.py */
    0xFF080C08u,     /* bg */
    0xFF288C00u,     /* fg_dim */
};

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
 * Keyboard encoding, per TODO.md T38. Printable characters come from
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
    free(app->disk[unit]);
    app->disk[unit] = buf;
    p2500_fdc_attach(&app->m.fdc, unit, buf, n);
    const char *base = SDL_strrchr(path, '/');
    SDL_strlcpy(app->disk_name[unit], base ? base + 1 : path, sizeof app->disk_name[unit]);
    set_status(app, "%c: %s", 'A' + (int)unit, app->disk_name[unit]);
}

static void eject_disk(App *app, unsigned unit)
{
    if (unit >= P2500_FDC_MAX_DRIVES || !app->disk[unit]) return;
    p2500_fdc_attach(&app->m.fdc, unit, NULL, 0);
    free(app->disk[unit]);
    app->disk[unit] = NULL;
    app->disk_name[unit][0] = '\0';
    set_status(app, "%c: empty", 'A' + (int)unit);
    SDL_Log("drive %c: ejected", 'A' + (int)unit);
}

/* One place, because three things reach it: the File menu, F12, and the
 * run/pause lamp. Resuming clears any breakpoint stop, or Run would trip
 * straight back over the address it is standing on. */
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

/* CBIOS supports A:, B: and C: (TODO.md T44), so the dialog is opened per
 * drive and the chosen unit travels with the pending path. */
static void open_disk_dialog(App *app, unsigned unit)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Disk images", "raw;img;dsk;imd" },
        { "All files", "*" },
    };
    app->pending_unit = unit;
    SDL_ShowOpenFileDialog(disk_chosen, app, app->window,
                           filters, SDL_arraysize(filters), NULL, false);
}

/* Deliberately not the default ImGui look: square, dark, and on the same
 * phosphor palette as the emulated screen, so the bar reads as part of the
 * machine's bezel rather than as a debug overlay. */
static void apply_style()
{
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = st.ChildRounding = st.FrameRounding = 0.0f;
    st.PopupRounding = st.ScrollbarRounding = st.GrabRounding = st.TabRounding = 0.0f;
    st.WindowBorderSize = st.PopupBorderSize = 1.0f;
    st.FrameBorderSize = 0.0f;
    st.FramePadding = ImVec2(10.0f, 5.0f);
    st.ItemSpacing = ImVec2(12.0f, 6.0f);
    st.WindowPadding = ImVec2(8.0f, 8.0f);

    ImVec4 *c = st.Colors;
    const ImVec4 green      = ImVec4(0.27f, 1.00f, 0.00f, 1.00f);
    const ImVec4 green_dim  = ImVec4(0.16f, 0.55f, 0.00f, 1.00f);
    const ImVec4 bezel      = ImVec4(0.031f, 0.047f, 0.031f, 1.00f);
    const ImVec4 bar        = ImVec4(0.055f, 0.086f, 0.055f, 1.00f);
    const ImVec4 edge       = ImVec4(0.12f, 0.35f, 0.08f, 1.00f);

    c[ImGuiCol_Text]            = green;
    c[ImGuiCol_TextDisabled]    = green_dim;
    c[ImGuiCol_WindowBg]        = bezel;
    c[ImGuiCol_ChildBg]         = ImVec4(0.02f, 0.031f, 0.02f, 1.00f);
    c[ImGuiCol_PopupBg]         = bezel;
    c[ImGuiCol_MenuBarBg]       = bar;
    c[ImGuiCol_Border]          = edge;
    c[ImGuiCol_BorderShadow]    = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    /* Section bands, not highlights: bright enough to group, dark enough
     * that the green text on top of them stays the brightest thing. */
    c[ImGuiCol_Header]          = ImVec4(0.08f, 0.22f, 0.06f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.13f, 0.34f, 0.09f, 1.00f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.18f, 0.46f, 0.12f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.08f, 0.16f, 0.08f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.12f, 0.28f, 0.10f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.16f, 0.40f, 0.12f, 1.00f);
    c[ImGuiCol_Separator]       = edge;
    c[ImGuiCol_SeparatorHovered]= ImVec4(0.20f, 0.60f, 0.14f, 1.00f);
    c[ImGuiCol_SeparatorActive] = green;
    c[ImGuiCol_CheckMark]       = green;
    /* The debugger panels are ordinary ImGui windows, so every remaining
     * default - title bars, buttons, scrollbars, tabs - would arrive in
     * ImGui's blue. Set the whole palette rather than the handful the menu
     * bar happened to need. */
    c[ImGuiCol_TitleBg]         = ImVec4(0.043f, 0.071f, 0.043f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.075f, 0.16f, 0.06f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]= ImVec4(0.043f, 0.071f, 0.043f, 0.85f);
    c[ImGuiCol_Button]          = ImVec4(0.09f, 0.22f, 0.07f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.14f, 0.36f, 0.10f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.20f, 0.52f, 0.14f, 1.00f);
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0.02f, 0.031f, 0.02f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.10f, 0.26f, 0.08f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.16f, 0.40f, 0.12f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.22f, 0.58f, 0.16f, 1.00f);
    c[ImGuiCol_SliderGrab]      = green_dim;
    c[ImGuiCol_SliderGrabActive]= green;
    c[ImGuiCol_ResizeGrip]      = ImVec4(0.12f, 0.35f, 0.08f, 0.60f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.20f, 0.60f, 0.14f, 0.80f);
    c[ImGuiCol_ResizeGripActive]  = green;
    c[ImGuiCol_Tab]             = ImVec4(0.06f, 0.13f, 0.05f, 1.00f);
    c[ImGuiCol_TabHovered]      = ImVec4(0.16f, 0.40f, 0.12f, 1.00f);
    c[ImGuiCol_TableHeaderBg]   = ImVec4(0.06f, 0.13f, 0.05f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = edge;
    c[ImGuiCol_TableBorderLight]  = ImVec4(0.08f, 0.20f, 0.06f, 1.00f);
    c[ImGuiCol_TableRowBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]   = ImVec4(0.27f, 1.00f, 0.00f, 0.035f);
    c[ImGuiCol_NavCursor]       = green;
    c[ImGuiCol_DragDropTarget]  = green;
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

/* The dim half of every indicator: drawn, not hidden, so nothing in the bar
 * ever moves and each position always means the same thing. */
static ImU32 indicator_colour(bool lit)
{
    if (lit) return ImGui::GetColorU32(ImGuiCol_Text);
    ImVec4 off = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    off.w = 0.45f;
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
static ImU32 lamp_paint(ImDrawList *dl, const Lamp &l, bool lit)
{
    if (!l.hovered) return indicator_colour(lit);
    ImVec4 ground = ImGui::GetStyleColorVec4(lit ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    dl->AddRectFilled(l.min, l.max, ImGui::GetColorU32(ground));
    return ImGui::GetColorU32(ImGuiCol_MenuBarBg);
}

static void draw_menu_bar(App *app)
{
    if (!ImGui::BeginMainMenuBar()) return;
    app->menu_h = ImGui::GetWindowSize().y;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Load Disk A...", "Ctrl+O")) open_disk_dialog(app, 0);
        if (ImGui::MenuItem("Load Disk B...")) open_disk_dialog(app, 1);
        if (ImGui::MenuItem("Load Disk C...")) open_disk_dialog(app, 2);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset", "Ctrl+R")) {
            p2500_reset(&app->m);
            p2500_debug_baseline(&app->dbg, &app->m);
            set_status(app, "reset - media still attached");
        }
        if (ImGui::MenuItem(app->paused ? "Unpause" : "Pause", "F12")) toggle_pause(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Screenshot", "F10")) save_screenshot(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) app->quit = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Machine")) {
        if (ImGui::MenuItem("Capitals lock", NULL, app->caps_lock)) {
            app->caps_lock = !app->caps_lock;
            set_status(app, "capitals lock %s", app->caps_lock ? "on (as shipped)" : "off");
        }
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

    /* Right-aligned, laid out from the right edge inwards and in a fixed
     * order, so no indicator ever moves as the status text changes:
     *
     *   status text | speed | A B C | run/pause | capitals lock
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
        const float drives_w = letter_w * 3.0f + letter_gap * 2.0f;

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
                SDL_Log("lamps: caps %.0f run %.0f drives %.0f %.0f %.0f rows %.0f-%.0f"
                        " speed %.0f",
                        caps_centre, run_centre,
                        drives_left + letter_w * 0.5f,
                        drives_left + drive_cell + letter_w * 0.5f,
                        drives_left + 2 * drive_cell + letter_w * 0.5f,
                        bar_y + 2.0f, bar_y + bar_h - 2.0f, speed_centre);

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
                            lamp_paint(dl, caps, app->caps_lock));

            Lamp run = lamp_button("##run", run_centre, run_w, bar_y, bar_h);
            if (run.clicked || run.alt_clicked) toggle_pause(app);
            ImGui::SetItemTooltip("%s  (F12)", app->paused ? "Paused - click to run"
                                                           : "Running - click to pause");
            draw_run_glyph(dl, ImVec2(run_centre, mid_y), gh * 0.86f, app->paused,
                           lamp_paint(dl, run, app->paused));

            /* A:, B: and C: - the three drives CBIOS actually supports
             * (TODO.md T44). Lit means media is attached, which is not the
             * same as the guest having logged the drive in, which is why the
             * tooltip says so. Left click loads, right click ejects. */
            for (unsigned u = 0; u < 3; u++) {
                const bool loaded = app->m.fdc.disk[u] != NULL;
                const float cx = drives_left + u * drive_cell + letter_w * 0.5f;
                char id[8];
                snprintf(id, sizeof id, "##dr%u", u);
                Lamp d = lamp_button(id, cx, letter_w, bar_y, bar_h);
                if (d.clicked) open_disk_dialog(app, u);
                if (d.alt_clicked) eject_disk(app, u);
                if (loaded)
                    ImGui::SetItemTooltip("%c: %s\nClick to change, right-click to eject.\n"
                                          "A swapped-in disk is readable at once; CP/M marks"
                                          " the drive read-only until a warm boot (Ctrl-C).",
                                          'A' + (int)u, app->disk_name[u]);
                else
                    ImGui::SetItemTooltip("%c: empty\nClick to load a disk.", 'A' + (int)u);
                const ImU32 ink = lamp_paint(dl, d, loaded);
                const char letter[2] = { (char)('A' + u), '\0' };
                dl->AddText(ImVec2(cx - letter_w * 0.5f, mid_y - font_h * 0.5f), ink, letter);
            }

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
            const ImU32 sink = lamp_paint(dl, sp, app->speed != 1.0f);
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
    if (!app->dbg.breaks && !app->dbg.watches && !app->dbg.counts) {
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
    p2500_debug_resume(&app->dbg);
    for (unsigned long i = 0; i < n; i++) {
        if (p2500_debug_before_step(&app->dbg, &app->m, app->m.total_instructions)) {
            hit_stop(app);
            return;
        }
        p2500_step(&app->m);
    }
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

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    const char *rom = "roms/ipl.bin";
    const char *charrom = "roms/charrom.bin";
    const char *disks[P2500_FDC_MAX_DRIVES] = {0};
    int scale = SCALE_DEFAULT;
    unsigned long frame_limit = 0;
    const char *shot_path = NULL;
    const char *win_shot_path = NULL;
    bool caps_lock = true; /* as the disks ship */
    bool force_pacing = false;
    float speed = 1.0f;
    unsigned long push_at[MAX_PUSHES] = {0};
    const char *push_text[MAX_PUSHES] = {0};
    int npush = 0;
    const char *panels_arg = NULL;
    bool verbose_devices = false;
    struct { unsigned long at_frame; int x, y, button; } mouse_script[MAX_PUSHES];
    int mouse_steps = 0;
    struct { unsigned long at_frame; const char *text; } ui_type_script[MAX_PUSHES];
    int ui_type_steps = 0;
    uint16_t break_at[MAX_PUSHES] = {0};
    int nbreak = 0;
    uint16_t watch_at[MAX_PUSHES] = {0};
    int nwatch = 0;
    uint16_t wbreak_at[MAX_PUSHES] = {0};
    int nwbreak = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
        else if (!strcmp(argv[i], "--charrom") && i + 1 < argc) charrom = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disks[0] = argv[++i];
        else if (!strcmp(argv[i], "--disk-b") && i + 1 < argc) disks[1] = argv[++i];
        else if (!strcmp(argv[i], "--disk-c") && i + 1 < argc) disks[2] = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frame_limit = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot_path = argv[++i];
        else if (!strcmp(argv[i], "--shot-window") && i + 1 < argc) win_shot_path = argv[++i];
        else if (!strcmp(argv[i], "--no-caps-lock")) caps_lock = false;
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
        }
        /* The debug flags exist so the panels can be driven - and captured -
         * with no display, the same way --frames/--shot-window already let
         * the screen be checked headlessly. */
        else if (!strcmp(argv[i], "--panels") && i + 1 < argc) panels_arg = argv[++i];
        else if (!strcmp(argv[i], "--verbose-io")) verbose_devices = true;
        else if (!strcmp(argv[i], "--ui-type") && i + 1 < argc && ui_type_steps < MAX_PUSHES) {
            char *arg = argv[++i], *colon = strchr(arg, ':');
            if (!colon) { SDL_Log("bad --ui-type, want FRAME:TEXT"); return SDL_APP_FAILURE; }
            *colon = '\0';
            ui_type_script[ui_type_steps].at_frame = strtoul(arg, NULL, 0);
            ui_type_script[ui_type_steps].text = colon + 1;
            ui_type_steps++;
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
            SDL_Log("usage: p2500-gui [--rom path] [--charrom path] [--scale N]\n"
                    "                 [--disk path] [--disk-b path] [--disk-c path]\n"
                    "                 [--frames N] [--screenshot out.ppm] [--shot-window out.ppm]\n"
                    "                 [--push-at MS:STRING ...] [--no-caps-lock]\n"
                    "                 [--panels devices,memory,disasm,log] [--verbose-io]\n"
                    "                 [--paced] [--speed X|unlimited]\n"
                    "                 [--mouse [FRAME:]X,Y[,left|right] ...]\n"
                    "                 [--ui-type FRAME:TEXT ...]\n"
                    "                 [--break ADDR ...] [--watch ADDR ...]\n"
                    "                 [--watch-break ADDR ...]");
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
    app->force_pacing = force_pacing;
    app->speed = speed;
    app->start_ns = SDL_GetTicksNS();
    app->pushes = npush;
    for (int i = 0; i < npush; i++) {
        app->push[i].at_ms = push_at[i];
        app->push[i].text = push_text[i];
    }

    p2500_init(&app->m);
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
        p2500_fdc_attach(&app->m.fdc, u, app->disk[u], n);
        const char *base = SDL_strrchr(disks[u], '/');
        SDL_strlcpy(app->disk_name[u], base ? base + 1 : disks[u], sizeof app->disk_name[u]);
        SDL_Log("attached %c: %s (%zu bytes)", 'A' + (int)u, disks[u], n);
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    p2500_video_info(&app->m, &app->info);
    if (!SDL_CreateWindowAndRenderer("Philips P2500", app->info.width * scale,
                                     app->info.height * scale + 24,
                                     SDL_WINDOW_RESIZABLE,
                                     &app->window, &app->renderer)) {
        SDL_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
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
    apply_style();
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
    p2500_debug_baseline(&app->dbg, &app->m);
    if (panels_arg) {
        app->panels.show_devices = strstr(panels_arg, "devices") != NULL;
        app->panels.show_memory = strstr(panels_arg, "memory") != NULL;
        app->panels.show_disasm = strstr(panels_arg, "disasm") != NULL;
        app->panels.show_log = strstr(panels_arg, "log") != NULL;
    }
    app->panels.verbose_devices = verbose_devices;
    app->ui_types = ui_type_steps;
    for (int i = 0; i < ui_type_steps; i++) {
        app->ui_type[i].at_frame = ui_type_script[i].at_frame;
        app->ui_type[i].text = ui_type_script[i].text;
    }
    app->mouse_steps = mouse_steps;
    for (int i = 0; i < mouse_steps; i++) {
        app->mouse[i].at_frame = mouse_script[i].at_frame;
        app->mouse[i].x = mouse_script[i].x;
        app->mouse[i].y = mouse_script[i].y;
        app->mouse[i].button = mouse_script[i].button;
    }

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
    case SDL_EVENT_TEXT_INPUT:
        if (ui_has_keyboard) return SDL_APP_CONTINUE;
        /* One byte per character the platform produced. Anything outside
         * 7-bit ASCII needs the $E274 dead-key table decoded first (T38), so
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
        if (ctrl && k == SDLK_R) { p2500_reset(&app->m); p2500_debug_baseline(&app->dbg, &app->m); set_status(app, "reset - media still attached"); return SDL_APP_CONTINUE; }
        if (ctrl && k == SDLK_Q) return SDL_APP_SUCCESS;
        if (k == SDLK_F10) { save_screenshot(app); return SDL_APP_CONTINUE; }
        if (k == SDLK_F12) { toggle_pause(app); return SDL_APP_CONTINUE; }
        if (k == SDLK_F11) { set_speed(app, app->speed == 0.0f ? 1.0f : 0.0f); return SDL_APP_CONTINUE; }
        if (k == SDLK_F1) { app->panels.show_devices = !app->panels.show_devices; return SDL_APP_CONTINUE; }
        if (k == SDLK_F2) { app->panels.show_memory = !app->panels.show_memory; return SDL_APP_CONTINUE; }
        if (k == SDLK_F3) { app->panels.show_disasm = !app->panels.show_disasm; return SDL_APP_CONTINUE; }
        if (k == SDLK_F4) { app->panels.show_log = !app->panels.show_log; return SDL_APP_CONTINUE; }
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
        app->m.sesam.verbose = v;
        app->m.keyboard.verbose = v;
        app->m.intctl.verbose = v;
    }

    if (SDL_GetAtomicInt(&app->disk_pending)) {
        SDL_SetAtomicInt(&app->disk_pending, 0);
        mount_disk(app, app->pending_unit, app->disk_path);
    }

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

    /* The CRTC can be reprogrammed by the guest - the manual's graphics mode
     * reinitialises it - so re-check the geometry rather than trusting the
     * size the window was created with. */
    P2500VideoInfo now;
    p2500_video_info(&app->m, &now);
    if (now.width != app->info.width || now.height != app->info.height)
        if (!make_screen_texture(app)) return SDL_APP_FAILURE;

    /* ~3 Hz blink, which is the usual rate for a text cursor. */
    bool blink_on = ((app->frames / 8) & 1) == 0;
    p2500_video_render(&app->m, &PALETTE, blink_on, app->fb, app->info.width);
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
        /* Re-arm the guest's keyboard.
         *
         * ImGui's SDL3 backend calls SDL_StopTextInput() when one of its
         * text fields loses focus (imgui_impl_sdl3.cpp, PlatformSetImeData),
         * and that switches SDL_EVENT_TEXT_INPUT off for the whole window.
         * Nothing turns it back on: as far as the backend is concerned
         * nobody wants text any more. But the guest always does - that event
         * is where every printable key it receives comes from - so one use
         * of any debugger address field left the machine untypeable until
         * restart. Key events were unaffected, which is why the menus, the
         * F-keys and the cursor keys went on working and hid it.
         *
         * Checked every frame rather than on a transition: the backend can
         * stop text input at moments we do not model, and asking SDL what
         * the state actually is costs nothing. */
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
        /* Stepping runs after the frame is composed, so the listing the user
         * clicked is the state the step started from. ~0 means "one 50 Hz
         * field", the same budget the free-running loop uses. */
        if (act.steps == ~0UL) {
            p2500_debug_resume(&app->dbg);
            run_machine(app, P2500_TSTATES_PER_FRAME);
        } else if (act.steps) {
            step_instructions(app, act.steps);
        }
    }

    /* Integer-scale the screen into whatever is left below the menu bar and
     * centre it. Done by hand rather than with SDL_SetRenderLogicalPresentation
     * because that would scale the UI too. */
    int win_w = 0, win_h = 0;
    SDL_GetRenderOutputSize(app->renderer, &win_w, &win_h);
    const int top = (int)(app->menu_h + 0.5f);
    const int avail_h = win_h - top;
    int scale = 1;
    if (app->info.width > 0 && app->info.height > 0 && avail_h > 0) {
        int sx = win_w / app->info.width;
        int sy = avail_h / app->info.height;
        scale = sx < sy ? sx : sy;
        if (scale < 1) scale = 1;
    }
    SDL_FRect dst;
    dst.w = (float)(app->info.width * scale);
    dst.h = (float)(app->info.height * scale);
    dst.x = ((float)win_w - dst.w) * 0.5f;
    dst.y = (float)top + ((float)avail_h - dst.h) * 0.5f;
    if (dst.y < (float)top) dst.y = (float)top;

    /* Reported so a headless run can assert the layout directly. Inferring
     * the offset from the captured pixels does not work: the screen's own
     * background and the window's clear colour are the same, and the top
     * text rows of a character cell are blank, so a screen drawn at y=0
     * looks identical to a correctly offset one in a screenshot. */
    if (app->win_shot_path && app->frames == 2) {
        SDL_Log("layout: menu %d px, screen at y=%.0f, %.0fx%.0f",
                top, dst.y, dst.w, dst.h);
        if (app->panels.goto_geom.field_x >= 0.0f)
            SDL_Log("memory panel: goto field %.0f,%.0f button %.0f,%.0f;"
                    " watch field %.0f,%.0f button %.0f,%.0f",
                    app->panels.goto_geom.field_x, app->panels.goto_geom.field_y,
                    app->panels.goto_geom.button_x, app->panels.goto_geom.button_y,
                    app->panels.watch_geom.field_x, app->panels.watch_geom.field_y,
                    app->panels.watch_geom.button_x, app->panels.watch_geom.button_y);
    }

    SDL_SetRenderDrawColor(app->renderer, 8, 12, 8, 255);
    SDL_RenderClear(app->renderer);
    SDL_RenderTexture(app->renderer, app->screen, NULL, &dst);
    if (app->has_ui)
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), app->renderer);
    SDL_RenderPresent(app->renderer);

    pace_field(app);

    if (app->quit) return SDL_APP_SUCCESS;

    /* Headless self-check: run a fixed number of fields, save what is on
     * screen, and exit. This is what lets the front-end be tested with no
     * display - see tools/run_tests.sh. */
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
    delete app;
}
