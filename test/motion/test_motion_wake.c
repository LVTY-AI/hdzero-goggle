#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/core/ht.c"

setting_t g_setting;
hw_status_t g_hw_stat;

static int power_on_calls, power_off_calls, tune_calls;
static int hdz_open_calls, hdz_close_calls, hdz_tune_calls;
static bool idle_inhibited;
static int expected_rx;
static uint8_t brightness;

static void analog_init(bool power, bool audio) {
    assert(idle_inhibited);
    if (power) {
        power_on_calls++;
        assert(audio == (g_setting.record.audio_source == SETTING_RECORD_AUDIO_SOURCE_AV_IN));
    } else {
        power_off_calls++;
        assert(!audio);
    }
}

static void analog_tune(int channel) {
    assert(idle_inhibited && power_on_calls == 1);
    assert(channel == g_setting.source.analog_channel - 1);
    tune_calls++;
}

rtc6715_t rtc6715 = {.init = analog_init, .set_ch = analog_tune};

static void set_brightness(uint8_t level) { brightness = level; }
screen_t screen = {.brightness = set_brightness};

void screen_set_inhibited(uint32_t reasons, bool inhibited) {
    assert(reasons == SCREEN_INHIBIT_IDLE);
    if (!inhibited) {
        assert(power_on_calls == expected_rx);
        assert(tune_calls == expected_rx);
        assert(brightness == g_setting.image.oled);
    }
    idle_inhibited = inhibited;
}

void HDZero_Close(void) { hdz_close_calls++; }
uint8_t hdzero_effective_bw(void) { return 1; }
void HDZero_open(int bw) {
    assert(idle_inhibited && bw == 1);
    hdz_open_calls++;
}
void DM6302_SetChannel(uint8_t band, uint8_t channel) {
    assert(idle_inhibited && hdz_open_calls == 1);
    assert(band == g_setting.source.hdzero_band);
    assert(channel == g_setting.scan.channel - 1);
    hdz_tune_calls++;
}
void beep_dur(int ms) {}
int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) { return 0; }

int main(int argc, char **argv) {
    assert(argc == 2);
    g_hw_stat.source_mode = SOURCE_MODE_AV;
    g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_INTERNAL;
    g_setting.image.oled = 8;
    g_setting.scan.channel = 6;
    g_setting.source.hdzero_band = SETTING_SOURCES_HDZERO_BAND_RACEBAND;

    if (!strcmp(argv[1], "built-in")) {
        expected_rx = 1;
    } else if (!strcmp(argv[1], "expansion")) {
        g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_EXTERNAL;
#if defined(HDZBOXPRO)
        expected_rx = 1; // BoxPro has no expansion receiver selection.
#endif
    } else if (!strcmp(argv[1], "dual")) {
        g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_EXTERNAL;
        g_setting.source.auto_protocol_detect = true;
        expected_rx = 1;
    } else if (!strcmp(argv[1], "av-in") || !strcmp(argv[1], "av-in-dual")) {
        g_hw_stat.is_av_in = 1;
        g_setting.source.auto_protocol_detect = !strcmp(argv[1], "av-in-dual");
    } else if (!strcmp(argv[1], "hdzero")) {
        g_hw_stat.source_mode = SOURCE_MODE_HDZERO;
    } else if (!strcmp(argv[1], "menu")) {
        g_hw_stat.source_mode = SOURCE_MODE_UI;
    } else if (!strcmp(argv[1], "hdmi")) {
        g_hw_stat.source_mode = SOURCE_MODE_HDMIIN;
    } else {
        assert(!"unknown scenario");
    }
#if defined(HDZGOGGLE)
    expected_rx = 0; // G1 has no built-in RTC6715.
#endif

    // Exercise complete idle/wake cycles, including both channel-table ends
    // and audio settings. A second cycle must restore the new saved channel.
    for (int cycle = 0; cycle < 2; cycle++) {
        g_setting.source.analog_channel = cycle ? 48 : 1;
        g_setting.record.audio_source = cycle ? SETTING_RECORD_AUDIO_SOURCE_AV_IN : SETTING_RECORD_AUDIO_SOURCE_MIC;
        power_on_calls = power_off_calls = tune_calls = 0;
        hdz_open_calls = hdz_close_calls = hdz_tune_calls = 0;
        for (int tick = 0; tick < 2 * MOTION_DUR_1MINUTE + 1; tick++)
            detect_motion(false);
        assert(idle_inhibited);
        assert(power_off_calls == (g_hw_stat.source_mode == SOURCE_MODE_AV));
        assert(hdz_close_calls == (g_hw_stat.source_mode == SOURCE_MODE_HDZERO));
        detect_motion(false);
        detect_motion(true);
        assert(idle_inhibited && !power_on_calls && !hdz_open_calls);
        detect_motion(true);
        assert(!idle_inhibited);
        assert(hdz_open_calls == (g_hw_stat.source_mode == SOURCE_MODE_HDZERO));
        assert(hdz_tune_calls == hdz_open_calls);
        detect_motion(true);
        assert(power_on_calls == expected_rx && tune_calls == expected_rx);
    }
    printf("motion wake: %s passed\n", argv[1]);
    return 0;
}
