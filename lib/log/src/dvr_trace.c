#include "log/dvr_trace.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef DVR_TRACE_PATH
#define DVR_TRACE_PATH "/mnt/extsd/dvr_dbg.log"
#endif
#define TRACE_QUEUE_SIZE 64
#define TRACE_TEXT_SIZE 480

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "DVR trace counters require lock-free atomics");
typedef struct {
    time_t when;
    char text[TRACE_TEXT_SIZE];
} trace_record_t;

static trace_record_t queue[TRACE_QUEUE_SIZE];
static unsigned head, count;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint initialized, running, queued, dropped, errors, written;

void dvr_trace_get_stats(dvr_trace_stats_t *stats) {
    stats->queued = atomic_load_explicit(&queued, memory_order_relaxed);
    stats->dropped = atomic_load_explicit(&dropped, memory_order_relaxed);
    stats->errors = atomic_load_explicit(&errors, memory_order_relaxed);
    stats->written = atomic_load_explicit(&written, memory_order_relaxed);
}

static void store_record(const trace_record_t *record) {
    static unsigned reported_drops, reported_errors;
    unsigned lost = atomic_load_explicit(&dropped, memory_order_relaxed);
    unsigned failed = atomic_load_explicit(&errors, memory_order_relaxed);
    char line[TRACE_TEXT_SIZE + 160], timestamp[20] = "unknown-time";
    struct tm local;
    if (localtime_r(&record->when, &local))
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    int prefix = 0;
    if (lost != reported_drops || failed != reported_errors)
        prefix = snprintf(line, sizeof(line), "%s [trace] dropped=%u errors=%u\n", timestamp, lost, failed);
    size_t length = prefix + snprintf(line + prefix, sizeof(line) - prefix,
                                     "%s %s\n", timestamp, record->text);

    // Open/append/close only on this worker, never with the queue mutex held.
    // Reopen per record so an old descriptor cannot pin a removed card/mount.
    int fd = open(DVR_TRACE_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        atomic_fetch_add_explicit(&errors, 1, memory_order_relaxed);
        return;
    }
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, line + done, length - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    int closed = close(fd);
    // This is a debug trace, not recording data: never force an SD flush.
    if (done != length || closed != 0) {
        atomic_fetch_add_explicit(&errors, 1, memory_order_relaxed);
    } else {
        reported_drops = lost;
        reported_errors = failed;
        atomic_fetch_add_explicit(&written, 1, memory_order_relaxed);
    }
}

static void *trace_worker(void *unused) {
    (void)unused;
    for (;;) {
        trace_record_t record;
        pthread_mutex_lock(&queue_mutex);
        bool available = count != 0;
        if (available) {
            record = queue[head];
            head = (head + 1) % TRACE_QUEUE_SIZE;
            count--;
            atomic_store_explicit(&queued, count, memory_order_relaxed);
        }
        pthread_mutex_unlock(&queue_mutex);
        if (available) {
            store_record(&record);
        } else {
            struct timespec pause = {0, 100000000};
            nanosleep(&pause, NULL);
        }
    }
    return NULL;
}

void dvr_trace_init(void) {
    if (atomic_exchange_explicit(&initialized, 1, memory_order_relaxed))
        return;
    pthread_attr_t attr;
    pthread_t thread;
    if (pthread_attr_init(&attr) != 0) {
        atomic_fetch_add_explicit(&errors, 1, memory_order_relaxed);
        return;
    }
    int result = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (result == 0)
        result = pthread_create(&thread, &attr, trace_worker, NULL);
    pthread_attr_destroy(&attr);
    if (result == 0)
        atomic_store_explicit(&running, 1, memory_order_release);
    else
        atomic_fetch_add_explicit(&errors, 1, memory_order_relaxed);
}

void dvr_trace_vlog(const char *role, const char *fmt, va_list ap) {
    if (!atomic_load_explicit(&running, memory_order_acquire)) {
        atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
        return;
    }
    trace_record_t record = {.when = time(NULL)};
    int prefix = snprintf(record.text, sizeof(record.text), "[%.8s] ", role);
    int length = vsnprintf(record.text + prefix, sizeof(record.text) - prefix, fmt, ap);
    if (length < 0)
        return;
    if ((size_t)length >= sizeof(record.text) - prefix)
        memcpy(record.text + sizeof(record.text) - 5, "...", 4);
    if (pthread_mutex_trylock(&queue_mutex) != 0) {
        atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
        return;
    }
    if (count == TRACE_QUEUE_SIZE) {
        atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
    } else {
        queue[(head + count) % TRACE_QUEUE_SIZE] = record;
        count++;
        atomic_store_explicit(&queued, count, memory_order_relaxed);
    }
    pthread_mutex_unlock(&queue_mutex);
}
