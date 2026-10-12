/* The configuration syntax - see ini.h for what the subset is and why.
 *
 * The document model is a list of LINES, each kept verbatim. A key line
 * additionally carries its section, key, parsed value and trailing comment,
 * and is re-rendered only when something sets it. That is what makes
 * "surviving a hand-edit" a property of the design rather than an effort:
 * the only lines this program can reformat are the ones it changed. */

#include "ini.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define MAX_ERRORS 16

typedef struct {
    char *raw;          /* the line as it will be written, no newline */
    char *section;      /* owned: this line's section, "" at the top */
    bool is_header;     /* a [section] line */
    char *key;          /* NULL unless this is a key = value line */
    P2500IniType type;
    char *str;          /* STRING: the decoded value */
    long i;
    double f;
    bool b;
    char **elem;        /* ARRAY: elements as written, strings decoded */
    P2500IniType *etype;
    size_t nelem;
    char *comment;      /* trailing comment, '#' included, or NULL */
    /* The four runs of whitespace a key line can carry, kept so that
     * changing a value does not re-flow a line someone had aligned by
     * hand. The comment still shifts when the value changes length -
     * nothing short of re-aligning the whole section could stop that -
     * but the author's spacing is what it shifts by. */
    char *indent;       /* before the key */
    char *pad_eq;       /* between the key and the = */
    char *pad_val;      /* between the = and the value */
    char *pad_cmt;      /* between the value and the # */
} IniLine;

struct P2500Ini {
    IniLine *line;
    size_t n, cap;
    char *err[MAX_ERRORS];
    size_t nerr;        /* total seen, which can exceed MAX_ERRORS */
    size_t nerr_kept;
};

/* --- small helpers -------------------------------------------------------- */

