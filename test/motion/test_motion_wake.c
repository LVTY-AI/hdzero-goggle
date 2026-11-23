#include <assert.h>
#include <stdio.h>

#include "../../src/core/ht.c"

setting_t g_setting;
hw_status_t g_hw_stat;
rx_status_t rx_status[2];

static bool idle_inhibited;
static uint8_t brightness;
static void set_brightness(uint8_t level) { brightness = level; }
screen_t screen = {.brightness = set_brightness};

void screen_set_inhibited(uint32_t reasons, bool inhibited) {
    assert(reasons == SCREEN_INHIBIT_IDLE);
    if (!inhibited)
        assert(brightness == g_setting.image.oled);
    idle_inhibited = inhibited;
}
void beep_dur(int ms) {}
int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) { return 0; }

static void still(int ticks) {
    while (ticks-- > 0) detect_motion(false);
}

static void expect_live(source_mode_t mode) {
    g_hw_stat.source_mode = mode;
    for (int i = 0; i < 3 * MOTION_DUR_1MINUTE; i++) {
        detect_motion(false);
        assert(!idle_inhibited && brightness == g_setting.image.oled);
    }
}

int main(void) {
    brightness = g_setting.image.oled = 8;
    rx_status[1].rx_valid = 1;
    expect_live(SOURCE_MODE_HDZERO);
    rx_status[0].rx_valid = 1;
    rx_status[1].rx_valid = 0;
    expect_live(SOURCE_MODE_HDZERO);

    // Only the routed analog input counts. A short loss must not leave any
    // time banked against the next dropout.
    rx_status[0].rx_valid = 0;
    g_hw_stat.av_valid[0] = 2;
    expect_live(SOURCE_MODE_AV);
    g_hw_stat.is_av_in = 1;
    still(MOTION_DUR_1MINUTE);
    g_hw_stat.av_valid[1] = 1;
    expect_live(SOURCE_MODE_AV);
    g_hw_stat.av_valid[1] = 0;
    still(MOTION_DUR_1MINUTE);
    assert(brightness == 8 && !idle_inhibited);
    still(1);
    assert(brightness == 0 && !idle_inhibited);
    g_hw_stat.av_valid[1] = 2;
    expect_live(SOURCE_MODE_AV); // wake from dim with no motion

    g_hw_stat.source_mode = SOURCE_MODE_HDMIIN;
    still(2 * MOTION_DUR_1MINUTE + 1);
    assert(idle_inhibited);
    g_hw_stat.hdmiin_valid = 1;
    expect_live(SOURCE_MODE_HDMIIN); // wake from off with no motion

    g_hw_stat.source_mode = SOURCE_MODE_UI;
    still(2 * MOTION_DUR_1MINUTE + 1);
    assert(idle_inhibited);
    detect_motion(true);
    assert(idle_inhibited);
    detect_motion(true);
    assert(!idle_inhibited && brightness == 8);
    puts("panel idle and live video: passed");
    return 0;
}
