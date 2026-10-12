/* The preferences file - see prefs.h for what belongs in it and why.
 *
 * The syntax and the parser are core/ini.c's, shared with the profiles and
 * the media metadata that will want the same file format. Nothing here
 * knows how to parse anything; it only names keys. */

#include "prefs.h"

#include "core/ini.h"
#include "core/video.h"

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

/* --- the value spellings, shared with the flags and with P2500_DISPLAY --- */

bool p2500_parse_aspect(const char *v, int *out)
{
    if (!SDL_strcasecmp(v, "43") || !SDL_strcasecmp(v, "4:3")) { *out = ASPECT_43; return true; }
    if (!SDL_strcasecmp(v, "square") || !SDL_strcasecmp(v, "1:1")) { *out = ASPECT_SQUARE; return true; }
    return false;
}

const char *p2500_aspect_name(int aspect)
{
    return aspect == ASPECT_SQUARE ? "square" : "4:3";
}

bool p2500_parse_onoff(const char *v, int *out)
{
    if (!SDL_strcasecmp(v, "on") || !SDL_strcasecmp(v, "1") ||
        !SDL_strcasecmp(v, "yes") || !SDL_strcasecmp(v, "true")) { *out = 1; return true; }
    if (!SDL_strcasecmp(v, "off") || !SDL_strcasecmp(v, "0") ||
        !SDL_strcasecmp(v, "no") || !SDL_strcasecmp(v, "false")) { *out = 0; return true; }
    return false;
}

bool p2500_parse_theme(const char *v, int *out)
{
    if (!SDL_strcasecmp(v, "dark"))  { *out = THEME_DARK;  return true; }
    if (!SDL_strcasecmp(v, "paper") || !SDL_strcasecmp(v, "light")) { *out = THEME_PAPER; return true; }
    return false;
}

const char *p2500_theme_name(int theme)
{
    return theme == THEME_PAPER ? "paper" : "dark";
}

/* --- where the file lives ------------------------------------------------- */

const char *p2500_prefs_path(char *buf, size_t n)
{
    const char *env = SDL_getenv("P2500_CONFIG");
    if (env) {
        if (!SDL_strcasecmp(env, "none")) return NULL;
        SDL_strlcpy(buf, env, n);
        return buf;
    }
#ifdef __linux__
    /* Preferences are configuration, and on Linux that is XDG_CONFIG_HOME -
     * SDL_GetPrefPath would put them under ~/.local/share, which is for
     * data. One branch, and it is the platform this is developed on. */
    const char *xdg = SDL_getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        SDL_snprintf(buf, n, "%s/p2500/settings.ini", xdg);
        return buf;
    }
    const char *home = SDL_getenv("HOME");
    if (home && *home) {
        SDL_snprintf(buf, n, "%s/.config/p2500/settings.ini", home);
        return buf;
    }
#endif
    /* NULL org on purpose, and measured: see the note in prefs.h. */
    char *pref = SDL_GetPrefPath(NULL, "p2500");
    if (!pref) return NULL;
    SDL_snprintf(buf, n, "%ssettings.ini", pref);   /* SDL's path ends in a separator */
    SDL_free(pref);
    return buf;
}

/* --- the keys ------------------------------------------------------------- */

/* The debugger panels, in the order App stores them. One table rather than
 * five copies of the same two lines in load and save. */
static const char *const PANEL_KEY[5] = { "system", "devices", "memory", "disasm", "log" };

/* A feature name in a top-level `requires` array means "a reader that does
 * not understand this must refuse the file rather than load it wrong". This
 * reader understands the tier-one preferences, and nothing more. Future
 * versions add names here as they add meaning; a version that only adds
 * KEYS needs no entry, because an unknown key is already safe to ignore. */
static const char *const UNDERSTOOD[] = { "prefs1" };