static char *dup_n(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *dup_s(const char *s) { return dup_n(s, strlen(s)); }

static bool is_space(char c) { return c == ' ' || c == '\t'; }

static const char *skip_space(const char *p)
{
    while (*p && is_space(*p)) p++;
    return p;
}

/* Trim trailing blanks in place; returns the new length. */
static size_t rtrim(char *s, size_t n)
{
    while (n && is_space(s[n - 1])) s[--n] = '\0';
    return n;
}

static bool key_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static void line_free(IniLine *l)
{
    free(l->raw);
    free(l->section);
    free(l->key);
    free(l->str);
    free(l->comment);
    free(l->indent);
    free(l->pad_eq);
    free(l->pad_val);
    free(l->pad_cmt);
    for (size_t i = 0; i < l->nelem; i++) free(l->elem[i]);
    free(l->elem);
    free(l->etype);
}

static IniLine *push_line(P2500Ini *ini)
{
    if (ini->n == ini->cap) {
        size_t cap = ini->cap ? ini->cap * 2 : 16;
        IniLine *g = (IniLine *)realloc(ini->line, cap * sizeof *g);
        if (!g) return NULL;
        ini->line = g;
        ini->cap = cap;
    }
    IniLine *l = &ini->line[ini->n++];
    memset(l, 0, sizeof *l);
    return l;
}

static void note_error(P2500Ini *ini, size_t lineno, const char *what)
{
    ini->nerr++;
    if (ini->nerr_kept >= MAX_ERRORS) return;
    char buf[160];
    snprintf(buf, sizeof buf, "line %zu: %s", lineno, what);
    char *m = dup_s(buf);
    if (m) ini->err[ini->nerr_kept++] = m;
}

/* --- value parsing -------------------------------------------------------- */

/* Decode a double-quoted string body into a fresh buffer. Returns NULL on a
 * bad escape or a missing closing quote. *end is left on the closing quote. */
static char *decode_string(const char *p, const char **end)
{
    if (*p != '"') return NULL;
    const char *s = p + 1;
    size_t cap = strlen(s) + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t n = 0;
    for (;;) {
        if (!*s) { free(out); return NULL; }        /* unterminated */
        if (*s == '"') { out[n] = '\0'; *end = s; return out; }
        if (*s == '\\') {
            s++;
            switch (*s) {
            case '\\': out[n++] = '\\'; break;
            case '"':  out[n++] = '"';  break;
            case 'n':  out[n++] = '\n'; break;
            case 't':  out[n++] = '\t'; break;
            default: free(out); return NULL;
            }
            s++;
        } else {
            out[n++] = *s++;
        }
    }
}

/* A scalar, already trimmed, with no trailing comment. Fills the line's
 * value fields. Returns NULL on success, or a message. */
static const char *parse_scalar(IniLine *l, const char *v, size_t len)
{
    if (!len) return "no value";
    if (v[0] == '"') {
        const char *end = NULL;
        char *s = decode_string(v, &end);
        if (!s) return "unterminated or badly escaped string";
        if ((size_t)(end - v) != len - 1) { free(s); return "trailing text after a string"; }
        l->type = P2500_INI_STRING;
        l->str = s;
        return NULL;
    }
    if (v[0] == '\'') return "single-quoted strings are not in the subset";
    if (len == 4 && !memcmp(v, "true", 4))  { l->type = P2500_INI_BOOL; l->b = true;  return NULL; }
    if (len == 5 && !memcmp(v, "false", 5)) { l->type = P2500_INI_BOOL; l->b = false; return NULL; }
    if (v[0] == '{') return "inline tables are not in the subset";

    /* Numbers. strtol first so an integer stays an integer - a port number
     * read back as a double and reformatted would be a nasty surprise. */
    char *tmp = dup_n(v, len);
    if (!tmp) return "out of memory";
    char *endp = NULL;
    errno = 0;
    /* Base explicitly: strtol's base 0 reads a leading zero as octal, and
     * `08` silently meaning a parse error is not a trap worth keeping.
     * TOML's integers are decimal or `0x`, and so are ours. */
    const char *digits = tmp + ((tmp[0] == '+' || tmp[0] == '-') ? 1 : 0);
    const int base = (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) ? 16 : 10;
    long iv = strtol(tmp, &endp, base);
    if (endp && *endp == '\0' && endp != tmp && errno != ERANGE) {
        l->type = P2500_INI_INT;
        l->i = iv;
        l->f = (double)iv;
        free(tmp);
        return NULL;
    }
    errno = 0;
    double dv = strtod(tmp, &endp);
    if (endp && *endp == '\0' && endp != tmp && errno != ERANGE) {
        l->type = P2500_INI_FLOAT;
        l->f = dv;
        free(tmp);
        return NULL;
    }
    free(tmp);
    return "not a string, number or boolean";
}

static bool array_push(IniLine *l, char *text, P2500IniType t)
{
    char **g = (char **)realloc(l->elem, (l->nelem + 1) * sizeof *g);
    if (!g) return false;
    l->elem = g;
    P2500IniType *gt = (P2500IniType *)realloc(l->etype, (l->nelem + 1) * sizeof *gt);
    if (!gt) return false;
    l->etype = gt;
    l->elem[l->nelem] = text;
    l->etype[l->nelem] = t;
    l->nelem++;
    return true;
}

/* `[a, b, c]`, one line, trailing comma allowed. */
static const char *parse_array(IniLine *l, const char *v, size_t len)
{
    if (v[len - 1] != ']') return "unterminated array";
    l->type = P2500_INI_ARRAY;
    const char *p = v + 1, *lim = v + len - 1;
    for (;;) {
        p = skip_space(p);
        if (p >= lim) break;
        const char *start = p;
        if (*p == '"') {
            const char *end = NULL;
            char *s = decode_string(p, &end);
            if (!s) return "unterminated or badly escaped string in an array";
            if (!array_push(l, s, P2500_INI_STRING)) { free(s); return "out of memory"; }
            p = end + 1;
        } else if (*p == '[') {
            return "nested arrays are not in the subset";
        } else {
            while (p < lim && *p != ',') p++;
            size_t n = (size_t)(p - start);
            while (n && is_space(start[n - 1])) n--;
            if (!n) return "empty array element";
            IniLine probe;
            memset(&probe, 0, sizeof probe);
            const char *why = parse_scalar(&probe, start, n);
            if (why) { line_free(&probe); return why; }
            char *text = dup_n(start, n);
            if (!text) { line_free(&probe); return "out of memory"; }
            P2500IniType t = probe.type;
            line_free(&probe);
            if (!array_push(l, text, t)) { free(text); return "out of memory"; }
        }
        p = skip_space(p);
        if (p >= lim) break;
        if (*p != ',') return "expected a comma between array elements";
        p++;
    }
    return NULL;
}

/* --- parsing -------------------------------------------------------------- */

/* Find where the value ends and a trailing comment begins. A '#' inside a
 * string or inside an array brackets is data, not a comment. */
static size_t value_end(const char *v)
{
    bool in_str = false;
    int depth = 0;
    for (size_t i = 0; v[i]; i++) {
        if (in_str) {
            if (v[i] == '\\' && v[i + 1]) { i++; continue; }
            if (v[i] == '"') in_str = false;
            continue;
        }
        if (v[i] == '"') { in_str = true; continue; }
        if (v[i] == '[') depth++;
        else if (v[i] == ']') { if (depth) depth--; }
        else if (v[i] == '#' && depth == 0) return i;
    }
    return strlen(v);
}

P2500Ini *p2500_ini_new(void)
{
    P2500Ini *ini = (P2500Ini *)calloc(1, sizeof *ini);
    return ini;
}

P2500Ini *p2500_ini_parse(const char *text, size_t len)
{
    P2500Ini *ini = p2500_ini_new();
    if (!ini) return NULL;
    char *section = dup_s("");
    if (!section) { free(ini); return NULL; }

    /* pos < len, so an empty document has no lines and a file with no
     * trailing newline has no phantom empty one at the end. Writing it back
     * does add the missing newline, which is a fix rather than a loss. */
    size_t pos = 0, lineno = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') end++;
        size_t rawlen = end - pos;
        if (rawlen && text[pos + rawlen - 1] == '\r') rawlen--;
        lineno++;

        IniLine *l = push_line(ini);
        if (!l) break;
        l->raw = dup_n(text + pos, rawlen);
        l->section = dup_s(section);
        if (!l->raw || !l->section) break;

        const char *body = skip_space(l->raw);
        if (!*body || *body == '#') {
            /* blank or comment: verbatim, nothing more to know */
        } else if (*body == '[' && body[1] != '[') {
            const char *close = strchr(body, ']');
            if (!close) {
                note_error(ini, lineno, "section header without a closing ]");
            } else {
                size_t n = (size_t)(close - body - 1);
                char *name = dup_n(body + 1, n);
                if (name) {
                    size_t m = rtrim(name, n);
                    const char *start = skip_space(name);
                    char *trimmed = dup_n(start, m - (size_t)(start - name));
                    free(name);
                    if (trimmed) {
                        free(section);
                        section = trimmed;
                        free(l->section);
                        l->section = dup_s(section);
                        l->is_header = true;
                    }
                }
            }
        } else if (*body == '[') {
            note_error(ini, lineno, "arrays of tables are not in the subset");
        } else {
            const char *eq = strchr(body, '=');
            if (!eq) {
                note_error(ini, lineno, "expected key = value");
            } else {
                size_t klen = (size_t)(eq - body);
                while (klen && is_space(body[klen - 1])) klen--;
                bool ok = klen > 0;
                for (size_t i = 0; i < klen; i++) if (!key_char(body[i])) ok = false;
                if (!ok) {
                    note_error(ini, lineno,
                               klen && memchr(body, '.', klen)
                                   ? "dotted keys are not in the subset"
                                   : "not a bare key");
                } else {
                    const char *v = skip_space(eq + 1);
                    size_t vlen = value_end(v);
                    size_t trimmed = vlen;
                    while (trimmed && is_space(v[trimmed - 1])) trimmed--;
                    const char *why;
                    if (!trimmed) why = "no value";
                    else if (v[0] == '[') why = parse_array(l, v, trimmed);
                    else why = parse_scalar(l, v, trimmed);
                    if (why) {
                        note_error(ini, lineno, why);
                        /* the parsed-value fields are abandoned, the raw
                         * line is kept, and nothing can read the key */
                        free(l->str); l->str = NULL;
                        for (size_t i = 0; i < l->nelem; i++) free(l->elem[i]);
                        free(l->elem); l->elem = NULL;
                        free(l->etype); l->etype = NULL;
                        l->nelem = 0;
                    } else {
                        l->key = dup_n(body, klen);
                        l->indent = dup_n(l->raw, (size_t)(body - l->raw));
                        l->pad_eq = dup_n(body + klen, (size_t)(eq - body) - klen);
                        l->pad_val = dup_n(eq + 1, (size_t)(v - (eq + 1)));
                        l->pad_cmt = dup_n(v + trimmed, vlen - trimmed);
                        if (v[vlen]) l->comment = dup_s(v + vlen);
                        if (l->comment) rtrim(l->comment, strlen(l->comment));
                    }
                }
            }
        }
        pos = end + 1;
    }
    free(section);
    return ini;
}

