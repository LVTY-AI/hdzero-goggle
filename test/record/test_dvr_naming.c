#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/wait.h>

#include "driver/rtc.h"

// Exercise recording configuration without hardware or disk writes.
void test_sync(void);
void test_get_clock(struct rtc_date *rd);
int test_has_valid_date(const struct rtc_date *rd);
#define sync test_sync
#define rtc_get_clock test_get_clock
#define rtc_has_valid_date test_has_valid_date
#define DVR_RECORDER_GRACE_S 1
#if defined(__linux__) // The worker insists on a tmpfs /tmp for its private config.
#define statfs(path, fs) ((fs)->f_type = 0x01021994, 0)
#endif
#include "../../src/core/dvr.c"
#undef sync
#undef rtc_get_clock
#undef rtc_has_valid_date

setting_t g_setting;
app_state_t g_app_state = APP_STATE_SLEEP;
hw_status_t g_hw_stat;
source_info_t g_source_info;
static video_resolution_t test_camera = VR_720P60;
camera_video_t camera_video_snapshot(void) { return (camera_video_t){.mode = test_camera}; }
bool g_sdcard_enable, g_sdcard_ready;
bool sdcard_is_full(void) { return false; }
void Display_Osd(bool enable) {}
uint8_t Display_Osd_Readback(void) { return 0; }

static struct rtc_date current_clock;
static int clock_valid = 1;
static int naming_writes;
static int recording_naming;
static char recording_type[8];
static int encoder_fps, encoder_h265, encoder_height, vi_fps, fail_write;

void test_get_clock(struct rtc_date *rd) {
    *rd = current_clock;
}

int test_has_valid_date(const struct rtc_date *rd) {
    return clock_valid ? 0 : -1;
}

int ini_putl(const char *section, const char *key, long value, const char *filename) {
    if (strcmp(section, "venc") == 0) {
        if (!strcmp(key, "fps")) encoder_fps = value;
        if (!strcmp(key, "h265")) encoder_h265 = value;
        if (!strcmp(key, "height")) encoder_height = value;
    }
    if (!strcmp(section, "vi") && !strcmp(key, "fps")) vi_fps = value;
    if (strcmp(section, "record") == 0 && strcmp(key, "naming") == 0) {
        if (strcmp(filename, SETTING_INI) == 0)
            naming_writes++;
        else if (strcmp(filename, REC_CONF) == 0)
            recording_naming = value;
    }
    return !fail_write;
}

