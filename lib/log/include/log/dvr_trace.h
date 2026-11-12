#pragma once

#include <stdarg.h>
#include <stdbool.h>

typedef struct {
    unsigned queued, dropped, errors, written;
} dvr_trace_stats_t;

// Start once during process startup, before DVR/input threads run. No storage I/O.
void dvr_trace_init(void);
// Disabled by default. Changing the switch discards queued trace records;
// an already-running storage call is never waited on by the caller.
void dvr_trace_set_enabled(bool enabled);
bool dvr_trace_enabled(void);
// Best-effort bounded handoff; never waits for storage or a busy queue lock.
void dvr_trace_vlog(const char *role, const char *fmt, va_list ap);
void dvr_trace_get_stats(dvr_trace_stats_t *stats);
