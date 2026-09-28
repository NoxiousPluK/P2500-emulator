#ifndef P2500_VIDEO_H
#define P2500_VIDEO_H

#include <stdint.h>
#include <stdbool.h>

#include "machine.h"

#ifdef __cplusplus
extern "C" {
#endif

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
 * Attributes come from the card's separate 4-bit plane, and the assignment
 * below is now established rather than guessed. The P2219 CP/M manual's
 * "CONTROL CODES (ESCAPE SEQUENCES)" section defines the ESC 0 parameter
 * byte:
 *
 *     bit 0 = 1  display character at low intensity
 *     bit 1 = 1  flash character
 *     bit 4 = 1  display character in reverse video
 *     bit 5 = 1  display character underlined
 *
 * CBIOS's handler at $F252 then scatters those into the nibble it latches
 * in port $0A - parameter bit 0 -> attribute bit 1, 1 -> 3, 4 -> 2, 5 -> 0
 * (verified by driving all 16 values through MBASIC and dumping the plane).
 * Composing the two gives the constants below. Keep them in step with
 * tools/render_vram.py, which carries the same four.
 */

#define P2500_CELL_W 8
#define P2500_CHARROM_SIZE 0x1000
#define P2500_CHARROM_STRIDE 16 /* bytes per code; only the first R9+1 used */

#define P2500_ATTR_UNDERLINE 0x01 /* ESC 0 parameter bit 5 */
#define P2500_ATTR_DIM       0x02 /* ESC 0 parameter bit 0, "low intensity" */
#define P2500_ATTR_REVERSE   0x04 /* ESC 0 parameter bit 4 */
#define P2500_ATTR_FLASH     0x08 /* ESC 0 parameter bit 1 */

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

#ifdef __cplusplus
}
#endif

#endif