int ini_puts(const char *section, const char *key, const char *value, const char *filename) {
    if (strcmp(key, "type") == 0)
        snprintf(recording_type, sizeof(recording_type), "%s", value);
    return !fail_write;
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

static unsigned continued;
static void continue_transition(uintptr_t value) {
    assert(value == 42);
    assert(!dvr_defer_transition(NULL, 0)); // Nested source/menu operations are authorized.
    continued++;
}

static void check_deferred_stop(void) {
    atomic_store(&control_phase, DVR_IDLE);
    control.desired = true;
    assert(dvr_defer_transition(continue_transition, 42));
    dvr_poll();
    assert(continued == 0 && dvr_input_blocked());
    control.stopped = control.stop;
    control.confirmed_idle = true;
    dvr_poll();
    assert(continued == 0); // A record STOP alone is not a live-capture pause ACK.
    control.paused = true;
    control.pause = false;
    atomic_store(&control_error, ETIMEDOUT);
    dvr_poll();
    assert(continued == 0); // Timeout must not authorize teardown.
    atomic_store(&control_error, 0);
    dvr_poll();
    assert(continued == 1 && !dvr_input_blocked());
}

// A stand-in recorder process on the real control queue, like the storage
// scripts' quit-then-restart: each one replaces its predecessor's queue.
static pid_t start_recorder(void) {
    pid_t pid = fork();
    if (pid) return pid;
    int queue = record_control_open(true);
    if (queue < 0) _exit(1);
    for (record_control_request_t request;;) {
        if (record_control_receive(queue, &request) == 1)
            record_control_reply(queue, &request, 0, request.command == RECORD_START);
        usleep(1000);
    }
}

static void end_recorder(pid_t pid) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

// Poll until the deferred transition runs, then let the unpause its resume
// queues land, or the next defer can find the recorder still paused.
static bool continues(unsigned before) {
    for (int i = 0; i < 600 && continued == before; i++, usleep(10000)) dvr_poll();
    for (int i = 0; i < 300; i++, usleep(10000)) {
        pthread_mutex_lock(&dvr_mutex);
        bool settled = (!control.live_stop && !control.paused) || atomic_load(&recorder_unavailable);
        pthread_mutex_unlock(&dvr_mutex);
        if (settled) break;
    }
    return continued == before + 1 && !dvr_input_blocked();
}

static bool transition_completes(unsigned before) {
    assert(dvr_defer_transition(continue_transition, 42) && dvr_input_blocked());
    return continues(before);
}

static void check_recorder_replaced(void) {
    app_state_t saved_state = g_app_state;
    g_app_state = APP_STATE_VIDEO; // So each resume queues the unpause, as on the goggles.
    memset(&control, 0, sizeof(control));
    config_file = "test_dvr_naming.c"; // Any small file stands in for the stock record.conf.
    pid_t recorder = start_recorder();
    dvr_init();
    while (recorder_pid != (unsigned)recorder) usleep(10000);
    // Idle in the menu while a storage operation replaces the recorder and its queue.
    int queue = msgget(RECORD_CONTROL_KEY, 0);
    pid_t replaced = recorder;
    recorder = start_recorder();
    while (msgget(RECORD_CONTROL_KEY, 0) == queue) usleep(1000);
    end_recorder(replaced);
    assert(transition_completes(continued));
    assert(recorder_pid == (unsigned)recorder && !atomic_load(&recorder_unavailable));
    end_recorder(recorder); // No successor: the transition proceeds after the grace.
    assert(transition_completes(continued) && atomic_load(&recorder_unavailable));
    dvr_cmd(DVR_START);
    assert(!control.desired);
    recorder = start_recorder();
    while (atomic_load(&recorder_unavailable)) usleep(10000);
    assert(recorder_pid == (unsigned)recorder);
    // A press deferred as the recorder dies must survive unavailable -> available
    // even when the UI is too busy to poll in between.
    unsigned before = continued;
    end_recorder(recorder);
    assert(dvr_defer_transition(continue_transition, 42));
    while (!atomic_load(&recorder_unavailable)) usleep(10000);
    recorder = start_recorder();
    while (atomic_load(&recorder_unavailable)) usleep(10000);
    assert(continues(before));
    end_recorder(recorder);
    msgctl(msgget(RECORD_CONTROL_KEY, 0), IPC_RMID, NULL);
    char cleanup[64];
    snprintf(cleanup, sizeof(cleanup), "rm -rf /tmp/hdz-dvr-control/%d", (int)getpid());
    assert(system(cleanup) == 0);
    g_app_state = saved_state;
}

static void check_recording(int expected) {
    setting_record_naming_t preference = g_setting.record.naming;
    recording_naming = -1;
    dvr_snapshot_t snapshot = dvr_snapshot();
    config_ok = true;
    assert(dvr_update_record_conf(&snapshot));
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

    test_camera = VR_1080P24;
    check_recording(SETTING_NAMING_ELRS);
    assert(encoder_height == 1080 && encoder_fps == 50 && encoder_h265 == 0);
    test_camera = VR_540P90;
    check_recording(SETTING_NAMING_ELRS);
    assert(encoder_height == 720 && encoder_fps == 90 && encoder_h265 == 0);
    g_source_info.source = SOURCE_HDMI_IN;
    g_hw_stat.hdmiin_vtmg = HDMIIN_VTMG_1080P60;
    check_recording(SETTING_NAMING_ELRS);
    assert(encoder_height == 1080 && encoder_fps == 60 && encoder_h265 == 0);
    config_ok = true;
    dvr_write_vi_conf(VR_1080P60);
    assert(config_ok && vi_fps == 59);
    fail_write = 1;
    config_ok = true;
    dvr_snapshot_t snapshot = dvr_snapshot();
    assert(!dvr_update_record_conf(&snapshot));
    fail_write = 0;
    check_deferred_stop();
    current_clock = synced;
    check_recording(SETTING_NAMING_ELRS);
    check_recorder_replaced();

    puts("DVR config/naming, encoder modes, failed writes, ACK-gated transitions and recorder replacement passed");
    return 0;
}
