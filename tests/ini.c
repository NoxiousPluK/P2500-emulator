/* The configuration syntax, checked against core/ini.h's own claims.
 *
 * It runs against libp2500.a directly rather than through a front-end,
 * for the same reason tests/fdc_eot.c does: the properties that matter
 * here are not reachable from a command line. "A file written back carries
 * everything it arrived with" is a claim about bytes, and the only honest
 * way to test it is to hand the parser a file a person would plausibly have
 * hand-edited - odd spacing, comments in awkward places, a section this
 * version has never heard of - and compare the output byte for byte.
 *
 * Built by `make tests/ini` and run from tests/run_tests.sh section 25. */

#include "core/ini.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void ok(int cond, const char *what)
{
    printf("  %s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

static void ok_str(const char *got, const char *want, const char *what)
{
    int c = got && !strcmp(got, want);
    printf("  %s  %s", c ? "PASS" : "FAIL", what);
    if (!c) printf(" (got %s, expected \"%s\")", got ? got : "(none)", want);
    printf("\n");
    if (!c) failures++;
}

static P2500Ini *parse(const char *text)
{
    P2500Ini *ini = p2500_ini_parse(text, strlen(text));
    if (!ini) { printf("  FAIL  out of memory\n"); exit(1); }
    return ini;
}

/* --- 1. the subset -------------------------------------------------------- */

static void test_types(void)
{
    static const char *src =
        "# a preferences file, as a person might have left it\n"
        "requires = [\"display\"]\n"
        "\n"
        "[display]\n"
        "aspect   = \"4:3\"      # the machine's own monitor\n"
        "snap = false\n"
        "scale=3\n"
        "\n"
        "[machine]\n"
        "speed = 0.5\n"
        "whole = 2\n"
        "caps_lock = true\n"
        "\n"
        "[cards.fdc2]\n"
        "base = 0x18\n"
        "quirks = [\"eightinch\", 2, \"double-density\"]\n";

    P2500Ini *ini = parse(src);
    const char *first = NULL;
    ok(p2500_ini_errors(ini, &first) == 0, "a file using the whole subset parses with no errors");
    if (first) printf("        first error was: %s\n", first);

    const char *s = NULL;
    ok(p2500_ini_get_str(ini, "display", "aspect", &s) && !strcmp(s, "4:3"),
       "a quoted string with a trailing comment reads back as the string alone");
    bool b = true;
    ok(p2500_ini_get_bool(ini, "display", "snap", &b) && b == false,
       "false is a boolean, not a string");
    long i = 0;
    ok(p2500_ini_get_int(ini, "display", "scale", &i) && i == 3,
       "a key with no spaces around the = still parses");
    double d = 0;
    ok(p2500_ini_get_float(ini, "machine", "speed", &d) && d == 0.5,
       "a float reads as a float");
    /* The trap ini.h promises not to fall into. */
    ok(p2500_ini_get_float(ini, "machine", "whole", &d) && d == 2.0,
       "an integer satisfies a float lookup - speed = 1 and speed = 1.0 are one setting");
    ok(!p2500_ini_get_int(ini, "machine", "speed", &i),
       "but a float does NOT satisfy an integer lookup");
    ok(p2500_ini_get_int(ini, "cards.fdc2", "base", &i) && i == 0x18,
       "a dotted section name is one section, and 0x18 is 24");

    /* A wrong-typed or absent key must be distinguishable from a real one,
     * because that return value is the whole "leave the default alone"
     * mechanism in the front-end. */
    ok(!p2500_ini_get_bool(ini, "display", "aspect", &b),
       "a string does not answer a boolean lookup");
    ok(!p2500_ini_get_str(ini, "display", "nosuch", &s),
       "an absent key reads as absent");
    ok(!p2500_ini_get_str(ini, "machine", "aspect", &s),
       "a key is not visible from another section");

    ok(p2500_ini_array_len(ini, "cards.fdc2", "quirks") == 3, "an array has its elements");
    ok_str(p2500_ini_array_str(ini, "cards.fdc2", "quirks", 0), "eightinch",
           "a string element comes back decoded");
    ok_str(p2500_ini_array_str(ini, "cards.fdc2", "quirks", 1), "2",
           "a non-string element comes back as written");
    long e = 0;
    ok(p2500_ini_array_int(ini, "cards.fdc2", "quirks", 1, &e) && e == 2,
       "and as a number when it is one");
    ok(!p2500_ini_array_int(ini, "cards.fdc2", "quirks", 0, &e),
       "a string element is not a number");

    p2500_ini_free(ini);
}

static void test_outside_the_subset(void)
{
    /* Each of these is a line ini.h says is NOT in the subset. The claim
     * under test is that it is REPORTED - a silently mis-read value is the
     * failure mode a partial TOML parser has, and the reason this file
     * exists. */
    struct { const char *line; const char *what; } bad[] = {
        { "key = 'single'",        "a single-quoted string" },
        { "key = \"unterminated",  "an unterminated string" },
        { "key = {a = 1}",         "an inline table" },
        { "key = [1, [2]]",        "a nested array" },
        { "key = 1979-05-27",      "a datetime" },
        { "a.b = 1",               "a dotted key" },
        { "[[products]]",          "an array of tables" },
        { "[unclosed",             "a section header with no ]" },
        { "novalue",               "a line with no =" },
        { "key =",                 "a key with no value" },
        { "key = bare",            "an unquoted string" },
    };
    for (size_t k = 0; k < sizeof bad / sizeof *bad; k++) {
        P2500Ini *ini = parse(bad[k].line);
        char what[128];
        snprintf(what, sizeof what, "%s is reported, not guessed at", bad[k].what);
        ok(p2500_ini_errors(ini, NULL) == 1, what);
        /* And it must still be readable: a rejected line is kept verbatim,
         * so one bad line in a settings file costs that line and nothing
         * else. */
        size_t len = 0;
        char *out = p2500_ini_render(ini, &len);
        char keep[128];
        snprintf(keep, sizeof keep, "...and %s is kept in the file", bad[k].what);
        ok(out && !strncmp(out, bad[k].line, strlen(bad[k].line)), keep);
        free(out);
        /* Nothing may be readable under that key, either: half-parsing a
         * value and offering it is worse than refusing it. */
        const char *s = NULL;
        long i = 0;
        bool b = false;
        double d = 0;
        ok(!p2500_ini_get_str(ini, "", "key", &s) && !p2500_ini_get_int(ini, "", "key", &i) &&
           !p2500_ini_get_bool(ini, "", "key", &b) && !p2500_ini_get_float(ini, "", "key", &d),
           "...and nothing can be read out of it");
        p2500_ini_free(ini);
    }

    /* A bad line must not cost the lines around it. */
    P2500Ini *ini = parse("[display]\naspect = \"4:3\"\nsnap = yes\ntheme = \"paper\"\n");
    ok(p2500_ini_errors(ini, NULL) == 1, "one bad line in three is one error");
    const char *s = NULL;
    ok(p2500_ini_get_str(ini, "display", "aspect", &s) && !strcmp(s, "4:3") &&
       p2500_ini_get_str(ini, "display", "theme", &s) && !strcmp(s, "paper"),
       "and the keys either side of it still read");
    p2500_ini_free(ini);
}

/* --- 2. preservation ------------------------------------------------------ */

static void test_round_trip(void)
{
    /* What a hand-edited file looks like, and every part of it is a thing
     * the renderer could plausibly destroy: a comment block, a comment
     * after a value, odd alignment, tabs, a blank line inside a section,
     * CRLF, a section this reader knows nothing about, and no trailing
     * newline on the last line. */
    static const char *src =
        "# Written by hand. Do not reformat.\r\n"
        "\r\n"
        "[display]\r\n"
        "aspect    = \"square\"   # I like square dots\r\n"
        "\tsnap  =\ttrue\r\n"
        "\r\n"
        "[something-from-a-later-version]\r\n"
        "whatever = [1, 2, 3]\r\n"
        "nested-thing = \"keep me\"";

    static const char *want =
        "# Written by hand. Do not reformat.\n"
        "\n"
        "[display]\n"
        "aspect    = \"square\"   # I like square dots\n"
        "\tsnap  =\ttrue\n"
        "\n"
        "[something-from-a-later-version]\n"
        "whatever = [1, 2, 3]\n"
        "nested-thing = \"keep me\"\n";

    P2500Ini *ini = parse(src);
    ok(p2500_ini_errors(ini, NULL) == 0, "a hand-edited file parses clean");
    size_t len = 0;
    char *out = p2500_ini_render(ini, &len);
    ok(out && !strcmp(out, want),
       "re-rendering an untouched file changes nothing but the line endings");
    if (out && strcmp(out, want)) printf("---- got ----\n%s---- want ----\n%s----\n", out, want);
    free(out);

    /* Now change ONE value. Everything else - including the section this
     * version has never heard of - must come out identical. */
    ok(p2500_ini_set_str(ini, "display", "aspect", "4:3"), "setting an existing key succeeds");
    out = p2500_ini_render(ini, &len);
    ok(out && strstr(out, "aspect    = \"4:3\"   # I like square dots") != NULL,
       "a changed value keeps the key's indentation and its trailing comment");
    ok(out && strstr(out, "[something-from-a-later-version]") != NULL &&
       strstr(out, "nested-thing = \"keep me\"") != NULL,
       "a section this version does not understand survives a write");
    ok(out && strstr(out, "# Written by hand. Do not reformat.") != NULL,
       "and so do the comments");
    ok(out && strstr(out, "\tsnap  =\ttrue") != NULL,
       "an untouched key is not reformatted at all");
    free(out);
    p2500_ini_free(ini);
}

static void test_writes(void)
{
    P2500Ini *ini = parse("[display]\naspect = \"4:3\"\n\n[machine]\nspeed = 1.0\n");

    /* A new key goes under its own section's header, not at the end of the
     * file - which is the only placement a person reading the file would
     * expect, and the easy thing to get wrong. */
    ok(p2500_ini_set_bool(ini, "display", "snap", true), "a new key can be added");
    size_t len = 0;
    char *out = p2500_ini_render(ini, &len);
    ok_str(out,
           "[display]\naspect = \"4:3\"\nsnap = true\n\n[machine]\nspeed = 1.0\n",
           "a new key lands under its own section, not at the end of the file");
    free(out);

    /* Two keys appended to the LAST section in the file: the section's last
     * line and the document's last line are the same index there, and
     * confusing the two grows a duplicate [header] per key. */
    ok(p2500_ini_set_bool(ini, "machine", "caps_lock", true) &&
       p2500_ini_set_str(ini, "machine", "phosphor-note", "x"),
       "two keys can be appended to the last section in the file");
    out = p2500_ini_render(ini, &len);
    ok_str(out,
           "[display]\naspect = \"4:3\"\nsnap = true\n\n[machine]\nspeed = 1.0\n"
           "caps_lock = true\nphosphor-note = \"x\"\n",
           "and the section header is written once, not once per key");
    free(out);

    /* A new section appends, with a blank line to separate it. */
    ok(p2500_ini_set_int(ini, "cards.fdc2", "base", 0x18), "a new section can be added");
    out = p2500_ini_render(ini, &len);
    ok(out && strstr(out, "\n\n[cards.fdc2]\nbase = 24\n") != NULL,
       "a new section is appended with a blank line before it");
    free(out);

    /* A float must not come out looking like an integer, or a real TOML
     * reader would change its type on us. */
    ok(p2500_ini_set_float(ini, "machine", "speed", 1.0), "a float can be set");
    out = p2500_ini_render(ini, &len);
    ok(out && strstr(out, "speed = 1.0\n") != NULL,
       "a whole-numbered float is written 1.0, not 1");
    free(out);

    /* A set must be visible to a get, with no reparse in between. */
    double d = 0;
    ok(p2500_ini_set_float(ini, "machine", "speed", 0.25) &&
       p2500_ini_get_float(ini, "machine", "speed", &d) && d == 0.25,
       "a value just set reads back as itself");

    /* Strings get escaped, and survive the round trip. */
    ok(p2500_ini_set_str(ini, "profile", "disk", "a \"quoted\\path\"\n"),
       "a string with quotes, a backslash and a newline can be set");
    out = p2500_ini_render(ini, &len);
    P2500Ini *again = parse(out);
    const char *s = NULL;
    ok(p2500_ini_errors(again, NULL) == 0 &&
       p2500_ini_get_str(again, "profile", "disk", &s) &&
       !strcmp(s, "a \"quoted\\path\"\n"),
       "and comes back byte-identical through a render and a reparse");
    p2500_ini_free(again);
    free(out);

    const char *vals[] = { "P25K_B.raw", "", "P25TEST.raw" };
    ok(p2500_ini_set_array_str(ini, "profile", "disks", vals, 3), "an array can be set");
    out = p2500_ini_render(ini, &len);
    again = parse(out);
    ok(p2500_ini_array_len(again, "profile", "disks") == 3 &&
       !strcmp(p2500_ini_array_str(again, "profile", "disks", 1), ""),
       "including an empty element, which is how a profile skips a drive");
    p2500_ini_free(again);
    free(out);
    p2500_ini_free(ini);
}

static void test_file_io(void)
{
    const char *path = "tests/.ini-tmp/sub/settings.ini";
    P2500Ini *ini = p2500_ini_new();
    ok(ini != NULL, "an empty document can be built from nothing");
    p2500_ini_append_comment(ini, "P2500 emulator preferences.");
    p2500_ini_append_comment(ini, NULL);
    p2500_ini_set_str(ini, "display", "phosphor", "green");
    /* Two directories deep, neither existing: a config directory that has
     * never been written to is the normal case, not the exception. */
    ok(p2500_ini_save(ini, path), "saving creates the directories it needs");
    p2500_ini_free(ini);

    bool missing = true;
    ini = p2500_ini_load(path, &missing);
    const char *s = NULL;
    ok(ini && !missing && p2500_ini_get_str(ini, "display", "phosphor", &s) &&
       !strcmp(s, "green"), "and it loads back");
    size_t len = 0;
    char *out = ini ? p2500_ini_render(ini, &len) : NULL;
    ok_str(out, "# P2500 emulator preferences.\n\n[display]\nphosphor = \"green\"\n",
           "a generated file reads the way a person would write one");
    free(out);
    p2500_ini_free(ini);

    /* Absent is not an error, and must be distinguishable from one: a
     * settings file that has never been written yet is the first-run case. */
    missing = false;
    ini = p2500_ini_load("tests/.ini-tmp/nope.ini", &missing);
    ok(ini == NULL && missing, "a file that does not exist reports missing, not broken");

    remove(path);
    remove("tests/.ini-tmp/sub");
    remove("tests/.ini-tmp");
}

/* --- 3. forward compatibility --------------------------------------------- */

static void test_requires(void)
{
    static const char *known[] = { "display", "machine" };
    P2500Ini *ini = parse("requires = [\"display\"]\n[display]\nsnap = true\n");
    ok(p2500_ini_requires_unmet(ini, known, 2) == NULL,
       "a file requiring only what this reader has loads");
    p2500_ini_free(ini);

    ini = parse("requires = [\"display\", \"hard-disk-geometry\"]\n");
    ok_str(p2500_ini_requires_unmet(ini, known, 2), "hard-disk-geometry",
           "a file requiring something this reader lacks names it");
    p2500_ini_free(ini);

    /* The common case by far, and it must not be made to fail: a file that
     * only ADDED keys requires nothing and loads everywhere. */
    ini = parse("[display]\nsnap = true\nsomething-new = 3\n");
    ok(p2500_ini_requires_unmet(ini, known, 2) == NULL,
       "a file with no requires list always loads");
    p2500_ini_free(ini);

    /* And it is the top-level key, not one inside a section - otherwise a
     * profile naming a section called `requires` would lock itself out. */
    ini = parse("[display]\nrequires = [\"nonsense\"]\n");
    ok(p2500_ini_requires_unmet(ini, known, 2) == NULL,
       "requires is read at the top level only");
    p2500_ini_free(ini);
}

int main(void)
{
    test_types();
    test_outside_the_subset();
    test_round_trip();
    test_writes();
    test_file_io();
    test_requires();
    return failures ? 1 : 0;
}
