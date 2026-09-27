/*
 * p2500-gui - SDL3 front-end (TODO.md T36/T37/T38).
 *
 * Uses SDL3's callback app model (SDL_AppInit/Iterate/Event) rather than a
 * hand-rolled main loop: it is the shape SDL3 is designed around and it is
 * what makes an Emscripten build (T40) nearly free.
 *
 * Single-threaded on purpose. The machine runs about ten times faster than
 * real time, so one frame's worth of emulation per presented frame has
 * comfortable headroom, and one thread is what will let T39's debugger
 * panels be written without a single lock.
 *
 * Pacing (T35): one video field is P2500_TSTATES_PER_FRAME T-states, which
 * is the same event as the machine's own 50 Hz clock strobe. The frame loop
 * and the guest's real-time clock are therefore the same clock by
 * construction, not by tuning.
 */

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/machine.h"
#include "core/video.h"

#define SCALE_DEFAULT 2

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
    const char *shot_path;       /* write a PPM and exit, for headless checks */
    bool paused;
    bool turbo;
} App;

static const P2500Palette PALETTE = {
    .fg = 0xFF46FF00u,     /* green phosphor, matching tools/render_vram.py */
    .bg = 0xFF080C08u,
    .fg_dim = 0xFF288C00u,
};

static uint8_t *read_whole_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n);
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
static int key_to_byte(SDL_Keycode key, SDL_Keymod mod) {
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

static void write_ppm(const App *app) {
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

static bool make_screen_texture(App *app) {
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
    app->fb = calloc((size_t)app->info.width * app->info.height, sizeof(uint32_t));
    return app->fb != NULL;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    const char *rom = "roms/ipl.bin";
    const char *charrom = "roms/charrom.bin";
    const char *disk = NULL;
    int scale = SCALE_DEFAULT;
    unsigned long frame_limit = 0;
    const char *shot_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
        else if (!strcmp(argv[i], "--charrom") && i + 1 < argc) charrom = argv[++i];
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frame_limit = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot_path = argv[++i];
        else {
            SDL_Log("usage: p2500-gui [--rom path] [--charrom path] [--disk path] [--scale N]\n"
                    "                 [--frames N] [--screenshot out.ppm]");
            return SDL_APP_FAILURE;
        }
    }
    if (scale < 1 || scale > 8) scale = SCALE_DEFAULT;

    App *app = calloc(1, sizeof(App));
    if (!app) return SDL_APP_FAILURE;
    *appstate = app;

    app->frame_limit = frame_limit;
    app->shot_path = shot_path;
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
                                     app->info.height * scale,
                                     SDL_WINDOW_RESIZABLE,
                                     &app->window, &app->renderer)) {
        SDL_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    /* Letterbox rather than stretch, so the 8x12 cell stays square-ish
     * whatever the user does to the window. */
    SDL_SetRenderLogicalPresentation(app->renderer, app->info.width, app->info.height,
                                     SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    if (!make_screen_texture(app)) return SDL_APP_FAILURE;
    SDL_StartTextInput(app->window);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    App *app = appstate;
    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_TEXT_INPUT:
        /* One byte per character the platform produced. Anything outside
         * 7-bit ASCII needs the $E274 dead-key table decoded first (T38), so
         * it is dropped rather than guessed at. */
        for (const char *c = event->text.text; *c; c++)
            if ((unsigned char)*c >= 0x20 && (unsigned char)*c < 0x7F)
                p2500_keyboard_push(&app->m.keyboard, (uint8_t)*c);
        return SDL_APP_CONTINUE;
    case SDL_EVENT_KEY_DOWN: {
        if (event->key.key == SDLK_F12) { app->paused = !app->paused; return SDL_APP_CONTINUE; }
        if (event->key.key == SDLK_F11) { app->turbo = !app->turbo; return SDL_APP_CONTINUE; }
        int b = key_to_byte(event->key.key, event->key.mod);
        if (b >= 0) p2500_keyboard_push(&app->m.keyboard, (uint8_t)b);
        return SDL_APP_CONTINUE;
    }
    default:
        return SDL_APP_CONTINUE;
    }
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    App *app = appstate;

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
    if (now.width != app->info.width || now.height != app->info.height) {
        if (!make_screen_texture(app)) return SDL_APP_FAILURE;
        SDL_SetRenderLogicalPresentation(app->renderer, app->info.width, app->info.height,
                                         SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    }

    /* ~3 Hz blink, which is the usual rate for a text cursor. */
    bool blink_on = ((app->frames / 8) & 1) == 0;
    p2500_video_render(&app->m, &PALETTE, blink_on, app->fb, app->info.width);

    SDL_UpdateTexture(app->screen, NULL, app->fb,
                      app->info.width * (int)sizeof(uint32_t));
    SDL_RenderClear(app->renderer);
    SDL_RenderTexture(app->renderer, app->screen, NULL, NULL);
    SDL_RenderPresent(app->renderer);

    /* Headless self-check: run a fixed number of fields, save what is on
     * screen, and exit. This is what lets the front-end be tested with no
     * display - see tools/run_tests.sh. */
    if (app->frame_limit && app->frames >= app->frame_limit) {
        if (app->shot_path) write_ppm(app);
        return SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    (void)result;
    App *app = appstate;
    if (!app) return;
    if (app->screen) SDL_DestroyTexture(app->screen);
    if (app->renderer) SDL_DestroyRenderer(app->renderer);
    if (app->window) SDL_DestroyWindow(app->window);
    free(app->fb);
    free(app->disk);
    free(app);
}
