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
 * is the same event as the machine's own 50 Hz clock strobe. The frame loop
 * and the guest's real-time clock are therefore the same clock by
 * construction, not by tuning.
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
#include "core/video.h"

#define SCALE_DEFAULT 2
#define MAX_PUSHES 8

typedef struct {
    P2500Machine m;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *screen;
    P2500VideoInfo info;
    uint32_t *fb;
    uint8_t *disk;
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
    bool turbo;
    bool quit;
    bool has_ui;                 /* false only if ImGui failed to initialise */
    float menu_h;                /* measured each frame, offsets the screen */
    SDL_AtomicInt disk_pending;  /* set by the file-dialog callback */
    char disk_path[1024];
    char status[160];            /* last action, shown at the right of the bar */
} App;

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

static void mount_disk(App *app, const char *path)
{
    size_t n = 0;
    uint8_t *buf = read_whole_file(path, &n);
    if (!buf) { set_status(app, "could not read %s", path); return; }
    free(app->disk);
    app->disk = buf;
    app->m.fdc.disk = buf;
    app->m.fdc.disk_size = n;
    const char *base = SDL_strrchr(path, '/');
    set_status(app, "mounted %s - Ctrl-C at the prompt, or Reset",
               base ? base + 1 : path);
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

    c[ImGuiCol_Text]           = green;
    c[ImGuiCol_TextDisabled]   = green_dim;
    c[ImGuiCol_WindowBg]       = bezel;
    c[ImGuiCol_PopupBg]        = bezel;
    c[ImGuiCol_MenuBarBg]      = bar;
    c[ImGuiCol_Border]         = ImVec4(0.12f, 0.35f, 0.08f, 1.00f);
    c[ImGuiCol_BorderShadow]   = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_Header]         = ImVec4(0.16f, 0.55f, 0.00f, 0.55f);
    c[ImGuiCol_HeaderHovered]  = ImVec4(0.27f, 1.00f, 0.00f, 0.35f);
    c[ImGuiCol_HeaderActive]   = ImVec4(0.27f, 1.00f, 0.00f, 0.55f);
    c[ImGuiCol_FrameBg]        = ImVec4(0.08f, 0.16f, 0.08f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.28f, 0.10f, 1.00f);
    c[ImGuiCol_FrameBgActive]  = ImVec4(0.16f, 0.40f, 0.12f, 1.00f);
    c[ImGuiCol_Separator]      = ImVec4(0.12f, 0.35f, 0.08f, 1.00f);
    c[ImGuiCol_CheckMark]      = green;
}

