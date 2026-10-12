#ifndef P2500_GUI_PREFS_H
#define P2500_GUI_PREFS_H

#include <stddef.h>

/* The two presentation enums, here rather than in main.cpp because the
 * preferences file, P2500_DISPLAY and the command line all have to agree on
 * their spellings, and only one of the three lives in main.cpp.
 *
 * ASPECT_43 draws the picture the machine's own monitor showed; square is
 * 1:1 dots. THEME is the interface chrome, NOT the emulated screen - the
 * screen's colour is the phosphor, which comes from core/video.h. */
enum { ASPECT_43 = 0, ASPECT_SQUARE = 1 };
enum { THEME_DARK = 0, THEME_PAPER = 1 };

/* Every accepted spelling of a presentation value, in one place, so a flag,
 * an environment variable and a line in the settings file cannot disagree
 * about what "square" means. Each returns false on a value it does not
 * recognise rather than falling back to a default: a typo that silently
 * changed how the screen was rendered would be a miserable thing to debug. */
bool p2500_parse_aspect(const char *v, int *out);
bool p2500_parse_onoff(const char *v, int *out);
bool p2500_parse_theme(const char *v, int *out);
const char *p2500_aspect_name(int aspect);
const char *p2500_theme_name(int theme);

/*
 * Tier one of the settings work: the preferences file.
 *
 * What belongs here and what does not is a line worth stating, because it
 * is the reason this is a separate thing from the profiles that will come
 * later. THIS FILE HOLDS ONLY SETTINGS THAT CHANGE HOW THE MACHINE LOOKS
 * AND FEELS - the viewport's shape, the screen's phosphor, the interface
 * theme, the speed, whether capitals lock is engaged, which debugger
 * panels were open. None of them changes what the machine *is*, so all of
 * them can be saved automatically and loaded always without anyone having
 * to be told.
 *
 * Everything that changes WHAT MACHINE YOU ARE RUNNING - mounted disks, the
 * cartridge key, write-protect, breakpoints, symbol files, which cards are
 * fitted - is deliberately NOT here. That is a named profile, loaded only
 * when asked for by name, because invisible machine state contaminates an
 * observation and this is a research tool before it is a toy.
 *
 * Capitals lock sits just inside the line and is worth justifying: it is a
 * key on the real keyboard that this emulator cannot model (the keyboard
 * controller is not modelled), so the toggle stands in for a physical
 * switch the user would have left where they left it. It is visible in the
 * menu at all times. Speed is the other judgement call - it changes
 * emulated timing rather than only presentation - but the lamp in the menu
 * bar shows it continuously, so remembering it is honest.
 *
 * PRECEDENCE, lowest to highest:
 *
 *     built-in defaults  <  this file  <  P2500_DISPLAY  <  command line
 *
 * WRITTEN ON CHANGE, not at exit, so a crash cannot cost a setting. What
 * gets written is the state the user is looking at, flags included: if you
 * launch with --phosphor white and then switch the theme from the menu, the
 * file records white. That is the predictable rule, and the flag wins over
 * the file next time anyway.
 *
 * P2500_CONFIG=none disables the file entirely - no read, no write. The
 * test suite sets it, for the same reason it sets P2500_DISPLAY: a
 * developer's saved settings must never reach a check. P2500_CONFIG=PATH
 * uses that file instead of the usual one.
 */

/* Where the preferences live, resolved once at startup:
 *
 *   P2500_CONFIG=none    -> disabled; returns NULL and nothing is read or
 *                           written for the rest of the run
 *   P2500_CONFIG=PATH    -> exactly that file
 *   otherwise, on Linux  -> $XDG_CONFIG_HOME/p2500/settings.ini, or
 *                           ~/.config/p2500/settings.ini
 *   otherwise            -> SDL_GetPrefPath()/settings.ini, which is the
 *                           right answer on Windows and macOS
 *
 * SDL_GetPrefPath is the portable answer but points at the *data*
 * directory on Linux (~/.local/share), and preferences are configuration -
 * so the one extra branch is taken.
 *
 * Where that lands, MEASURED rather than read off the documentation (an
 * SDL3 3.4.18 build run under wine):
 *
 *   Windows  C:\Users\<you>\AppData\Roaming\p2500\settings.ini
 *   macOS    ~/Library/Application Support/p2500/settings.ini
 *
 * Roaming, not Local or LocalLow - which is the right half for a
 * preferences file, since it follows the user onto another machine on a
 * roaming profile; Local is where a cache would go.
 *
 * The NULL org is deliberate. SDL's documentation asks new applications to
 * pass a real organisation name, and warns that omitting it can fail an app
 * store's certification - neither applies here, and there is no honest
 * organisation to name: "Philips" would be a claim about who wrote this.
 * NULL gives `Roaming\p2500\`, which matches the Linux `~/.config/p2500/`
 * rather than nesting it one level deeper on one platform only.
 *
 * Fills `buf` and returns it, or NULL. */
const char *p2500_prefs_path(char *buf, size_t n);

/* Where each setting's value came from. Reported in the `settings:` log
 * line and, through it, testable - which is what makes the precedence a
 * claim rather than an intention. */
enum P2500PrefSource {
    P2500_PREF_DEFAULT = 0,
    P2500_PREF_FILE,
    P2500_PREF_ENV,
    P2500_PREF_FLAG,
};

/* What the file said. Every field is a sentinel meaning "the file did not
 * mention this", so a key that is absent, misspelled or of the wrong type
 * all come out the same way: the layer above keeps its default. */
struct P2500Prefs {
    int aspect = -1;
    int snap = -1;
    int phosphor = -1;
    int theme = -1;
    float speed = -1.0f;
    int caps_lock = -1;
    int panel[5] = { -1, -1, -1, -1, -1 };   /* system, devices, memory, disasm, log */
};

/* Read the file. Returns false only when it exists and could not be read;
 * a file that has never been written is not a failure. A line that does not
 * parse is logged and skipped - a settings file must not stop the emulator
 * from starting - and *bad_lines, when given, counts them. */
bool p2500_prefs_load(const char *path, P2500Prefs *out, size_t *bad_lines);

/* Write the file. The existing one is re-read first and only the keys below
 * are replaced, so comments, hand-formatting, and anything a later version
 * wrote survive being saved over by this one. */
bool p2500_prefs_save(const char *path, const P2500Prefs &now);

#endif /* P2500_GUI_PREFS_H */
