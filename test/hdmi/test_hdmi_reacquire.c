#include <assert.h>
#include <stdio.h>
#include <string.h>

#if defined(HDZGOGGLE)
#include "../../src/driver/hardware-goggle.c"
#elif defined(HDZGOGGLE2)
#include "../../src/driver/hardware-goggle2.c"
#elif defined(HDZBOXPRO)
#include "../../src/driver/hardware-boxpro.c"
#else
#error "Select a firmware target"
#endif

static int signal_present;
static int detected_timing = HDMIIN_VTMG_720P60;
static int receiver_resets;
static int signal_reads;
static int timing_reads;
static int display_enabled;
static int configured_vi = -1;
static int receiver_phase = -1;

int IT66021_Sig_det(void) {
    signal_reads++;
    return signal_present;
}

void IT66021_init(void) {
    receiver_resets++;
}

int IT66021_Get_VTMG(int *freq_ref) {
    timing_reads++;
    *freq_ref = 92;
    return detected_timing;
}

int IT66021_Get_CS(void) {
    return 0;
}

void IT66021_Set_CSMatrix(int cs) {}

void IT66021_Set_Pclk(int inv, int dly) {
    receiver_phase = inv;
}

void IT66021_Mask_WR(uint8_t is_ring, uint8_t addr, uint8_t mask, uint8_t wdat) {}

int i2c_write(int port, uint8_t slave_address, uint8_t addr, uint8_t val) {
    if (port == 3 && slave_address == ADDR_IT66021 && addr == 0x50)
        receiver_phase = val;
    return 0;
}

void IT66121_set_phase(uint8_t phase, uint8_t inv) {}
void TP2825_Set_Pclk(uint8_t inv) {}

int system_exec(const char *command) {
    return 0;
}

int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) {
    return 0;
}

void dvr_update_vi_conf(video_resolution_t fmt) {
    configured_vi = fmt;
}

static void test_display(bool on) {
    display_enabled = on;
}

static void test_vtmg(int mode) {}

screen_t screen = {
    .display = test_display,
    .vtmg = test_vtmg,
};

static void ticks(int count) {
    for (int i = 0; i < count; i++)
        HDMI_in_detect();
}

static void test_retry(source_mode_t source) {
    g_hw_stat.source_mode = source;
    ticks(19);
    assert(receiver_resets == 0);
    ticks(1);
    assert(receiver_resets == 1);
    ticks(39);
    assert(receiver_resets == 1);
    ticks(1);
    assert(receiver_resets == 2);
    ticks(40);
    assert(receiver_resets == 3);
    assert(signal_reads == 100);
    assert(timing_reads == 0);
}

static void test_other_sources(void) {
    g_hw_stat.source_mode = SOURCE_MODE_HDZERO;
    ticks(100);
    g_hw_stat.source_mode = SOURCE_MODE_AV;
    ticks(100);
    assert(receiver_resets == 0);
    assert(signal_reads == 0);
}

static void test_transient_loss(void) {
    g_hw_stat.source_mode = SOURCE_MODE_UI;
    ticks(19);
    signal_present = 1;
    ticks(1);
    signal_present = 0;
    ticks(19);
    assert(receiver_resets == 0);
    ticks(1);
    assert(receiver_resets == 1);
}

static void test_recovery(void) {
    g_hw_stat.source_mode = SOURCE_MODE_HDMIIN;
    memcpy(pclk_phase, pclk_phase_default, sizeof(pclk_phase));
#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    memcpy(vclk_phase, vclk_phase_default, sizeof(vclk_phase));
#endif
    ticks(20);
    assert(receiver_resets == 1);
    signal_present = 1;
    ticks(1);
    assert(g_hw_stat.hdmiin_valid == 1);
    assert(g_hw_stat.hdmiin_vtmg == HDMIIN_VTMG_720P60);
    assert(configured_vi == VR_720P60);
    assert(display_enabled == 1);
#if defined(HDZGOGGLE2)
    assert(receiver_phase == 0xa1);
#else
    assert(receiver_phase == 0);
#endif
    ticks(100);
    assert(receiver_resets == 1);
    signal_present = 0;
    ticks(19);
    assert(receiver_resets == 1);
    ticks(1);
    assert(receiver_resets == 2);
    signal_present = 1;
    configured_vi = -1;
    receiver_phase = -1;
    display_enabled = 0;
    ticks(1);
    assert(configured_vi == VR_720P60);
    assert(display_enabled == 1);
    assert(receiver_phase != -1);
}

static void test_present_unknown(void) {
    g_hw_stat.source_mode = SOURCE_MODE_HDMIIN;
    signal_present = 1;
    detected_timing = HDMIIN_VTMG_UNKNOW;
    ticks(100);
    assert(receiver_resets == 0);
    assert(g_hw_stat.hdmiin_valid == 1);
    assert(timing_reads == 100);
    assert(display_enabled == 0);
    assert(configured_vi == -1);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(pthread_mutex_init(&hardware_mutex, NULL) == 0);
    if (strcmp(argv[1], "retry-ui") == 0)
        test_retry(SOURCE_MODE_UI);
    else if (strcmp(argv[1], "retry-hdmi") == 0)
        test_retry(SOURCE_MODE_HDMIIN);
    else if (strcmp(argv[1], "other-sources") == 0)
        test_other_sources();
    else if (strcmp(argv[1], "transient-loss") == 0)
        test_transient_loss();
    else if (strcmp(argv[1], "recovery") == 0)
        test_recovery();
    else if (strcmp(argv[1], "present-unknown") == 0)
        test_present_unknown();
    else
        assert(!"Unknown test scenario");
    assert(pthread_mutex_destroy(&hardware_mutex) == 0);
    printf("HDMI reacquire: %s passed\n", argv[1]);
    return 0;
}
