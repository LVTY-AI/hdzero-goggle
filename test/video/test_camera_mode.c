#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "core/msp_displayport.h"
#include "driver/hardware.h"
#include "driver/screen.h"
#include "driver/rtc6715.h"
#include "core/app_state.h"
#include "core/settings.h"
#include "ui/ui_main_menu.h"

static int inject_at;
static int fps_calls;
static int recorded_format;
static bool late_retime;
setting_t g_setting;
source_info_t g_source_info;
progress_bar_t progress_bar;
int valid_channel_tb[10], user_select_index;

static void publish(uint8_t mode, uint8_t ratio) {
    uint8_t packet[13] = {0};
    packet[1] = mode;
    memcpy(packet + 2, "BTFL", 4);
    packet[12] = ratio ? 0xaa : 0x55;
    parser_config(packet);
    parser_config(packet); // the existing camera-type debounce needs two packets
}

static void inject(int point) {
    if (inject_at == point) {
        inject_at = 0;
        publish(0x77, 0); // another packet arrives while the switch is blocked
    }
}

void DM5680_SetFPS(uint8_t mode) { fps_calls++; inject(1); }
void DM6302_openM0(uint32_t on) {}
void dvr_update_vi_conf(video_resolution_t mode) { recorded_format = mode; inject(2); }
int system_exec(const char *command) { return 0; }
int system_script(const char *command) { inject(3); return 0; }
int i2c_write(int port, uint8_t dev, uint8_t reg, uint8_t value) {
    if (late_retime && dev == 0x64 && reg == 0x84) {
        late_retime = false;
        publish(0x77, 0);
        assert(HDZERO_detect() == 1);
    }
    return 1;
}
uint8_t i2c_read(int port, uint8_t dev, uint8_t reg) { return 0; }
void IT66021_Set_Pclk(int inv, int delay) {}
void IT66021_Mask_WR(uint8_t bank, uint8_t addr, uint8_t mask, uint8_t value) {}
void IT66121_set_phase(uint8_t phase, uint8_t invert) {}
void TP2825_Set_Pclk(uint8_t invert) {}
void load_fc_osd_font(uint8_t fhd) {}
void screen_vtmg_invalidate(void) {}
static void analog_init(bool power, bool audio) {}
rtc6715_t rtc6715 = {.init = analog_init};
uint8_t hdzero_effective_bw(void) { return 0; }
void scan_core_notify_analog_powered_off(void) {}
int DM6302_init(uint8_t freq, uint8_t bw) { return 0; }
void DM6302_SetChannel(uint8_t band, uint8_t channel) {}
void DM5680_SetBR(uint8_t bw) {}
void DM5680_SetBB(uint8_t on) {}
void DM5680_ResetRF(uint8_t on) {}
void DM5680_clear_vldflg(void) {}
void DM5680_req_vldflg(void) {}
void osd_detecting_show(bool on) {}
void osd_fhd(uint8_t fhd) {}
int osd_clear(void) { return 0; }
void osd_show(bool on) {}
uint8_t channel_osd_mode;
uint32_t lv_timer_handler(void) { return 0; }
int lvgl_switch_to_720p(void) { return 0; }
int lvgl_switch_to_1080p(void) { return 0; }
void image_settings_apply(setting_image_source_t source) {}
int ini_putl(const char *section, const char *key, long value, const char *path) { return 1; }

static void display(bool on) {}
static void vtmg(int mode) {}
static void ratio(bool is43) {}
static void mode720(uint8_t mode) {}
static void mode60(uint8_t mode, bool is43) {}
static void mode(void) {}
screen_t screen = {
    .display = display, .vtmg = vtmg,
    .mfpga = {.set_ratio = ratio, .set720p90 = mode720, .set720p60 = mode60,
              .set540p60 = mode, .set1080p30 = mode},
};

int main(void) {
    hw_stat_init();
    for (int point = 1; point <= 3; point++) {
        g_hw_stat.source_mode = SOURCE_MODE_HDZERO;
        g_hw_stat.hdz_mode = VR_720P60;
        publish(0x33, 1); // 540p60
        camera_video_t before = camera_video_snapshot();
        assert(before.mode == VR_540P60 && before.is_43 == 1);
        fps_calls = 0;
        inject_at = point;
        assert(HDZERO_detect() == 1);
        assert(g_hw_stat.hdz_mode == VR_540P60);
        assert(recorded_format == VR_540P60);
        assert(CAM_MODE == VR_1080P30);
        assert(HDZERO_detect() == 1);
        assert(g_hw_stat.hdz_mode == VR_1080P30);
        assert(recorded_format == VR_1080P30);
        assert(HDZERO_detect() == 0);
        assert(fps_calls == 2);
    }

    // Direct source entry must update the same applied-mode cache as polling.
    publish(0xaa, 0);
#if defined(HDZBOXPRO)
    Display_HDZ(VR_540P60, 0);
#else
    Display_720P60_50(VR_540P60, 0);
#endif
    assert(g_hw_stat.hdz_mode == VR_540P60);
    assert(HDZERO_detect() == 1);
    assert(g_hw_stat.hdz_mode == VR_720P60);
    assert(recorded_format == VR_720P60);
    // A background retime after source entry's display lock is released must
    // not be undone by the main thread's later DVR tail.
    publish(0x33, 0);
    g_setting.scan.channel = 1;
    g_hw_stat.hdzero_open = 1;
    late_retime = true;
    app_switch_to_hdzero(true);
    assert(!late_retime);
    assert(g_hw_stat.hdz_mode == VR_1080P30);
    assert(recorded_format == VR_1080P30);
    puts("camera mode: in-flight updates and direct-entry cache passed");
}