P2500Ini *p2500_ini_load(const char *path, bool *missing)
{
    if (missing) *missing = false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (missing && errno == ENOENT) *missing = true;
        return NULL;
    }
    char *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n == cap) {
            size_t g = cap ? cap * 2 : 4096;
            char *p = (char *)realloc(buf, g);
            if (!p) { free(buf); fclose(f); return NULL; }
            buf = p;
            cap = g;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) break;
    }
    fclose(f);
    P2500Ini *ini = p2500_ini_parse(buf ? buf : "", n);
    free(buf);
    return ini;
}

size_t p2500_ini_errors(const P2500Ini *ini, const char **first)
{
    if (!ini) return 0;
    if (first && ini->nerr_kept) *first = ini->err[0];
    return ini->nerr;
}

const char *p2500_ini_error_at(const P2500Ini *ini, size_t i)
{
    if (!ini || i >= ini->nerr_kept) return NULL;
    return ini->err[i];
}

void p2500_ini_free(P2500Ini *ini)
{
    if (!ini) return;
    for (size_t i = 0; i < ini->n; i++) line_free(&ini->line[i]);
    free(ini->line);
    for (size_t i = 0; i < ini->nerr_kept; i++) free(ini->err[i]);
    free(ini);
}

/* --- rendering ------------------------------------------------------------ */