bool p2500_prefs_load(const char *path, P2500Prefs *out, size_t *bad_lines)
{
    if (bad_lines) *bad_lines = 0;
    if (!path || !out) return true;

    bool missing = false;
    P2500Ini *ini = p2500_ini_load(path, &missing);
    if (!ini) {
        if (missing) return true;      /* never written yet: the first run */
        SDL_Log("settings: cannot read %s", path);
        return false;
    }

    const char *unmet = p2500_ini_requires_unmet(ini, UNDERSTOOD,
                                                 SDL_arraysize(UNDERSTOOD));
    if (unmet) {
        /* Refusing cleanly beats loading it wrong - which is the whole
         * reason `requires` exists rather than only ignoring unknown keys.
         * The file is left alone, so a newer build still reads it. */
        SDL_Log("settings: %s needs '%s', which this build does not understand"
                " - using defaults and leaving the file alone", path, unmet);
        p2500_ini_free(ini);
        return false;
    }

    size_t nerr = p2500_ini_errors(ini, NULL);
    if (nerr) {
        for (size_t i = 0; p2500_ini_error_at(ini, i); i++)
            SDL_Log("settings: %s: %s", path, p2500_ini_error_at(ini, i));
        if (bad_lines) *bad_lines = nerr;
    }

    const char *s = NULL;
    bool b = false;
    double d = 0;

    if (p2500_ini_get_str(ini, "display", "aspect", &s)) {
        int v;
        if (p2500_parse_aspect(s, &v)) out->aspect = v;
        else SDL_Log("settings: aspect: '%s' is not 4:3 or square", s);
    }
    if (p2500_ini_get_bool(ini, "display", "snap", &b)) out->snap = b ? 1 : 0;
    if (p2500_ini_get_str(ini, "display", "phosphor", &s)) {
        P2500PhosphorId id;
        if (p2500_phosphor_by_name(s, &id)) out->phosphor = (int)id;
        else SDL_Log("settings: phosphor: no such screen colour '%s'", s);
    }
    if (p2500_ini_get_str(ini, "display", "theme", &s)) {
        int v;
        if (p2500_parse_theme(s, &v)) out->theme = v;
        else SDL_Log("settings: theme: '%s' is not dark or paper", s);
    }

    /* Speed is a float with one spelling that is not a number, the same one
     * --speed takes. 0 IS unlimited on the wire, so both are accepted. */
    if (p2500_ini_get_str(ini, "machine", "speed", &s)) {
        if (!SDL_strcasecmp(s, "unlimited") || !SDL_strcasecmp(s, "max")) out->speed = 0.0f;
        else SDL_Log("settings: speed: '%s' is not a number or \"unlimited\"", s);
    } else if (p2500_ini_get_float(ini, "machine", "speed", &d)) {
        if (d >= 0.0) out->speed = (float)d;
        else SDL_Log("settings: speed: %g is negative", d);
    }
    if (p2500_ini_get_bool(ini, "machine", "caps_lock", &b)) out->caps_lock = b ? 1 : 0;

    for (int i = 0; i < 5; i++)
        if (p2500_ini_get_bool(ini, "panels", PANEL_KEY[i], &b)) out->panel[i] = b ? 1 : 0;

    p2500_ini_free(ini);
    return true;
}

bool p2500_prefs_save(const char *path, const P2500Prefs &now)
{
    if (!path) return true;

    /* Re-read before writing, so everything in the file this version does
     * not know about comes back out of it unchanged. */
    bool missing = false;
    P2500Ini *ini = p2500_ini_load(path, &missing);
    if (!ini) {
        ini = p2500_ini_new();
        if (!ini) return false;
        p2500_ini_append_comment(ini, "P2500 emulator preferences.");
        p2500_ini_append_comment(ini, "Written automatically when a setting changes;"
                                      " safe to hand-edit - comments and anything");
        p2500_ini_append_comment(ini, "this version does not recognise are kept."
                                      " Command-line flags and P2500_DISPLAY");
        p2500_ini_append_comment(ini, "override what is here; P2500_CONFIG=none ignores"
                                      " this file entirely.");
        p2500_ini_append_comment(ini, NULL);
    }

    p2500_ini_set_str(ini, "display", "aspect", p2500_aspect_name(now.aspect));
    p2500_ini_set_bool(ini, "display", "snap", now.snap > 0);
    p2500_ini_set_str(ini, "display", "phosphor",
                      p2500_phosphor_name((P2500PhosphorId)now.phosphor));
    p2500_ini_set_str(ini, "display", "theme", p2500_theme_name(now.theme));

    if (now.speed == 0.0f) p2500_ini_set_str(ini, "machine", "speed", "unlimited");
    else p2500_ini_set_float(ini, "machine", "speed", (double)now.speed);
    p2500_ini_set_bool(ini, "machine", "caps_lock", now.caps_lock > 0);

    for (int i = 0; i < 5; i++)
        p2500_ini_set_bool(ini, "panels", PANEL_KEY[i], now.panel[i] > 0);

    bool ok = p2500_ini_save(ini, path);
    if (!ok) SDL_Log("settings: cannot write %s", path);
    p2500_ini_free(ini);
    return ok;
}
