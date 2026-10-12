/* Does SDL_SCALEMODE_PIXELART behave differently from NEAREST and LINEAR in
 * the *software* renderer? The emulator's whole GUI test suite runs under
 * SDL_VIDEODRIVER=dummy, which is the software renderer, so if PIXELART
 * degrades there then headless captures stop matching what a user sees on a
 * GPU - unacceptable in a project that asserts on pixels.
 *
 * Draws a 4x4 checkerboard scaled to 15x15 (deliberately non-integer) under
 * each scale mode and reports how many distinct grey levels come out:
 *   NEAREST  -> 2 (hard edges, uneven cell widths)
 *   LINEAR   -> many (blurred)
 *   PIXELART -> a few (sharp bilinear: mostly hard, one blended row/col)
 * Identical counts for NEAREST and PIXELART means PIXELART is not implemented
 * on this renderer.
 *
 * Build: cc tools/sdl_scalemode_probe.c $(pkg-config --cflags --libs sdl3) -o /tmp/probe
 */
#include <SDL3/SDL.h>
#include <stdio.h>

static void report(const char *name, SDL_Renderer *r, SDL_Texture *src, int dst)
{
    SDL_SetRenderTarget(r, NULL);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    if (!SDL_SetTextureScaleMode(src, name[0] == 'N' ? SDL_SCALEMODE_NEAREST
                                   : name[0] == 'L' ? SDL_SCALEMODE_LINEAR
                                   : name[0] == '2' ? SDL_SCALEMODE_LINEAR
                                   : SDL_SCALEMODE_PIXELART)) {
        printf("%-9s SetTextureScaleMode failed: %s\n", name, SDL_GetError());
        return;
    }
    SDL_FRect d = { 0, 0, (float)dst, (float)dst };
    SDL_RenderTexture(r, src, NULL, &d);
    SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
    if (!s) { printf("%-9s ReadPixels failed: %s\n", name, SDL_GetError()); return; }
    SDL_Surface *rgb = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGB24);
    SDL_DestroySurface(s);
    int seen[256] = {0}, levels = 0;
    for (int y = 0; y < dst; y++)
        for (int x = 0; x < dst; x++) {
            unsigned char v = ((unsigned char *)rgb->pixels)[y * rgb->pitch + x * 3];
            if (!seen[v]++) levels++;
        }
    printf("%-9s %2d distinct grey levels in the %dx%d result:", name, levels, dst, dst);
    for (int v = 0; v < 256; v++) if (seen[v]) printf(" %d", v);
    printf("\n");
    SDL_DestroySurface(rgb);
}

int main(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    int ver = SDL_GetVersion();
    printf("compiled against SDL %d.%d.%d, linked against %d.%d.%d\n",
           SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
           SDL_VERSIONNUM_MAJOR(ver), SDL_VERSIONNUM_MINOR(ver), SDL_VERSIONNUM_MICRO(ver));

    SDL_Window *w = NULL; SDL_Renderer *r = NULL;
    if (!SDL_CreateWindowAndRenderer("probe", 64, 64, 0, &w, &r)) {
        printf("CreateWindowAndRenderer: %s\n", SDL_GetError()); return 1;
    }
    printf("renderer: %s\n\n", SDL_GetRendererName(r));

    SDL_Texture *src = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, 4, 4);
    Uint32 px[16];
    for (int i = 0; i < 16; i++)
        px[i] = ((i / 4 + i % 4) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
    SDL_UpdateTexture(src, NULL, px, 4 * sizeof(Uint32));

    /* The fallback the emulator actually uses: nearest to an integer
     * multiple at least the target size, then one linear pass down. Must
     * work identically here and on a GPU, which is the whole point. */
    SDL_Texture *mid = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_TARGET, 16, 16);
    if (!mid) printf("TARGET texture: %s\n", SDL_GetError());

    report("NEAREST", r, src, 15);
    report("LINEAR", r, src, 15);
    report("PIXELART", r, src, 15);

    if (mid) {
        SDL_SetTextureScaleMode(src, SDL_SCALEMODE_NEAREST);
        SDL_SetRenderTarget(r, mid);
        SDL_RenderTexture(r, src, NULL, NULL);
        SDL_SetRenderTarget(r, NULL);
        report("2PASS", r, mid, 15);
    }
    SDL_Quit();
    return 0;
}
