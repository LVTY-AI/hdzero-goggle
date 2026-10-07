#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/rtc.h"

// Exercise recording configuration without hardware or disk writes.
void test_sync(void);
void test_get_clock(struct rtc_date *rd);
int test_has_valid_date(const struct rtc_date *rd);
#define sync test_sync
#define rtc_get_clock test_get_clock
#define rtc_has_valid_date test_has_valid_date
#include "../../src/core/dvr.c"
#undef sync
#undef rtc_get_clock
#undef rtc_has_valid_date

setting_t g_setting;
hw_status_t g_hw_stat;
source_info_t g_source_info;
video_resolution_t CAM_MODE = VR_720P60;

static struct rtc_date current_clock;
static int clock_valid = 1;
static int naming_writes;
static int recording_naming;
static char recording_type[8];

void test_get_clock(struct rtc_date *rd) {
    *rd = current_clock;
}

int test_has_valid_date(const struct rtc_date *rd) {
    return clock_valid ? 0 : -1;
}

int ini_putl(const char *section, const char *key, long value, const char *filename) {
    if (strcmp(section, "record") == 0 && strcmp(key, "naming") == 0) {
        if (strcmp(filename, SETTING_INI) == 0)
            naming_writes++;
        else if (strcmp(filename, REC_CONF) == 0)
            recording_naming = value;
    }
    return 1;
}

int ini_puts(const char *section, const char *key, const char *value, const char *filename) {
    if (strcmp(key, "type") == 0)
        snprintf(recording_type, sizeof(recording_type), "%s", value);
    return 1;
}

int ini_gets(const char *section, const char *key, const char *default_value,
             char *buffer, int size, const char *filename) {
    return snprintf(buffer, size, "%s", recording_type);
}

int system_exec(const char *command) {
    return 0;
}

int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) {
    return 0;
}

void test_sync(void) {}

static void check_recording(int expected) {
    setting_record_naming_t preference = g_setting.record.naming;
    recording_naming = -1;
    dvr_update_record_conf();
    assert(recording_naming == expected);
    assert(g_setting.record.naming == preference);
    assert(naming_writes == 0);
}

int main(void) {
    const struct rtc_date unset = {.year = 1970, .month = 1, .day = 1};
    const struct rtc_date synced = {
        .year = 2026, .month = 10, .day = 7, .hour = 12, .min = 30,
    };

    g_setting.record.naming = SETTING_NAMING_DATE;
    current_clock = unset;
    check_recording(SETTING_NAMING_CONTIGUOUS);
    current_clock = synced;
    check_recording(SETTING_NAMING_DATE);

    current_clock = unset; // Losing time must not erase the preference.
    check_recording(SETTING_NAMING_CONTIGUOUS);
    current_clock = synced;
    check_recording(SETTING_NAMING_DATE);

    clock_valid = 0;
    check_recording(SETTING_NAMING_CONTIGUOUS);
    clock_valid = 1;
    check_recording(SETTING_NAMING_DATE);

    current_clock = unset;
    g_setting.record.naming = SETTING_NAMING_CONTIGUOUS;
    check_recording(SETTING_NAMING_CONTIGUOUS);
    g_setting.record.naming = SETTING_NAMING_ELRS;
    check_recording(SETTING_NAMING_ELRS);
    current_clock = synced;
    check_recording(SETTING_NAMING_ELRS);

    puts("test_dvr_naming: all checks passed");
    return 0;
}
