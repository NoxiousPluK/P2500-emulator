#include "icon.h"

#include <SDL3/SDL.h>

#include "icon_data.h"

/*
 * SDL3 core decodes no image formats, so the icon arrives as the 1-bit
 * bitmap tools/mk_icon.py generated and is expanded here. Two colours is
 * all the artwork uses - see icon_data.h.
 */
void p2500_set_window_icon(SDL_Window *window) {
    static const Uint8 fg[3] = {P2500_ICON_FG};
    static const Uint8 bg[3] = {P2500_ICON_BG};
    const int w = P2500_ICON_WIDTH, h = P2500_ICON_HEIGHT;
    const int stride = (w + 7) / 8;

    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!s) {
        SDL_Log("window icon: SDL_CreateSurface: %s", SDL_GetError());
        return;
    }
    if (SDL_MUSTLOCK(s) && !SDL_LockSurface(s)) {
        SDL_Log("window icon: SDL_LockSurface: %s", SDL_GetError());
        SDL_DestroySurface(s);
        return;
    }
    for (int y = 0; y < h; y++) {
        Uint8 *row = (Uint8 *)s->pixels + (size_t)y * s->pitch;
        for (int x = 0; x < w; x++) {
            bool set = p2500_icon_bits[y * stride + (x >> 3)] & (0x80 >> (x & 7));
            const Uint8 *c = set ? fg : bg;
            row[x * 4 + 0] = c[0];
            row[x * 4 + 1] = c[1];
            row[x * 4 + 2] = c[2];
            row[x * 4 + 3] = 0xFF; /* the artwork is opaque: a black screen
                                    * with white phosphor, not a cut-out */
        }
    }
    if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);

    /* Debug level, not a warning: the usual reason this fails is a video
     * driver with no concept of a window icon (the dummy driver the test
     * suite runs under says "not supported"), which is not a problem and
     * should not print on every headless run. Nothing downstream depends
     * on the icon either way. */
    if (!SDL_SetWindowIcon(window, s))
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "window icon not set: %s", SDL_GetError());
    SDL_DestroySurface(s);
}
