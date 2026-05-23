#include "page_scannow.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "../conf/ui.h"

#include "core/app_state.h"
#include "core/scan_core.h"
#include "core/common.hh"
#include "core/defines.h"
#include "core/msp_displayport.h"
#include "core/osd.h"
#include "core/settings.h"
#include "driver/dm6302.h"
#include "driver/fbtools.h"
#include "driver/hardware.h"
#include "driver/i2c.h"
#include "driver/rtc6715.h"
#include "driver/screen.h"
#include "driver/uart.h"
#include "lang/language.h"
#include "ui/page_common.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_style.h"

LV_IMG_DECLARE(img_signal_status);
LV_IMG_DECLARE(img_signal_status2);
LV_IMG_DECLARE(img_signal_status3);
LV_IMG_DECLARE(img_signal_level);
LV_IMG_DECLARE(img_ant1);
LV_IMG_DECLARE(img_ant2);
LV_IMG_DECLARE(img_ant3);
LV_IMG_DECLARE(img_ant4);
LV_IMG_DECLARE(img_ant5);
LV_IMG_DECLARE(img_ant6);
LV_IMG_DECLARE(img_ant7);
LV_IMG_DECLARE(img_ant8);
LV_IMG_DECLARE(img_ant9);
LV_IMG_DECLARE(img_ant10);
LV_IMG_DECLARE(img_ant11);
LV_IMG_DECLARE(img_ant12);
LV_IMG_DECLARE(img_ant13);
LV_IMG_DECLARE(img_ant14);

typedef struct {
    bool is_valid;
    int gain;
    int bw;
} channel_status_t;

typedef struct {
    lv_obj_t *img0;
    lv_obj_t *label;
    lv_obj_t *img1;
} channel_t;

channel_t channel_tb[BASE_CH_NUM];
channel_status_t channel_status_tb[BASE_CH_NUM];

////////////////////////////////////////////////////////////////////////////////////////////////////
int valid_channel_tb[BASE_CH_NUM];
int user_select_index = 0;

// local
static int auto_scaned_cnt = 0;
static lv_obj_t *progressbar;
static lv_obj_t *label;
static lv_coord_t col_dsc1[] = {UI_SCANNOW_SCANNER_COLS};
static lv_coord_t row_dsc1[] = {UI_SCANNOW_SCANNER_ROWS};
static lv_coord_t col_dsc2[] = {UI_SCANNOW_SIGNAL_COLS};
static lv_coord_t row_dsc2[] = {UI_SCANNOW_SIGNAL_ROWS};
#if defined(HDZBOXPRO)
typedef enum {
    SCAN_MODE_HDZERO = 0,
    SCAN_MODE_ANALOG = 1,
    SCAN_MODE_AUTO   = 2,
} scan_mode_t;

static scan_mode_t scan_mode = SCAN_MODE_HDZERO;
static lv_obj_t *mode_dropdown = NULL;
static lv_obj_t *band_dropdown = NULL;
#endif

static void select_signal(channel_t *channel) {
    for (int i = 0; i < BASE_CH_NUM; i++) {
        if (channel_status_tb[i].is_valid) {
            lv_img_set_src(channel_tb[i].img0, &img_signal_status2);
        } else {
            lv_img_set_src(channel_tb[i].img0, &img_signal_status);
        }
    }
    lv_img_set_src(channel->img0, &img_signal_status3);
}

