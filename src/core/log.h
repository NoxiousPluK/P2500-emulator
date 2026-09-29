#ifndef P2500_LOG_H
#define P2500_LOG_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Diagnostics out of the core, through the caller.
 *
 * Writing diagnostics straight to stderr would be fine for one front-end
 * and useless for every other: a GUI log panel, a MAME `logerror()`, a
 * browser console and a test harness all want the same messages and none
 * of them is stderr. Each message carries a `category` argument (a short
 * tag such as `[cat]`) so a sink can format it however it needs to.
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
#if defined(__MINGW32__)
    /* mingw's own runtime supports the C99 length modifiers (%zu etc.);
     * only GCC's format-checker needs telling, since its default "printf"
     * checking there means MSVCRT's, not the runtime actually in use. */
    __attribute__((format(gnu_printf, 4, 5)))
#elif defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

#ifdef __cplusplus
}
#endif

#endif