char *p2500_ini_render(const P2500Ini *ini, size_t *len)
{
    size_t need = 1;
    for (size_t i = 0; i < ini->n; i++) need += strlen(ini->line[i].raw) + 1;
    char *out = (char *)malloc(need);
    if (!out) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < ini->n; i++) {
        size_t m = strlen(ini->line[i].raw);
        memcpy(out + n, ini->line[i].raw, m);
        n += m;
        out[n++] = '\n';
    }
    out[n] = '\0';
    if (len) *len = n;
    return out;
}

/* Create the parent directory of `path`, and its parents. mkdir only, no
 * shelling out: this runs on Windows too, where `mkdir -p` does not exist. */
static void make_parent_dirs(const char *path)
{
    char *copy = dup_s(path);
    if (!copy) return;
    for (char *p = copy + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char sep = *p;
        *p = '\0';
#ifdef _WIN32
        _mkdir(copy);
#else
        mkdir(copy, 0755);
#endif
        *p = sep;
    }
    free(copy);
}

bool p2500_ini_save(const P2500Ini *ini, const char *path)
{
    size_t len = 0;
    char *text = p2500_ini_render(ini, &len);
    if (!text) return false;
    make_parent_dirs(path);
    /* Write a temporary beside it and rename: a crash mid-write must not
     * leave a truncated settings file where a good one was. */
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(text); return false; }
    bool ok = fwrite(text, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    free(text);
    if (!ok) { remove(tmp); return false; }
    remove(path);                   /* Windows rename() will not replace */
    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}

