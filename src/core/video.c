#include "video.h"

void p2500_video_info(const P2500Machine *m, P2500VideoInfo *info) {
    int cols = m->crtc_regs[1];
    int rows = m->crtc_regs[6] & 0x7F;
    int cell_h = (m->crtc_regs[9] & 0x1F) + 1;

    /* An unprogrammed CRTC reads back all zeroes, which is not a mode. Use
     * the geometry this machine's own IPL programs so a window can be sized
     * before the guest has touched the chip. */
    if (cols <= 0 || rows <= 0 || cell_h <= 1) {
        cols = 80;
        rows = 24;
        cell_h = 12;
    }
    if (cols > 255) cols = 255;
    if (rows > 127) rows = 127;
    if (cell_h > 32) cell_h = 32;

    info->cols = cols;
    info->rows = rows;
    info->cell_h = cell_h;
    info->width = cols * P2500_CELL_W;
    info->height = rows * cell_h;
    info->graphics = (m->port0a_latch & P2500_PORT0A_GRAPHICS) != 0;
}

static int crtc_start_addr(const P2500Machine *m) {
    return ((m->crtc_regs[12] & 0x3F) << 8) | m->crtc_regs[13];
}

int p2500_video_cursor_cell(const P2500Machine *m) {
    P2500VideoInfo info;
    p2500_video_info(m, &info);
    int cursor = ((m->crtc_regs[14] & 0x3F) << 8) | m->crtc_regs[15];
    int rel = cursor - crtc_start_addr(m);
    if (rel < 0 || rel >= info.cols * info.rows) return -1;
    return rel;
}

/* R10 bits 6-5: 00 = solid, 01 = cursor off, 10 = blink 1/16, 11 = 1/32. */
static bool cursor_visible(const P2500Machine *m, bool blink_on) {
    switch (m->crtc_regs[10] & 0x60) {
    case 0x00: return true;      /* non-blink, displayed */
    case 0x20: return false;     /* non-display */
    default:   return blink_on;  /* blinking */
    }
}

void p2500_video_render(const P2500Machine *m, const P2500Palette *pal,
                        bool blink_on, uint32_t *fb, int pitch_px) {
    P2500VideoInfo info;
    p2500_video_info(m, &info);

    const int start = crtc_start_addr(m);

    if (info.graphics) {
        /*
         * One bit per pixel, MSB leftmost, out of the same 16K the character
         * codes normally occupy - 512 x 256 = 131072 bits = 16384 bytes,
         * exactly.
         *
         * The arrangement is the MC6845's, not a linear framebuffer: the
         * raster address (which scanline of the cell row is being drawn)
         * is wired to address bits 12-13, so it selects one of four 4K
         * banks, and the CRTC's own memory address indexes within a bank.
         * That wiring is why CBIOS programs R9 to 3 on the way in - four
         * scanlines per row is all two bits of raster address can reach.
         *
         *     addr = (y % cell_h) * 4096 + (y / cell_h) * cols + x / 8
         *     bit  = 7 - (x % 8)
         *
         * Derived by driving CBIOS's own set-point call and reading back
         * where it wrote (TODO.md T47), at both ends of both axes and for
         * every raster address - not from the manual, which gives the
         * resolution but not the layout.
         */
        const int bank = P2500_VRAM_SIZE / 4;
        for (int y = 0; y < info.height; y++) {
            const int ra = (y % info.cell_h) & 3;
            const int cellrow = y / info.cell_h;
            uint32_t *out = fb + (size_t)y * pitch_px;
            for (int x = 0; x < info.width; x++) {
                /* The start address offsets the CRTC's address only - the
                 * bank select is a hardware line, not part of MA. Every
                 * dump seen so far has start = 0, so the placement of
                 * `start` here is reasoned, not observed. */
                const int ma = (start + cellrow * info.cols + (x >> 3)) & (bank - 1);
                const uint8_t bits = m->vram[ra * bank + ma];
                out[x] = (bits & (0x80 >> (x & 7))) ? pal->fg : pal->bg;
            }
        }
        return;
    }

    const int cursor_cell = p2500_video_cursor_cell(m);
    const bool cursor_lit = cursor_visible(m, blink_on);
    const int cur_first = m->crtc_regs[10] & 0x1F;
    const int cur_last = m->crtc_regs[11] & 0x1F;

    for (int row = 0; row < info.rows; row++) {
        for (int col = 0; col < info.cols; col++) {
            const int cell = row * info.cols + col;
            const int addr = (start + cell) & (P2500_VRAM_SIZE - 1);
            const uint8_t code = m->vram[addr];
            const uint8_t attr = m->vram_attr[addr] & P2500_VRAM_ATTR_MASK;

            uint32_t fg = (attr & P2500_ATTR_DIM) ? pal->fg_dim : pal->fg;
            uint32_t bg = pal->bg;
            if (attr & P2500_ATTR_REVERSE) {
                uint32_t t = fg; fg = bg; bg = t;
            }
            /* A flashing cell alternates between its glyph and blank. */
            const bool blanked = (attr & P2500_ATTR_FLASH) && !blink_on;

            const uint8_t *glyph = &m->charrom[code * P2500_CHARROM_STRIDE];
            const bool on_cursor = cursor_lit && cell == cursor_cell;

            for (int y = 0; y < info.cell_h; y++) {
                uint8_t bits = 0;
                if (!blanked && y < P2500_CHARROM_STRIDE) bits = glyph[y];
                if (!blanked && (attr & P2500_ATTR_UNDERLINE) &&
                    y == info.cell_h - 2)
                    bits = 0xFF;
                /* The cursor is drawn by inverting the scanlines R10-R11
                 * select, which is how the real chip composites it. */
                if (on_cursor && y >= cur_first && y <= cur_last)
                    bits = (uint8_t)~bits;

                uint32_t *out = fb + (size_t)(row * info.cell_h + y) * pitch_px
                                   + (size_t)col * P2500_CELL_W;
                for (int x = 0; x < P2500_CELL_W; x++)
                    out[x] = (bits & (0x80 >> x)) ? fg : bg;
            }
        }
    }
}
