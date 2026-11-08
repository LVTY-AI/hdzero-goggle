#include "log/log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define get_filename(file) (strrchr(file, '/') ? strrchr(file, '/') + 1 : file)

static int fd_log_file = -1;
static char offline_buffer[1 * 1024 * 1024];
static size_t offline_offset = 0;
static char console_buffer[64 * 1024];
static size_t console_offset;
static size_t dropped_file, dropped_console;
static atomic_bool file_opened;
static pthread_once_t log_once = PTHREAD_ONCE_INIT;
static bool worker_started;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
// Storage must never be accessed while holding the producer's buffer lock.
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *log_level_names[] = {
    "DEBUG",   // -2
    "VERBOSE", // -1
    "INFO",    // 0
    "WARN",    // 1
    "ERROR",   // 2
    "FATAL",   // 3
};

/**
 * Worker thread for updating log file on SD Card.
 */
static size_t log_write(int fd, const char *data, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    return done;
}

static size_t log_take(char *dst, char *src, size_t *offset, size_t *dropped) {
    size_t bytes = 0;
    if (*dropped) {
        bytes = snprintf(dst, 128, "[logger] dropped %zu bytes (buffer full or write failed)\n", *dropped);
        *dropped = 0;
    }
    memcpy(dst + bytes, src, *offset);
    bytes += *offset;
    *offset = 0;
    return bytes;
}

static void *log_sync_thread(void *arg) {
    static char pending[sizeof(offline_buffer) + 128];
    static char console[sizeof(console_buffer) + 128];

    while (true) {
        pthread_mutex_lock(&log_mutex);
        size_t bytes = log_take(console, console_buffer, &console_offset, &dropped_console);
        pthread_mutex_unlock(&log_mutex);

        size_t lost = bytes - log_write(STDOUT_FILENO, console, bytes);
        pthread_mutex_lock(&log_mutex);
        dropped_console += lost;
        pthread_mutex_unlock(&log_mutex);

        pthread_mutex_lock(&file_mutex);
        if (fd_log_file >= 0) {
            pthread_mutex_lock(&log_mutex);
            bytes = log_take(pending, offline_buffer, &offline_offset, &dropped_file);
            pthread_mutex_unlock(&log_mutex);
            lost = bytes - log_write(fd_log_file, pending, bytes);
            pthread_mutex_lock(&log_mutex);
            dropped_file += lost;
            pthread_mutex_unlock(&log_mutex);
        }
        pthread_mutex_unlock(&file_mutex);
        sleep(1);
    }
}

/**
 * Once initialized detach until completed.
 */
static void log_init_thread(void) {
    pthread_t tid;
    if (!pthread_create(&tid, NULL, log_sync_thread, NULL)) {
        worker_started = true;
        pthread_detach(tid);
    } else {
        dprintf(STDERR_FILENO, "[logger] worker unavailable; using console only\n");
    }
}

static void log_enqueue(char *dst, size_t capacity, size_t *offset, size_t *dropped,
                        const char *data, size_t size) {
    if (*offset + size > capacity) {
        *dropped += *offset;
        *offset = 0;
    }
    memcpy(dst + *offset, data, size);
    *offset += size;
}

int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) {
    pthread_once(&log_once, log_init_thread);

    char buf[1024], buffer[1280];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Timestamp
    struct timeval now;
    gettimeofday(&now, NULL);
    int milli = now.tv_usec / 1000;
    char timestamp[sizeof "YYYY-MM-DD HH:MM:SS.sss"];
    struct tm tm;
    gmtime_r(&now.tv_sec, &tm);
    int bytes = strftime(timestamp, sizeof timestamp, "%F %T.", &tm);
    snprintf(&timestamp[bytes], sizeof(timestamp) - bytes, "%03d", milli);

    bytes =
        snprintf(buffer,
                 sizeof(buffer),
                 "%s [%s][%s:%s:%d] %s\r\n",
                 timestamp,
                 log_level_names[level + 2],
                 get_filename(file),
                 func,
                 line,
                 buf);

    size_t length = strnlen(buffer, sizeof(buffer) - 1);
    if (!worker_started)
        return (int)log_write(STDOUT_FILENO, buffer, length);
    pthread_mutex_lock(&log_mutex);
    log_enqueue(offline_buffer, sizeof(offline_buffer), &offline_offset, &dropped_file, buffer, length);
    log_enqueue(console_buffer, sizeof(console_buffer), &console_offset, &dropped_console, buffer, length);

    pthread_mutex_unlock(&log_mutex);

    return (int)length;
}

bool log_file_opened() {
    return atomic_load(&file_opened);
}

bool log_file_open(const char *filename) {
    pthread_once(&log_once, log_init_thread);
    if (!worker_started)
        return false;
    int fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return false;
    pthread_mutex_lock(&file_mutex);
    if (fd_log_file >= 0) {
        close(fd_log_file);
    }
    fd_log_file = fd;
    atomic_store(&file_opened, true);
    pthread_mutex_unlock(&file_mutex);
    return true;
}

void log_file_close() {
    pthread_mutex_lock(&file_mutex);

    if (fd_log_file >= 0) {
        close(fd_log_file);
        fd_log_file = -1;
    }
    atomic_store(&file_opened, false);
    pthread_mutex_unlock(&file_mutex);
}