/* --- lookups -------------------------------------------------------------- */

static const IniLine *find(const P2500Ini *ini, const char *section, const char *key)
{
    if (!ini || !key) return NULL;
    if (!section) section = "";
    for (size_t i = 0; i < ini->n; i++) {
        const IniLine *l = &ini->line[i];
        if (!l->key) continue;
        if (strcmp(l->key, key) || strcmp(l->section, section)) continue;
        return l;
    }
    return NULL;
}

bool p2500_ini_type(const P2500Ini *ini, const char *section, const char *key,
                    P2500IniType *out)
{
    const IniLine *l = find(ini, section, key);
    if (!l) return false;
    if (out) *out = l->type;
    return true;
}

bool p2500_ini_get_str(const P2500Ini *ini, const char *section, const char *key,
                       const char **out)
{
    const IniLine *l = find(ini, section, key);
    if (!l || l->type != P2500_INI_STRING) return false;
    if (out) *out = l->str;
    return true;
}

bool p2500_ini_get_int(const P2500Ini *ini, const char *section, const char *key,
                       long *out)
{
    const IniLine *l = find(ini, section, key);
    if (!l || l->type != P2500_INI_INT) return false;
    if (out) *out = l->i;
    return true;
}

bool p2500_ini_get_float(const P2500Ini *ini, const char *section, const char *key,
                         double *out)
{
    const IniLine *l = find(ini, section, key);
    if (!l) return false;
    if (l->type != P2500_INI_FLOAT && l->type != P2500_INI_INT) return false;
    if (out) *out = l->f;
    return true;
}

bool p2500_ini_get_bool(const P2500Ini *ini, const char *section, const char *key,
                        bool *out)
{
    const IniLine *l = find(ini, section, key);
    if (!l || l->type != P2500_INI_BOOL) return false;
    if (out) *out = l->b;
    return true;
}

size_t p2500_ini_array_len(const P2500Ini *ini, const char *section, const char *key)
{
    const IniLine *l = find(ini, section, key);
    if (!l || l->type != P2500_INI_ARRAY) return 0;
    return l->nelem;
}

const char *p2500_ini_array_str(const P2500Ini *ini, const char *section,
                                const char *key, size_t i)
{
    const IniLine *l = find(ini, section, key);
    if (!l || l->type != P2500_INI_ARRAY || i >= l->nelem) return NULL;
    return l->elem[i];
}

bool p2500_ini_array_int(const P2500Ini *ini, const char *section, const char *key,
                         size_t i, long *out)
{
    const IniLine *l = find(ini, section, key);
    if (!l || l->type != P2500_INI_ARRAY || i >= l->nelem) return false;
    if (l->etype[i] != P2500_INI_INT) return false;
    char *endp = NULL;
    const char *d = l->elem[i] + ((l->elem[i][0] == '+' || l->elem[i][0] == '-') ? 1 : 0);
    const int base = (d[0] == '0' && (d[1] == 'x' || d[1] == 'X')) ? 16 : 10;
    long v = strtol(l->elem[i], &endp, base);
    if (!endp || *endp) return false;
    if (out) *out = v;
    return true;
}

/* --- writes --------------------------------------------------------------- */

/* Quote a string for output, growing a caller-freed buffer. */
static char *quote(const char *s)
{
    size_t n = strlen(s);
    char *out = (char *)malloc(n * 2 + 3);
    if (!out) return NULL;
    size_t k = 0;
    out[k++] = '"';
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '\\': out[k++] = '\\'; out[k++] = '\\'; break;
        case '"':  out[k++] = '\\'; out[k++] = '"';  break;
        case '\n': out[k++] = '\\'; out[k++] = 'n';  break;
        case '\t': out[k++] = '\\'; out[k++] = 't';  break;
        default:   out[k++] = s[i];
        }
    }
    out[k++] = '"';
    out[k] = '\0';
    return out;
}

