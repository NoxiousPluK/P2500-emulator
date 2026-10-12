#ifndef P2500_INI_H
#define P2500_INI_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The project's configuration syntax: INI, written as a valid TOML subset.
 *
 * One parser, three jobs, which is why this lives in core/ rather than in
 * the front-end that needed it first:
 *
 *   - the preferences file (settings.ini), which is what prompted it;
 *   - named profiles - a disk set, a cartridge key, the cards fitted;
 *   - the metadata block inside a self-describing media container.
 *
 * So the syntax is fixed here once, deliberately, rather than growing a
 * second dialect per consumer later.
 *
 * THE SUBSET, exactly:
 *
 *   - UTF-8. LF is written; CRLF is accepted on read.
 *   - A line is blank, a comment (`#` as the first non-blank character),
 *     a section header `[name]`, or `key = value`.
 *   - Section names are bare segments joined by `.` - `[display]`,
 *     `[cards.fdc2]`. A dotted name is one opaque section name here, not a
 *     tree; TOML reads it as a nested table, which is the same document.
 *   - Keys are bare: letters, digits, `_`, `-`. Case-sensitive.
 *   - Values: a double-quoted string (escapes `\\ \" \n \t`), a decimal or
 *     `0x` integer, a float, `true`/`false`, or a single-line array
 *     `[a, b, c]` with an optional trailing comma.
 *   - A `#` outside a string or an array ends the value and starts a
 *     trailing comment.
 *
 * Deliberately NOT in the subset, because a hand-rolled half-TOML that
 * claims more than it parses is worse than one that claims less: multi-line
 * strings, literal (single-quoted) strings, datetimes, inline tables,
 * arrays of tables, dotted *keys*. A line using one is reported and kept
 * verbatim rather than guessed at.
 *
 * TWO PROPERTIES THE CONSUMERS DEPEND ON:
 *
 *   - Unknown sections and keys are preserved, not dropped. A file written
 *     back carries everything it arrived with, so hand-edits survive, a
 *     downgrade does not silently delete the newer version's settings, and
 *     comments stay where their author put them. Every line is kept
 *     verbatim; only a line whose value is actually *set* is re-rendered.
 *   - "Ignore what you did not understand" is not enough on its own: it
 *     mis-loads a file whose meaning changed. So a document may carry a
 *     top-level `requires = [...]` naming features a reader must understand,
 *     and p2500_ini_requires_unmet() is how a reader refuses cleanly
 *     instead of loading it wrong. Files that only *add* keys need no
 *     entry there and keep loading everywhere.
 */

typedef struct P2500Ini P2500Ini;

typedef enum {
    P2500_INI_STRING,
    P2500_INI_INT,
    P2500_INI_FLOAT,
    P2500_INI_BOOL,
    P2500_INI_ARRAY,
} P2500IniType;

/* An empty document, for writing one from nothing. NULL on allocation
 * failure, which is the only way any constructor here fails. */
P2500Ini *p2500_ini_new(void);

/* Parse text. A malformed line is counted, its message recorded, and the
 * line kept verbatim - parsing never fails as a whole, because a settings
 * file with one bad line must not stop a program from starting. Ask
 * p2500_ini_errors() what happened. */
P2500Ini *p2500_ini_parse(const char *text, size_t len);

/* Read a file. Returns NULL when it cannot be read AT ALL; *missing is set
 * true when that is because it does not exist, which is not an error for a
 * file that has simply never been written yet. */
P2500Ini *p2500_ini_load(const char *path, bool *missing);

/* How many lines did not parse, and the first such message
 * ("line 7: unterminated string"). Returns 0 and leaves *first alone when
 * the document was clean. */
size_t p2500_ini_errors(const P2500Ini *ini, const char **first);
/* Every message, in order; NULL past the end. */
const char *p2500_ini_error_at(const P2500Ini *ini, size_t i);

/* Render to a NUL-terminated buffer the caller frees. *len, when given,
 * excludes the NUL. */
char *p2500_ini_render(const P2500Ini *ini, size_t *len);
/* Write to a file, creating the parent directory if it is missing. */
bool p2500_ini_save(const P2500Ini *ini, const char *path);

void p2500_ini_free(P2500Ini *ini);

/* Lookups. `section` is "" or NULL for the top level, above the first
 * header. Each returns false when the key is absent or holds another type -
 * so a caller that wants "leave the default alone unless the file says
 * otherwise" gets it from the return value, with no sentinel needed.
 *
 * get_float accepts an integer too: `speed = 1` and `speed = 1.0` are the
 * same setting written two ways, and rejecting the first would be a trap. */
bool p2500_ini_get_str(const P2500Ini *ini, const char *section, const char *key,
                       const char **out);
bool p2500_ini_get_int(const P2500Ini *ini, const char *section, const char *key,
                       long *out);
bool p2500_ini_get_float(const P2500Ini *ini, const char *section, const char *key,
                         double *out);
bool p2500_ini_get_bool(const P2500Ini *ini, const char *section, const char *key,
                        bool *out);

/* Does the key exist at all, and as what. */
bool p2500_ini_type(const P2500Ini *ini, const char *section, const char *key,
                    P2500IniType *out);

/* Arrays. Elements keep their own types; element_str renders a non-string
 * element the way it was written, so a caller reading a list of names does
 * not have to care that someone quoted one of them and not another. */
size_t p2500_ini_array_len(const P2500Ini *ini, const char *section, const char *key);
const char *p2500_ini_array_str(const P2500Ini *ini, const char *section,
                                const char *key, size_t i);
bool p2500_ini_array_int(const P2500Ini *ini, const char *section, const char *key,
                         size_t i, long *out);

/* Writes. An existing key keeps its place, its indentation and its trailing
 * comment, and only the value changes; a new key is appended to the end of
 * its section; a new section is appended to the end of the document. */
bool p2500_ini_set_str(P2500Ini *ini, const char *section, const char *key,
                       const char *value);
bool p2500_ini_set_int(P2500Ini *ini, const char *section, const char *key, long value);
bool p2500_ini_set_float(P2500Ini *ini, const char *section, const char *key, double value);
bool p2500_ini_set_bool(P2500Ini *ini, const char *section, const char *key, bool value);
bool p2500_ini_set_array_str(P2500Ini *ini, const char *section, const char *key,
                             const char *const *values, size_t n);

/* Append a comment or a blank line at the end of the document. For writing
 * a header onto a file this program created - the one thing a generated
 * settings file owes the person who has to edit it. Pass NULL for a blank
 * line. */
bool p2500_ini_append_comment(P2500Ini *ini, const char *text);

/* Forward compatibility. Reads the top-level `requires` array and returns
 * the first entry not in `known`, or NULL when the document asks for
 * nothing this reader lacks. A document with no `requires` always passes. */
const char *p2500_ini_requires_unmet(const P2500Ini *ini,
                                     const char *const *known, size_t nknown);

#ifdef __cplusplus
}
#endif

#endif /* P2500_INI_H */