static void set_signal_bar(channel_t *channel, bool is_valid, int gain) {
    if (!is_valid) {
        lv_img_set_src(channel->img0, &img_signal_status);

        if (gain < 5) {
            lv_img_set_src(channel->img1, &img_ant1);
        } else if (gain < 10) {
            lv_img_set_src(channel->img1, &img_ant3);
        } else if (gain < 15) {
            lv_img_set_src(channel->img1, &img_ant4);
        } else if (gain < 16) {
            lv_img_set_src(channel->img1, &img_ant5);
        } else if (gain < 20) {
            lv_img_set_src(channel->img1, &img_ant6);
        } else if (gain < 30) {
            lv_img_set_src(channel->img1, &img_ant7);
        } else if (gain <= 77) {
            lv_img_set_src(channel->img1, &img_ant7);
        } else {
            lv_img_set_src(channel->img1, &img_ant1);
        }

    } else {
        lv_img_set_src(channel->img0, &img_signal_status2);
        if (gain < 5) {
            lv_img_set_src(channel->img1, &img_ant8);
        } else if (gain < 10) {
            lv_img_set_src(channel->img1, &img_ant10);
        } else if (gain < 15) {
            lv_img_set_src(channel->img1, &img_ant11);
        } else if (gain < 16) {
            lv_img_set_src(channel->img1, &img_ant12);
        } else if (gain < 20) {
            lv_img_set_src(channel->img1, &img_ant13);
        } else if (gain < 30) {
            lv_img_set_src(channel->img1, &img_ant14);
        } else if (gain <= 77) {
            lv_img_set_src(channel->img1, &img_ant14);
        } else {
            lv_img_set_src(channel->img1, &img_ant8);
        }
    }
}
// gain, 0-60
static void create_channel_switch(lv_obj_t *parent, int col, int row, channel_t *channel) {

    channel->img0 = lv_img_create(parent);
    lv_img_set_src(channel->img0, &img_signal_status);
    lv_obj_set_size(channel->img0, img_signal_status.header.w, img_signal_status.header.h);
    lv_obj_set_grid_cell(channel->img0, LV_GRID_ALIGN_START, col, 1,
                         LV_GRID_ALIGN_CENTER, row, 1);

    channel->label = lv_label_create(parent);
    lv_obj_set_style_text_font(channel->label, UI_SCANNOW_CHAN_FONT, 0);
    lv_obj_set_style_text_align(channel->label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(channel->label, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(channel->label, UI_SCANNOW_CHAN_PAD, 0);
    lv_label_set_long_mode(channel->label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_grid_cell(channel->label, LV_GRID_ALIGN_START, col + 1, 1,
                         LV_GRID_ALIGN_CENTER, row, 1);

    channel->img1 = lv_img_create(parent);
    lv_img_set_src(channel->img1, &img_ant1);
    lv_obj_set_size(channel->img1, img_ant1.header.w, img_ant1.header.h);
    lv_obj_set_grid_cell(channel->img1, LV_GRID_ALIGN_START, col + 2, 1,
                         LV_GRID_ALIGN_CENTER, row, 1);
}

void page_scannow_set_channel_label(void) {
    static const char *race_band_channel_str[] = {"R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "E1", "F1", "F2", "F4"};
    static const char *low_band_channel_str[]  = {"L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8"};
    uint8_t i;

#if defined(HDZBOXPRO)
    if (scan_mode == SCAN_MODE_ANALOG) {
        static const char *analog_band_letters[6] = {"A", "B", "E", "F", "R", "L"};
        static char buf[8][4];
        const char *letter = analog_band_letters[g_setting.source.analog_scan_band & 0x07];
        for (i = 0; i < 8; i++) {
            snprintf(buf[i], sizeof(buf[i]), "%s%d", letter, i + 1);
            lv_label_set_text(channel_tb[i].label, buf[i]);
        }
        // Analog always uses exactly 8 cells; hide the extra 4 Race-band cells.
        for (i = 8; i < BASE_CH_NUM; i++) {
            lv_obj_add_flag(channel_tb[i].img0, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(channel_tb[i].label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(channel_tb[i].img1, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
#endif

    if (g_setting.source.hdzero_band == RACE_BAND) {
        for (i = 0; i < BASE_CH_NUM; i++) {
            lv_label_set_text(channel_tb[i].label, race_band_channel_str[i]);
        }
        for (i = 8; i < BASE_CH_NUM; i++) {
            lv_obj_clear_flag(channel_tb[i].img0, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(channel_tb[i].label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(channel_tb[i].img1, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        for (i = 0; i < 8; i++) {
            lv_label_set_text(channel_tb[i].label, low_band_channel_str[i]);
        }
        for (i = 8; i < BASE_CH_NUM; i++) {
            lv_obj_add_flag(channel_tb[i].img0, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(channel_tb[i].label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(channel_tb[i].img1, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// 1920-500
// 1420
// 1420*0.18
// 255.6
// 1420-256
// 1164
#if defined(HDZBOXPRO)
static void on_band_dropdown_change(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_target(e);
    uint8_t band = (uint8_t)lv_dropdown_get_selected(dd);
    g_setting.source.analog_scan_band = band;
    ini_putl("source", "analog_scan_band", band, SETTING_INI);
    page_scannow_set_channel_label();
}

static void on_mode_dropdown_change(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_target(e);
    scan_mode = (scan_mode_t)lv_dropdown_get_selected(dd);
    LOGI("scan_mode -> %d", scan_mode);

    if (band_dropdown) {
        if (scan_mode == SCAN_MODE_ANALOG) {
            lv_obj_clear_flag(band_dropdown, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(band_dropdown, LV_OBJ_FLAG_HIDDEN);
        }
    }
    page_scannow_set_channel_label();
}
#endif

static lv_obj_t *page_scannow_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[256];

    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_scan, LV_PART_MAIN);
    lv_obj_set_style_pad_top(page, UI_SCANNOW_PAGE_PAD, 0);

    lv_obj_t *cont1 = lv_obj_create(page);
    lv_obj_set_size(cont1, UI_SCANNOW_SCANNER_SIZE);
    lv_obj_set_layout(cont1, LV_LAYOUT_GRID);
    lv_obj_clear_flag(cont1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont1, &style_scan, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cont1, 0, 0);
    lv_obj_set_style_grid_column_dsc_array(cont1, col_dsc1, 0);
    lv_obj_set_style_grid_row_dsc_array(cont1, row_dsc1, 0);

    progressbar = lv_bar_create(cont1);
    lv_obj_set_size(progressbar, UI_SCANNOW_PROG_BAR_SIZE);
    lv_obj_center(progressbar);
    lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progressbar, lv_color_make(0xff, 0xff, 0xff), LV_PART_MAIN);
    lv_obj_set_style_radius(progressbar, 0, LV_PART_MAIN);
#if defined(HDZBOXPRO)
    lv_obj_set_style_bg_color(progressbar, lv_color_make(0, 0x80, 0), LV_PART_INDICATOR);
#else
    lv_obj_set_style_bg_color(progressbar, lv_color_make(0, 0xff, 0), LV_PART_INDICATOR);
#endif
    lv_obj_set_style_radius(progressbar, 0, LV_PART_INDICATOR);

    lv_obj_set_grid_cell(progressbar, LV_GRID_ALIGN_START, 0, 1,
                         LV_GRID_ALIGN_CENTER, 1, 1);

    lv_bar_set_range(progressbar, 0, 14);

    label = lv_label_create(cont1);
    lv_label_set_text(label, _lang("Scan Ready"));
    lv_obj_set_style_text_font(label, UI_SCANNOW_READY_FONT, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(label, UI_SCANNOW_READY_PAD, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_grid_cell(label, LV_GRID_ALIGN_START, 0, 1,
                         LV_GRID_ALIGN_CENTER, 0, 1);

    lv_obj_t *label2 = lv_label_create(cont1);
    snprintf(buf, sizeof(buf), "%s\n %s\n %s",
             _lang("When scanning is complete, use the"),
             _lang("dial to select a channel and press"),
             _lang("the Enter button to choose"));
    lv_label_set_text(label2, buf);
    lv_obj_set_style_text_font(label2, UI_SCANNOW_NOTE_FONT, 0);
    lv_obj_set_style_text_align(label2, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label2, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(label2, UI_SCANNOW_NOTE_PAD, 0);
    lv_label_set_long_mode(label2, LV_LABEL_LONG_WRAP);
#if defined(HDZBOXPRO)
    // On BoxPro: row 0 = Mode dropdown, row 1 = Band dropdown, row 2 = notes.
    // label2 is shifted to row 2 (rowspan=1) to make room for the band dropdown at row 1.
    lv_obj_set_grid_cell(label2, LV_GRID_ALIGN_START, 2, 1,
                         LV_GRID_ALIGN_START, 2, 1);

    mode_dropdown = lv_dropdown_create(cont1);
    lv_dropdown_set_options(mode_dropdown, "HDZero\nAnalog\nAuto");
    lv_obj_set_grid_cell(mode_dropdown, LV_GRID_ALIGN_END, 2, 1,
                         LV_GRID_ALIGN_START, 0, 1);
    {
        uint16_t default_mode;
        if (g_setting.source.auto_protocol_detect) {
            default_mode = SCAN_MODE_AUTO;
        } else if (g_source_info.source == SOURCE_AV_MODULE) {
            default_mode = SCAN_MODE_ANALOG;
        } else {
            default_mode = SCAN_MODE_HDZERO;
        }
        lv_dropdown_set_selected(mode_dropdown, default_mode);
        scan_mode = (scan_mode_t)default_mode;
    }
    lv_obj_add_event_cb(mode_dropdown, on_mode_dropdown_change,
                        LV_EVENT_VALUE_CHANGED, NULL);

    band_dropdown = lv_dropdown_create(cont1);
    lv_dropdown_set_options(band_dropdown, "A\nB\nE\nF\nR\nL");
    lv_obj_set_grid_cell(band_dropdown, LV_GRID_ALIGN_END, 2, 1,
                         LV_GRID_ALIGN_START, 1, 1);
    lv_dropdown_set_selected(band_dropdown, g_setting.source.analog_scan_band);
    lv_obj_add_event_cb(band_dropdown, on_band_dropdown_change,
                        LV_EVENT_VALUE_CHANGED, NULL);
    // Hidden unless Mode == Analog. Sync initial visibility with scan_mode set above.
    if (scan_mode != SCAN_MODE_ANALOG) {
        lv_obj_add_flag(band_dropdown, LV_OBJ_FLAG_HIDDEN);
    }
#else
    lv_obj_set_grid_cell(label2, LV_GRID_ALIGN_START, 2, 1,
                         LV_GRID_ALIGN_START, 0, 3);
#endif

    lv_obj_t *cont2 = lv_obj_create(page);
    lv_obj_set_size(cont2, UI_SCANNOW_FREQ_SIZE);
    lv_obj_set_layout(cont2, LV_LAYOUT_GRID);
    lv_obj_clear_flag(cont2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont2, &style_scan, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cont2, 0, 0);
    lv_obj_set_style_grid_column_dsc_array(cont2, col_dsc2, 0);
    lv_obj_set_style_grid_row_dsc_array(cont2, row_dsc2, 0);

    // create channel
    uint8_t col_offset = 1;
    uint8_t row_offset = 0;

    for (int i = 0; i < 8; i++) {
        create_channel_switch(cont2, ((i >> 2) << 2) + col_offset, i & 0x03, &channel_tb[i]);
    }

    row_offset = 4;
    for (int i = 0; i < 4; i++) {
        create_channel_switch(cont2, ((i >> 1) << 2) + col_offset, row_offset + (i & 0x01), &channel_tb[8 + i]);
    }
    page_scannow_set_channel_label();

    return page;
}

static void user_select_signal(void) {
    if (valid_channel_tb[0] == -1)
        return;

    user_select_index = 0;
    select_signal(&channel_tb[valid_channel_tb[0] & 0x7F]);
}

static void user_clear_signal(void) {
    user_select_index = 0;
    for (int i = 0; i < BASE_CH_NUM; i++) {
        lv_img_set_src(channel_tb[i].img0, &img_signal_status);
        lv_img_set_src(channel_tb[i].img1, &img_ant1);
    }
}


static int8_t scan_now_hdzero(void) {
    uint8_t ch, gain;
    bool valid;
    uint8_t valid_index;
    char buf[128];

    snprintf(buf, sizeof(buf), "%s...", _lang("Scanning"));
    lv_label_set_text(label, buf);
    lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
    lv_timer_handler();
    lv_bar_set_value(progressbar, 2, LV_ANIM_OFF);
    lv_timer_handler();

    // clear
    for (ch = 0; ch < BASE_CH_NUM; ch++) {
        valid_channel_tb[ch] = -1;
        channel_status_tb[ch].is_valid = 0;
    }

    HDZero_open(g_setting.source.hdzero_bw);
    lv_bar_set_value(progressbar, 4, LV_ANIM_OFF);
    lv_timer_handler();

    for (ch = 0; ch < HDZERO_CHANNEL_NUM; ch++) {
        scan_probe_hdzero(g_setting.source.hdzero_band, ch, &gain, &valid);
        if (valid) {
            channel_status_tb[ch].is_valid = 1;
            channel_status_tb[ch].gain = gain;
            set_signal_bar(&channel_tb[ch], channel_status_tb[ch].is_valid, channel_status_tb[ch].gain);
        }
        lv_bar_set_value(progressbar, ch + 5, LV_ANIM_OFF);
        lv_timer_handler();
    }
    lv_bar_set_value(progressbar, 14, LV_ANIM_OFF);

    valid_index = 0;
    for (ch = 0; ch < HDZERO_CHANNEL_NUM; ch++) {
        if (channel_status_tb[ch].is_valid) {
            valid_channel_tb[valid_index++] = ch;
        }

        lv_timer_handler();
    }

    user_select_signal();
    lv_label_set_text(label, _lang("Scanning done"));
    if (!valid_index)
        return -1;
    else
        return valid_index;
}

#if defined(HDZBOXPRO)
static int8_t scan_now_analog(uint8_t band_idx) {
    char buf[128];
    uint16_t rssi_mv;
    bool valid;
    uint8_t valid_index = 0;

    snprintf(buf, sizeof(buf), "%s...", _lang("Scanning"));
    lv_label_set_text(label, buf);
    lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
    lv_timer_handler();

    // Reset state for the 8 visible cells.
    for (uint8_t ch = 0; ch < BASE_CH_NUM; ch++) {
        valid_channel_tb[ch] = -1;
        channel_status_tb[ch].is_valid = 0;
    }

    // Power on RTC6715; HDZ stays closed while we probe analog channels.
    rtc6715.init(1, 0);

    for (uint8_t i = 0; i < 8; i++) {
        uint8_t global_idx = band_idx * 8 + i; // 0..47
        scan_probe_analog(global_idx, &rssi_mv, &valid);
        if (valid) {
            channel_status_tb[i].is_valid = 1;
            // set_signal_bar buckets were tuned for HDZ gain (0..60). Map RSSI mV
            // to that range with a coarse /32 divisor and clamp.
            {
                uint16_t scaled = rssi_mv / 32;
                if (scaled > 60) scaled = 60;
                channel_status_tb[i].gain = (uint8_t)scaled;
            }
            set_signal_bar(&channel_tb[i],
                           channel_status_tb[i].is_valid,
                           channel_status_tb[i].gain);
        }
        lv_bar_set_value(progressbar, (int)((i + 1) * 14 / 8), LV_ANIM_OFF);
        lv_timer_handler();
    }
    lv_bar_set_value(progressbar, 14, LV_ANIM_OFF);

    for (uint8_t ch = 0; ch < 8; ch++) {
        if (channel_status_tb[ch].is_valid) {
            valid_channel_tb[valid_index++] = ch;
        }
    }

    user_select_signal();
    lv_label_set_text(label, _lang("Scanning done"));
    return valid_index ? (int8_t)valid_index : -1;
}

static int8_t scan_now_auto(void) {
    return -1; // implemented in Task 9
}

static int8_t scan_now_dispatch(void) {
    switch (scan_mode) {
    case SCAN_MODE_HDZERO: return scan_now_hdzero();
    case SCAN_MODE_ANALOG: return scan_now_analog(g_setting.source.analog_scan_band);
    case SCAN_MODE_AUTO:   return scan_now_auto();
    }
    return -1;
}
#endif

int scan_reinit(void) {
    lv_label_set_text(label, _lang("Scanning ready"));
    lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
    user_clear_signal();
    lv_timer_handler();
    return 0;
}

int scan(void) {
    g_scanning = true;
#if defined(HDZBOXPRO)
    // SOURCE_HDZERO is set only for the HDZ mode path; Analog/Auto manage source on click.
    if (scan_mode == SCAN_MODE_HDZERO) {
        g_source_info.source = SOURCE_HDZERO;
    }
    int8_t ret = scan_now_dispatch();
#else
    g_source_info.source = SOURCE_HDZERO;
    int8_t ret = scan_now_hdzero();
#endif
    g_scanning = false;
    return ret;
}

void autoscan_exit(void) {
    if (!g_autoscan_exit) {
        LOGI("autoscan_exit, lelve=1");
        g_autoscan_exit = true;
        if (auto_scaned_cnt > 1)
            app_state_push(APP_STATE_SUBMENU);
        else
            app_state_push(APP_STATE_MAINMENU);
    }
}

static void page_scannow_enter() {
    auto_scaned_cnt = scan();
    LOGI("scan return :%d", auto_scaned_cnt);

    if (auto_scaned_cnt == 1) {
        if (!g_autoscan_exit)
            g_autoscan_exit = true;

        app_state_push(APP_STATE_VIDEO);
#if defined(HDZBOXPRO)
        if (scan_mode == SCAN_MODE_ANALOG) {
            // valid_channel_tb[0] is the cell index within the current band.
            uint8_t band = g_setting.source.analog_scan_band;
            uint8_t ch_in_band = valid_channel_tb[0] & 0x7F;
            uint8_t global_idx = band * 8 + ch_in_band;
            g_setting.source.analog_channel = global_idx + 1;
            ini_putl("source", "analog_channel",
                     g_setting.source.analog_channel, SETTING_INI);
            app_switch_to_analog(0);
        } else
#endif
        {
            app_switch_to_hdzero(false);
        }
    }

    if (auto_scaned_cnt == -1)
        submenu_exit();
}

static void page_scannow_exit() {
#if defined(HDZBOXPRO)
    if (scan_mode == SCAN_MODE_ANALOG) {
        rtc6715.init(0, 0); // power down analog RX on exit
        return;             // HDZ was never opened in this mode
    }
#endif
    HDZero_Close();
}

static void page_scannow_on_roller(uint8_t key) {
    if (valid_channel_tb[0] == -1)
        return;

    if (key == DIAL_KEY_UP) {
        if (valid_channel_tb[user_select_index + 1] != -1)
            user_select_index++;
    } else if (key == DIAL_KEY_DOWN) {
        if (user_select_index > 0)
            user_select_index--;
    }
    select_signal(&channel_tb[valid_channel_tb[user_select_index] & 0x07F]);
}

static void page_scannow_on_click(uint8_t key, int sel) {
#if defined(HDZBOXPRO)
    if (scan_mode == SCAN_MODE_ANALOG) {
        if (valid_channel_tb[0] == -1) return;
        uint8_t band = g_setting.source.analog_scan_band;
        uint8_t ch_in_band = (uint8_t)(valid_channel_tb[user_select_index] & 0x7F);
        uint8_t global_idx = band * 8 + ch_in_band;
        g_setting.source.analog_channel = global_idx + 1; // 1-indexed
        ini_putl("source", "analog_channel",
                 g_setting.source.analog_channel, SETTING_INI);
        app_state_push(APP_STATE_VIDEO);
        app_switch_to_analog(0);
        return;
    }
    // Task 9 will add Auto mode here.
#endif
    app_state_push(APP_STATE_VIDEO);
    app_switch_to_hdzero(false);
}

page_pack_t pp_scannow = {
    .name = "Scan Now",
    .create = page_scannow_create,
    .enter = page_scannow_enter,
    .exit = page_scannow_exit,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = page_scannow_on_roller,
    .on_click = page_scannow_on_click,
    .on_right_button = NULL,
};