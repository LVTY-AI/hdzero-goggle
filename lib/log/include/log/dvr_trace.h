#pragma once

#include <stdarg.h>

typedef struct {
    unsigned queued, dropped, errors, written;
} dvr_trace_stats_t;

// Start once during process startup, before DVR/input threads run. No storage I/O.
void dvr_trace_init(void);
// Best-effort bounded handoff; never waits for storage or a busy queue lock.
void dvr_trace_vlog(const char *role, const char *fmt, va_list ap);
void dvr_trace_get_stats(dvr_trace_stats_t *stats);
