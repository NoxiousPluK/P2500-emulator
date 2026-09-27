#ifndef P2500_VIDEO_H
#define P2500_VIDEO_H

#include <stdint.h>
#include <stdbool.h>

#include "machine.h"

/*
 * Text-mode renderer, driven by the MC6845's own registers rather than a
 * hardcoded 80x24 (TODO.md T37).
 *
 * The character cell is 8 pixels wide by (R9 + 1) scanlines - 12 on this
 * machine. The glyph is the WHOLE cell: the character ROM's stride is 16
 * bytes per code, of which rows 0-11 are the glyph and 12-15 are padding
 * that happens to hold packed Z80 code (TODO.md T27a). Reading only 8 rows
 * truncates every descender, which is what every render in this project did
 * until 2026-09-28 - the only printable codes with ink in rows 8-11 are
 * "$ , ; @ f g j p q y", exactly the descender set.
 *
 * Attributes come from the card's separate 4-bit plane. Which nibble bit
 * carries which attribute is NOT established - the four the P2219 manual
 * documents are underline, reverse, flash and low intensity, and the
 * assignment below is a placeholder. It must stay in step with
 * tools/render_vram.py, which has the same four constants.
 */

#define P2500_CELL_W 8
#define P2500_CHARROM_SIZE 0x1000
#define P2500_CHARROM_STRIDE 16 /* bytes per code; only the first R9+1 used */

#define P2500_ATTR_UNDERLINE 0x01
#define P2500_ATTR_REVERSE   0x02
#define P2500_ATTR_FLASH     0x04
#define P2500_ATTR_DIM       0x08

typedef struct {
    int cols, rows;   /* character grid, from R1 and R6 */
    int cell_h;       /* scanlines per row, from R9 + 1 */
    int width, height;/* framebuffer size in pixels */
} P2500VideoInfo;

/* Geometry the CRTC is currently programmed for. Falls back to 80x24x12 if
 * the registers have not been written yet (all-zero is not a usable mode),
 * so a front-end can size a window before the guest has booted. */
void p2500_video_info(const P2500Machine *m, P2500VideoInfo *info);

typedef struct {
    uint32_t fg, bg, fg_dim; /* 0xAARRGGBB */
} P2500Palette;

/* Render the visible screen into `fb`, which must hold at least
 * info.height rows of `pitch_px` pixels. `blink_on` is the caller's blink
 * phase, used for the cursor (R10 bits 6-5) and the flash attribute; pass
 * a value that alternates a couple of times a second. */
void p2500_video_render(const P2500Machine *m, const P2500Palette *pal,
                        bool blink_on, uint32_t *fb, int pitch_px);

/* Cursor cell as the CRTC reports it (R14/R15), or -1 when it is outside
 * the displayed area. CP/M keeps this up to date: after DIR it reads
 * row 6 x 80 + 2, exactly where the A> prompt leaves it. */
int p2500_video_cursor_cell(const P2500Machine *m);

#endif