/* Re-render a key line from its parts. */
static bool rebuild(IniLine *l, const char *value)
{
    const char *ind = l->indent ? l->indent : "";
    const char *pe = l->pad_eq ? l->pad_eq : "";
    const char *pv = l->pad_val ? l->pad_val : " ";
    const char *pc = l->comment ? (l->pad_cmt && *l->pad_cmt ? l->pad_cmt : " ") : "";
    const char *cmt = l->comment ? l->comment : "";
    size_t need = strlen(ind) + strlen(l->key) + strlen(pe) + strlen(pv) +
                  strlen(value) + strlen(pc) + strlen(cmt) + 2;
    char *raw = (char *)malloc(need);
    if (!raw) return false;
    snprintf(raw, need, "%s%s%s=%s%s%s%s", ind, l->key, pe, pv, value, pc, cmt);
    free(l->raw);
    l->raw = raw;
    return true;
}

/* Where a new key for `section` goes: just after the section's last
 * non-blank line, so appended keys gather under their header rather than
 * after whatever trailing blank line the author left.
 *
 * Returns (size_t)-1, NOT ini->n, when the section does not exist yet - the
 * two are the same number whenever the section happens to be the last one
 * in the file, and conflating them grows a duplicate header per key. */
static size_t insert_point(P2500Ini *ini, const char *section)
{
    size_t last = (size_t)-1;
    bool seen = false;
    for (size_t i = 0; i < ini->n; i++) {
        if (strcmp(ini->line[i].section, section)) continue;
        seen = true;
        const char *body = skip_space(ini->line[i].raw);
        if (*body) last = i;
    }
    if (!seen) return (size_t)-1;
    return last == (size_t)-1 ? ini->n : last + 1;
}

static bool insert_at(P2500Ini *ini, size_t at, const char *raw, const char *section,
                      const char *key, bool header)
{
    if (!push_line(ini)) return false;
    /* push_line appended; shuffle it into place. */
    IniLine tmp = ini->line[ini->n - 1];
    for (size_t i = ini->n - 1; i > at; i--) ini->line[i] = ini->line[i - 1];
    ini->line[at] = tmp;
    IniLine *l = &ini->line[at];
    memset(l, 0, sizeof *l);
    l->raw = dup_s(raw);
    l->section = dup_s(section);
    l->is_header = header;
    if (key) {
        l->key = dup_s(key);
        l->indent = dup_s("");
        if (!l->key || !l->indent) return false;
    }
    return l->raw && l->section;
}

/* Set a key to an already-rendered value text. */
static bool set_raw(P2500Ini *ini, const char *section, const char *key,
                    const char *value, P2500IniType type)
{
    if (!ini || !key || !value) return false;
    if (!section) section = "";
    for (size_t i = 0; i < ini->n; i++) {
        IniLine *l = &ini->line[i];
        if (!l->key || strcmp(l->key, key) || strcmp(l->section, section)) continue;
        l->type = type;
        return rebuild(l, value);
    }
    size_t at = insert_point(ini, section);
    if (at == (size_t)-1 && *section) {
        /* A new section: a blank separator, then the header. Not a second
         * one when the document already ends in a blank line - a generated
         * file that grows a gap every time it gains a section would look
         * like a bug to the person reading it. */
        const char *tail = ini->n ? skip_space(ini->line[ini->n - 1].raw) : "x";
        if (ini->n && *tail && !insert_at(ini, ini->n, "", section, NULL, false)) return false;
        char hdr[256];
        snprintf(hdr, sizeof hdr, "[%s]", section);
        if (!insert_at(ini, ini->n, hdr, section, NULL, true)) return false;
        at = ini->n;
    } else if (at == (size_t)-1) {
        at = ini->n;        /* a top-level key in a document with no lines */
    }
    char line[1024];
    snprintf(line, sizeof line, "%s = %s", key, value);
    if (!insert_at(ini, at, line, section, key, false)) return false;
    ini->line[at].type = type;
    return true;
}

/* The parsed value fields have to agree with the text, since a get after a
 * set must see the new value. Re-parse the rendered text rather than
 * duplicating the conversions. */
