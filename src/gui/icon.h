#ifndef P2500_GUI_ICON_H
#define P2500_GUI_ICON_H

struct SDL_Window;

/*
 * Give the window the application icon. Purely cosmetic: a failure is
 * logged and otherwise ignored, because an emulator that will not start
 * over a title-bar picture would be a worse program than one with no icon.
 *
 * Only the window icon is set here. The Windows .exe carries the same
 * artwork as a PE resource instead (src/win/p2500.rc), which is what
 * Explorer and the taskbar read; ELF binaries have no equivalent, so on
 * Linux this call is all there is.
 */
void p2500_set_window_icon(SDL_Window *window);

#endif
