// rtc_hw_emu.c - Emulator backend for the physical clock (see rtc_hw.h).
//
// No /dev/rtc on a dev box, so the clock is just an in-memory value the app sets
// and reads back. Starts at the epoch so rtc_init() treats it as "no battery" and
// seeds it from settings, exactly like a fresh goggle. Compiled instead of
// src/driver/rtc_hw.c when EMULATOR_BUILD.
#ifdef EMULATOR_BUILD

#include "rtc_hw.h"
#include "rtc.h"

#include <time.h>

static struct rtc_date g_rtc_date = {1970, 1, 1, 0, 0, 0};
static struct timespec g_rtc_set_at;
static int g_rtc_was_set;

void rtc_hw_read(struct rtc_date *rd) {
    *rd = g_rtc_date;
    if (!g_rtc_was_set) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    time_t elapsed = now.tv_sec - g_rtc_set_at.tv_sec;
    if (now.tv_nsec < g_rtc_set_at.tv_nsec) elapsed--;
    if (elapsed <= 0) return;

    struct timeval tv;
    rtc_rd2tv(&g_rtc_date, &tv);
    tv.tv_sec += elapsed;
    rtc_tv2rd(&tv, rd);
}

int rtc_hw_write(const struct rtc_date *rd) {
    g_rtc_date = *rd;
    clock_gettime(CLOCK_MONOTONIC, &g_rtc_set_at);
    g_rtc_was_set = 1;
    return 1;
}

void rtc_hw_log_devices(void) {
}

#endif // EMULATOR_BUILD