static bool set_and_reparse(P2500Ini *ini, const char *section, const char *key,
                            const char *value, P2500IniType type)
{
    if (!set_raw(ini, section, key, value, type)) return false;
    IniLine *l = NULL;
    if (!section) section = "";
    for (size_t i = 0; i < ini->n; i++) {
        IniLine *c = &ini->line[i];
        if (c->key && !strcmp(c->key, key) && !strcmp(c->section, section)) { l = c; break; }
    }
    if (!l) return false;
    free(l->str); l->str = NULL;
    for (size_t i = 0; i < l->nelem; i++) free(l->elem[i]);
    free(l->elem); l->elem = NULL;
    free(l->etype); l->etype = NULL;
    l->nelem = 0;
    const char *why = (value[0] == '[') ? parse_array(l, value, strlen(value))
                                        : parse_scalar(l, value, strlen(value));
    return why == NULL;
}

bool p2500_ini_set_str(P2500Ini *ini, const char *section, const char *key,
                       const char *value)
{
    char *q = quote(value);
    if (!q) return false;
    bool ok = set_and_reparse(ini, section, key, q, P2500_INI_STRING);
    free(q);
    return ok;
}

bool p2500_ini_set_int(P2500Ini *ini, const char *section, const char *key, long value)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", value);
    return set_and_reparse(ini, section, key, buf, P2500_INI_INT);
}

bool p2500_ini_set_float(P2500Ini *ini, const char *section, const char *key, double value)
{
    /* %g, then a ".0" when it came out looking like an integer: TOML reads a
     * bare 1 as an integer, and a float field that changes type depending on
     * its value is a trap for anyone parsing this with a real library. */
    char buf[40];
    snprintf(buf, sizeof buf, "%g", value);
    if (!strpbrk(buf, ".eEn")) {       /* n catches inf/nan, which %g may emit */
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof buf - n, ".0");
    }
    return set_and_reparse(ini, section, key, buf, P2500_INI_FLOAT);
}

bool p2500_ini_set_bool(P2500Ini *ini, const char *section, const char *key, bool value)
{
    return set_and_reparse(ini, section, key, value ? "true" : "false", P2500_INI_BOOL);
}

bool p2500_ini_set_array_str(P2500Ini *ini, const char *section, const char *key,
                             const char *const *values, size_t n)
{
    size_t need = 3;
    for (size_t i = 0; i < n; i++) need += strlen(values[i]) * 2 + 5;
    char *text = (char *)malloc(need);
    if (!text) return false;
    size_t k = 0;
    text[k++] = '[';
    for (size_t i = 0; i < n; i++) {
        char *q = quote(values[i]);
        if (!q) { free(text); return false; }
        if (i) { text[k++] = ','; text[k++] = ' '; }
        size_t m = strlen(q);
        memcpy(text + k, q, m);
        k += m;
        free(q);
    }
    text[k++] = ']';
    text[k] = '\0';
    bool ok = set_and_reparse(ini, section, key, text, P2500_INI_ARRAY);
    free(text);
    return ok;
}

bool p2500_ini_append_comment(P2500Ini *ini, const char *text)
{
    if (!ini) return false;
    if (!text) return insert_at(ini, ini->n, "", "", NULL, false);
    char line[1024];
    snprintf(line, sizeof line, "# %s", text);
    /* A comment belongs to the top of the file, where this is used, so it
     * carries the top-level section rather than whatever came last. */
    return insert_at(ini, ini->n, line, ini->n ? ini->line[ini->n - 1].section : "",
                     NULL, false);
}

const char *p2500_ini_requires_unmet(const P2500Ini *ini, const char *const *known,
                                     size_t nknown)
{
    size_t n = p2500_ini_array_len(ini, "", "requires");
    for (size_t i = 0; i < n; i++) {
        const char *want = p2500_ini_array_str(ini, "", "requires", i);
        if (!want) continue;
        bool have = false;
        for (size_t k = 0; k < nknown; k++)
            if (!strcmp(want, known[k])) { have = true; break; }
        if (!have) return want;
    }
    return NULL;
}
