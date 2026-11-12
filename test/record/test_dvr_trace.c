#include <log/dvr_trace.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void trace(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    dvr_trace_vlog("app", fmt, ap);
    va_end(ap);
}
static dvr_trace_stats_t stats(void) {
    dvr_trace_stats_t value;
    dvr_trace_get_stats(&value);
    return value;
}
static double seconds(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    alarm(10);
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/hdz-trace-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    assert(mkdtemp(dir) && chdir(dir) == 0);
    assert(mkfifo("dvr-trace-fifo", 0600) == 0);
    trace("disabled by default");
    assert(!dvr_trace_enabled() && stats().queued == 0 && stats().dropped == 0);
    dvr_trace_set_enabled(true);
    trace("before init");
    assert(stats().dropped == 1); // No synchronous fallback without a worker.
    dvr_trace_init();
    unsigned before;
    do {
        before = stats().dropped;
        trace("stale record");
    } while (stats().dropped != before);
    while (stats().queued) usleep(1000); // Worker takes record, then blocks opening FIFO.
    double start = seconds();
    for (unsigned i = 0; i < 1024; i++) trace("queued %u", i);
    assert(seconds() - start < 1.0);
    assert(stats().queued == 64 && stats().written == 0 && stats().dropped > 900);
    dvr_trace_set_enabled(false);
    trace("disabled record");
    assert(!dvr_trace_enabled() && stats().queued == 0);
    dvr_trace_set_enabled(true);
    trace("first record");

    int reader = open("dvr-trace-fifo", O_RDONLY | O_NONBLOCK);
    assert(reader >= 0);
    char text[2048];
    int saw_first = 0, saw_loss = 0;
    for (;;) {
        ssize_t n = read(reader, text, sizeof(text) - 1);
        if (n > 0) {
            text[n] = '\0';
            assert(!strstr(text, "stale record") && !strstr(text, "queued ") && !strstr(text, "disabled record"));
            saw_first |= strstr(text, "[app] first record") != NULL;
            saw_loss |= strstr(text, "[trace] dropped=") != NULL;
        }
        if (n <= 0 && stats().written == 1) break;
        usleep(1000);
    }
    assert(saw_first && saw_loss && stats().errors == 0 && stats().queued == 0);
    assert(close(reader) == 0 && unlink("dvr-trace-fifo") == 0);
    assert(mkdir("dvr-trace-fifo", 0700) == 0);
    trace("unwritable sink");
    while (!stats().errors) usleep(1000);
    assert(stats().written == 1 && rmdir("dvr-trace-fifo") == 0);
    dvr_trace_set_enabled(false);
    trace("must not create a file");
    assert(access("dvr-trace-fifo", F_OK) != 0 && stats().queued == 0);
    assert(chdir("/") == 0 && rmdir(dir) == 0);
    puts("DVR trace: default off, live toggle, stale queue discard and bounded asynchronous storage passed");
    return 0;
}