static void draw_menu_bar(App *app)
{
    if (!ImGui::BeginMainMenuBar()) return;
    app->menu_h = ImGui::GetWindowSize().y;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Load Disk...", "Ctrl+O")) {
            static const SDL_DialogFileFilter filters[] = {
                { "Disk images", "raw;img;dsk;imd" },
                { "All files", "*" },
            };
            SDL_ShowOpenFileDialog(disk_chosen, app, app->window,
                                   filters, SDL_arraysize(filters), NULL, false);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset", "Ctrl+R")) {
            p2500_reset(&app->m);
            set_status(app, "reset - media still attached");
        }
        if (ImGui::MenuItem(app->paused ? "Unpause" : "Pause", "F12")) {
            app->paused = !app->paused;
            set_status(app, app->paused ? "paused" : "running");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Screenshot", "F10")) save_screenshot(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) app->quit = true;
        ImGui::EndMenu();
    }

    /* Status, right-aligned. Keeps the bar useful rather than decorative. */
    if (app->status[0]) {
        float w = ImGui::CalcTextSize(app->status).x;
        float avail = ImGui::GetWindowWidth();
        if (avail - w - 12.0f > ImGui::GetCursorPosX()) {
            ImGui::SetCursorPosX(avail - w - 12.0f);
            ImGui::TextDisabled("%s", app->status);
        }
    }
    ImGui::EndMainMenuBar();
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
    const char *disk = NULL;
    int scale = SCALE_DEFAULT;
    unsigned long frame_limit = 0;
    const char *shot_path = NULL;
    const char *win_shot_path = NULL;
    unsigned long push_at[MAX_PUSHES] = {0};
    const char *push_text[MAX_PUSHES] = {0};
    int npush = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
        else if (!strcmp(argv[i], "--charrom") && i + 1 < argc) charrom = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frame_limit = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot_path = argv[++i];
        else if (!strcmp(argv[i], "--shot-window") && i + 1 < argc) win_shot_path = argv[++i];
        else if (!strcmp(argv[i], "--push-at") && i + 1 < argc && npush < MAX_PUSHES) {
            char *arg = argv[++i], *colon = strchr(arg, ':');
            if (!colon) { SDL_Log("bad --push-at, want MS:STRING"); return SDL_APP_FAILURE; }
            *colon = '\0';
            push_at[npush] = strtoul(arg, NULL, 0);
            push_text[npush] = colon + 1;
            npush++;
        }
        else {
            SDL_Log("usage: p2500-gui [--rom path] [--charrom path] [--disk path] [--scale N]\n"
                    "                 [--frames N] [--screenshot out.ppm] [--shot-window out.ppm]\n"
                    "                 [--push-at MS:STRING ...]");
            return SDL_APP_FAILURE;
        }
    }
    if (scale < 1 || scale > 8) scale = SCALE_DEFAULT;

    App *app = (App *)calloc(1, sizeof(App));
    if (!app) return SDL_APP_FAILURE;
    *appstate = app;

    app->frame_limit = frame_limit;
    app->shot_path = shot_path;
    app->win_shot_path = win_shot_path;
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
    if (disk) {
        size_t n = 0;
        app->disk = read_whole_file(disk, &n);
        if (!app->disk) {
            SDL_Log("failed to load disk image %s", disk);
            return SDL_APP_FAILURE;
        }
        app->m.fdc.disk = app->disk;
        app->m.fdc.disk_size = n;
        SDL_Log("attached %s (%zu bytes)", disk, n);
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

    SDL_StartTextInput(app->window);
    set_status(app, disk ? "ready" : "no disk - File > Load Disk...");
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
        for (const char *c = event->text.text; *c; c++)
            if ((unsigned char)*c >= 0x20 && (unsigned char)*c < 0x7F)
                p2500_keyboard_push(&app->m.keyboard, (uint8_t)*c);
        return SDL_APP_CONTINUE;
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode k = event->key.key;
        const bool ctrl = (event->key.mod & SDL_KMOD_CTRL) != 0;
        /* Menu accelerators win over the guest, which is why Ctrl-C still
         * reaches CP/M: it is deliberately not one of them. */
        if (ctrl && k == SDLK_O) { /* handled via the menu's dialog call */
            static const SDL_DialogFileFilter filters[] = {
                { "Disk images", "raw;img;dsk;imd" }, { "All files", "*" },
            };
            SDL_ShowOpenFileDialog(disk_chosen, app, app->window,
                                   filters, SDL_arraysize(filters), NULL, false);
            return SDL_APP_CONTINUE;
        }
        if (ctrl && k == SDLK_R) { p2500_reset(&app->m); set_status(app, "reset - media still attached"); return SDL_APP_CONTINUE; }
        if (ctrl && k == SDLK_Q) return SDL_APP_SUCCESS;
        if (k == SDLK_F10) { save_screenshot(app); return SDL_APP_CONTINUE; }
        if (k == SDLK_F12) { app->paused = !app->paused; set_status(app, app->paused ? "paused" : "running"); return SDL_APP_CONTINUE; }
        if (k == SDLK_F11) { app->turbo = !app->turbo; set_status(app, app->turbo ? "turbo" : "running"); return SDL_APP_CONTINUE; }
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

    if (SDL_GetAtomicInt(&app->disk_pending)) {
        SDL_SetAtomicInt(&app->disk_pending, 0);
        mount_disk(app, app->disk_path);
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
        unsigned long budget = P2500_TSTATES_PER_FRAME;
        if (app->turbo) budget *= 8;
        p2500_run_tstates(&app->m, budget);
    }
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
        ImGui::NewFrame();
        draw_menu_bar(app);
        ImGui::Render();
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

    SDL_SetRenderDrawColor(app->renderer, 8, 12, 8, 255);
    SDL_RenderClear(app->renderer);
    SDL_RenderTexture(app->renderer, app->screen, NULL, &dst);
    if (app->has_ui)
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), app->renderer);
    SDL_RenderPresent(app->renderer);

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
    if (app->has_ui) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (app->screen) SDL_DestroyTexture(app->screen);
    if (app->renderer) SDL_DestroyRenderer(app->renderer);
    if (app->window) SDL_DestroyWindow(app->window);
    free(app->fb);
    free(app->disk);
    free(app);
}
