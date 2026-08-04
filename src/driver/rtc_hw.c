// rtc_hw.c - Goggle backend for the physical clock (see rtc_hw.h).
//
// Reads/writes /dev/rtc via the kernel rtc ioctls and mirrors the wall clock into
// the OS via settimeofday(). The struct rtc_time <-> struct rtc_date conversions
// live here because only this hardware path needs the kernel type; the shared
// rtc.c deals purely in struct rtc_date. The emulator supplies rtc_hw_emu.c instead.
#include "rtc_hw.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h> // settimeofday, struct timeval
#include <unistd.h>

#include <log/log.h>

#include "rtc.h" // rtc_rd2tv

#define RTC_DEV_MAX 4
#define RTC_DEV_FALLBACK "/dev/rtc"

static int rtc_open_dev(int index, int flags) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/rtc%d", index);
    return open(path, flags);
}

static void rd2rt(const struct rtc_date *rd, struct rtc_time *rt) {
    rt->tm_year = rd->year - 1900;
    rt->tm_mon = rd->month - 1;
    rt->tm_mday = rd->day;
    rt->tm_hour = rd->hour;
    rt->tm_min = rd->min;
    rt->tm_sec = rd->sec;
}

static void rt2rd(const struct rtc_time *rt, struct rtc_date *rd) {
    rd->year = rt->tm_year + 1900;
    rd->month = rt->tm_mon + 1;
    rd->day = rt->tm_mday;
    rd->hour = rt->tm_hour;
    rd->min = rt->tm_min;
    rd->sec = rt->tm_sec;
}

void rtc_hw_read(struct rtc_date *rd) {
    // Some boards expose both a volatile SoC RTC and a battery-backed chip.
    // Read all available nodes and keep the latest valid value so a reset
    // volatile clock cannot hide the time preserved by the external RTC.
    uint64_t best = 0;
    int found = 0;
    for (int i = 0; i < RTC_DEV_MAX; ++i) {
        int fd = rtc_open_dev(i, O_RDONLY);
        if (fd < 0) continue;
        found++;
        struct rtc_time rt;
        if (ioctl(fd, RTC_RD_TIME, &rt) == 0) {
            struct rtc_date candidate;
            rt2rd(&rt, &candidate);
            struct timeval tv;
            rtc_rd2tv(&candidate, &tv);
            if (rtc_has_valid_date(&candidate) == 0 &&
                (best == 0 || (uint64_t)tv.tv_sec > best)) {
                best = (uint64_t)tv.tv_sec;
                *rd = candidate;
            }
        } else {
            LOGE("rtc_hw_read: RTC_RD_TIME on /dev/rtc%d failed with errno(%d)", i, errno);
        }
        close(fd);
    }

    if (found == 0) {
        int fd = open(RTC_DEV_FALLBACK, O_RDONLY);
        if (fd >= 0) {
            struct rtc_time rt;
            if (ioctl(fd, RTC_RD_TIME, &rt) == 0) {
                rt2rd(&rt, rd);
            } else {
                LOGE("rtc_hw_read: ioctl(%d,RTC_RD_TIME,rt) failed with errno(%d)", fd, errno);
            }
            close(fd);
        } else {
            LOGE("rtc_hw_read failed to open(%s, O_RDONLY)", RTC_DEV_FALLBACK);
        }
    }
}

int rtc_hw_write(const struct rtc_date *rd) {
    struct rtc_time rt;
    struct timeval tv;
    rd2rt(rd, &rt);
    rtc_rd2tv(rd, &tv);

    if (settimeofday(&tv, NULL) != 0) {
        LOGE("settimeofday(&tv, NULL) failed with errno(%d)", errno);
    }

    int written = 0;
    for (int i = 0; i < RTC_DEV_MAX; ++i) {
        int fd = rtc_open_dev(i, O_WRONLY);
        if (fd < 0) continue;
        if (ioctl(fd, RTC_SET_TIME, &rt) != 0) {
            LOGE("rtc_hw_write: RTC_SET_TIME on /dev/rtc%d failed with errno(%d)", i, errno);
        } else {
            written++;
        }
        close(fd);
    }

    if (written == 0) {
        int fd = open(RTC_DEV_FALLBACK, O_WRONLY);
        if (fd >= 0) {
            if (ioctl(fd, RTC_SET_TIME, &rt) != 0) {
                LOGE("rtc_hw_write: ioctl(%d,RTC_SET_TIME,&rt) failed with errno(%d)", fd, errno);
            } else {
                written++;
            }
            close(fd);
        } else {
            LOGE("rtc_hw_write failed to open(%s, O_WRONLY)", RTC_DEV_FALLBACK);
        }
    }

    return written;
}

void rtc_hw_log_devices(void) {
    // Boot diagnostics identify which RTC driver is present and which device
    // actually returned a usable time after power-off.
    for (int i = 0; i < RTC_DEV_MAX; ++i) {
        char path[32], name[32] = "?";
        snprintf(path, sizeof(path), "/sys/class/rtc/rtc%d/name", i);
        FILE *fp = fopen(path, "r");
        if (fp) {
            if (fgets(name, sizeof(name), fp)) {
                name[strcspn(name, "\n")] = 0;
            }
            fclose(fp);
        }

        int fd = rtc_open_dev(i, O_RDONLY);
        if (fd < 0) {
            continue;
        }
        struct rtc_time rt;
        if (ioctl(fd, RTC_RD_TIME, &rt) == 0) {
            LOGI("rtc_init: /dev/rtc%d (%s) reads %04d-%02d-%02d %02d:%02d:%02d",
                 i, name, rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday,
                 rt.tm_hour, rt.tm_min, rt.tm_sec);
        } else {
            LOGI("rtc_init: /dev/rtc%d (%s) read failed errno(%d)", i, name, errno);
        }
        close(fd);
    }
}
