#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "core/app_state.h"
#include "core/common.hh"
#include "core/dvr.h"
#include "core/settings.h"
#include "core/sleep_mode.h"
#include "driver/fans.h"
#include "driver/rtc6715.h"

// ---- what sleep_mode.c reaches: a clock, a beeper and a goggle that does nothing ----

static uint32_t now_ms;
uint32_t time_ms() { return now_ms; }

static int beeps;
void beep_dur(int ms) { beeps++; }

setting_t g_setting;
app_state_t g_app_state;
fan_speed_t fan_speed;
static void rx_init(bool power_on, bool audio_on) {}
rtc6715_t rtc6715 = {.init = rx_init};
void app_state_push(app_state_t state) { g_app_state = state; }
void app_switch_to_menu() {}
void app_exit_menu() {}
void submenu_exit() {}
void dvr_cmd(osd_dvr_cmd_t cmd) {}
void dvr_update_vi_conf(video_resolution_t fmt) {}
void screen_set_inhibited(uint32_t reasons, bool inhibited) {}
void HDZero_Close() {}
hw_revision_t getHwRevision() { return HW_REV_2; }
void DM5680_ExternalAnalog_Power(uint8_t on) {}
void Analog_Module_Power(bool force) {}
void fans_top_setspeed(uint8_t speed) {}
void fans_left_setspeed(uint8_t speed) {}
void fans_right_setspeed(uint8_t speed) {}
int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) { return 0; }

// ---- the checks ----

static int failures;

// Sleep for `for_ms` with the main loop passing every `pass_ms`; the beeps heard.
static int beeps_asleep(uint32_t pass_ms, uint32_t for_ms) {
    go_sleep();
    beeps = 0;
    for (const uint32_t end = now_ms + for_ms; now_ms < end;) {
        now_ms += pass_ms;
        sleep_reminder();
    }
    wake_up();
    return beeps;
}

static void check_beeps(uint32_t pass_ms, uint32_t for_ms, int want, const char *what) {
    const int got = beeps_asleep(pass_ms, for_ms);
    if (got != want) {
        fprintf(stderr, "FAIL: %s: %d beeps, want %d\n", what, got, want);
        failures++;
    }
}

int main(void) {
    // A minute asleep beeps three times, however fast the main loop runs. (A
    // second over the minute: each beep can come up to one pass late.)
    check_beeps(5, 61000, 3, "a minute with a 5 ms loop");
    check_beeps(8, 61000, 3, "a minute with an 8 ms loop");
    check_beeps(16, 61000, 3, "a minute with a 16 ms loop");
    check_beeps(33, 61000, 3, "a minute with a 33 ms loop");

    // The first beep comes a full interval after falling asleep, not before.
    check_beeps(5, SLEEP_BEEP_INTERVAL_MS - 5, 0, "just short of one interval");
    check_beeps(5, SLEEP_BEEP_INTERVAL_MS, 1, "one interval");

    // Awake, the reminder is silent.
    beeps = 0;
    for (int i = 0; i < 10000; i++) {
        now_ms += 10;
        sleep_reminder();
    }
    if (beeps) {
        fprintf(stderr, "FAIL: %d beeps while awake\n", beeps);
        failures++;
    }

    // Asleep again, the interval starts over rather than running on from before.
    now_ms += 12345;
    check_beeps(5, SLEEP_BEEP_INTERVAL_MS - 5, 0, "a second sleep, just short of one interval");

    if (failures)
        return 1;
    puts("test_sleep_beep: all checks passed");
    return 0;
}
