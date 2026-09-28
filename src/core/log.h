#ifndef P2500_LOG_H
#define P2500_LOG_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Diagnostics out of the core, through the caller (TODO.md T34).
 *
 * The core used to write its 63 diagnostics straight to stderr. That is fine
 * for one front-end and useless for every other: a GUI log panel, a MAME
 * `logerror()`, a browser console and a test harness all want the same
 * messages and none of them is stderr. The message text is unchanged - the
 * `[cat]` prefix each line used to carry is now the `category` argument, and
 * the CLI's sink puts it back, so output is byte-for-byte what it was.
 *
 * Levels do not gate anything here. Each device keeps its own `verbose`
 * switch, which is what decides whether a message is produced at all; the
 * level is for the sink to colour or filter by.
 */

typedef enum {
    /* The emulator declined to act, or acted on something it does not
     * model. The project's most expensive bugs were all silent drops, so
     * these are the lines that matter most. */
    P2500_LOG_WARN = 0,
    /* A state change worth seeing once. */
    P2500_LOG_INFO,
    /* The per-access firehose behind the `verbose` switches. */
    P2500_LOG_TRACE,
} P2500LogLevel;

typedef void (*P2500LogFn)(void *userdata, P2500LogLevel level,
                           const char *category, const char *message);

typedef struct {
    P2500LogFn fn;
    void *userdata;
} P2500Log;

/* Formats and hands the message to the sink. A NULL log, or one with no
 * function installed, discards it - which is what makes the core usable
 * with no front-end at all. `message` carries no trailing newline; adding
 * one is the sink's business, because a GUI list does not want it. */
void p2500_logf(const P2500Log *log, P2500LogLevel level, const char *category,
                const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 4, 5)))
#endif
    ;

#ifdef __cplusplus
}
#endif

#endif
