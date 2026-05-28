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
#include "core/dvr.h"
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

// Two-state UI: page lands in IDLE (user picks Mode), click runs a scan and
// transitions to RESULTS. Right button returns to the menu (existing exit).
typedef enum {
    SCAN_PAGE_IDLE    = 0,
    SCAN_PAGE_RESULTS = 1,
} scan_page_state_t;

static scan_mode_t scan_mode = SCAN_MODE_HDZERO;
static scan_page_state_t page_state = SCAN_PAGE_IDLE;
static lv_obj_t *mode_btns[3];     // 0=HDZero, 1=Analog, 2=Auto/Both

static void update_mode_btn_focus(void) {
    // Theme button background defaults are very light on this skin and the
    // label text ends up invisible. Style each button explicitly: dark grey
    // background for unfocused, green for focused, white text always.
    for (int i = 0; i < 3; i++) {
        if (!mode_btns[i]) continue;
        bool is_focused = (i == (int)scan_mode);
        lv_obj_set_style_bg_color(mode_btns[i],
                                  is_focused ? lv_color_make(0, 0xA0, 0)
                                             : lv_color_make(0x40, 0x40, 0x40),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(mode_btns[i], LV_OPA_100, LV_PART_MAIN);
        lv_obj_set_style_border_width(mode_btns[i],
                                      is_focused ? 2 : 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(mode_btns[i],
                                      lv_color_make(0xFF, 0xFF, 0xFF),
                                      LV_PART_MAIN);
        if (is_focused) {
            lv_obj_add_state(mode_btns[i], LV_STATE_FOCUSED);
        } else {
            lv_obj_clear_state(mode_btns[i], LV_STATE_FOCUSED);
        }
    }
}

#define AUTO_RESULT_MAX 64 // up to 48 analog channels or ~55 freq-table rows

typedef struct {
    uint16_t freq_mhz;
    scan_protocol_t protocol;
    int8_t   hdz_channel;
    int8_t   analog_channel;
    uint8_t  strength;
} auto_result_t;

static auto_result_t auto_results[AUTO_RESULT_MAX];
static size_t auto_result_count = 0;
static int auto_select_index = 0;
static lv_obj_t *auto_list = NULL;
static lv_obj_t *auto_focused_btn = NULL;
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

#if defined(HDZBOXPRO)
// Set auto_protocol_detect based on the user's mode pick: ON only when they
// chose Auto/Both, OFF when they picked a single-protocol mode (HDZero or
// Analog). Called when the user clicks a scan result.
static void apply_auto_detect_for_mode(scan_mode_t mode) {
    bool want = (mode == SCAN_MODE_AUTO);
    if (g_setting.source.auto_protocol_detect != want) {
        g_setting.source.auto_protocol_detect = want;
        settings_put_bool("source", "auto_protocol_detect", want);
    }
}

static void set_results_widget_visibility(void) {
    // While the page is IDLE (user picking a mode), hide both the HDZ grid
    // and the auto_list. Otherwise (RESULTS), show whichever matches the
    // current mode: grid for HDZero, list for Analog/Auto.
    bool show_grid = (page_state == SCAN_PAGE_RESULTS) && (scan_mode == SCAN_MODE_HDZERO);
    bool show_list = (page_state == SCAN_PAGE_RESULTS) && !show_grid;

    if (auto_list) {
        if (show_list) {
            lv_obj_clear_flag(auto_list, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(auto_list, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (show_grid) {
        page_scannow_set_channel_label(); // restores grid cells
    } else {
        for (int i = 0; i < BASE_CH_NUM; i++) {
            if (channel_tb[i].img0)  lv_obj_add_flag(channel_tb[i].img0,  LV_OBJ_FLAG_HIDDEN);
            if (channel_tb[i].label) lv_obj_add_flag(channel_tb[i].label, LV_OBJ_FLAG_HIDDEN);
            if (channel_tb[i].img1)  lv_obj_add_flag(channel_tb[i].img1,  LV_OBJ_FLAG_HIDDEN);
        }
    }
}
#endif

static lv_obj_t *page_scannow_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[256];

    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_scan, LV_PART_MAIN);
    lv_obj_set_style_pad_top(page, UI_SCANNOW_PAGE_PAD, 0);

#if defined(HDZBOXPRO)
    // Mode selector at the very top of the page: three buttons in a row.
    // Sits in its own absolute-positioned container so it isn't constrained
    // by cont1's grid template (which only has 3 narrow columns).
    {
        lv_obj_t *cont_mode = lv_obj_create(page);
        lv_obj_set_size(cont_mode, 780, 56);
        lv_obj_clear_flag(cont_mode, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(cont_mode, &style_scan, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(cont_mode, 0, 0);
        lv_obj_set_style_border_width(cont_mode, 0, 0);
        lv_obj_set_style_pad_all(cont_mode, 0, 0);

        static const char *mode_names[3] = {"HDZero", "Analog", "Auto/Both"};
        for (int i = 0; i < 3; i++) {
            mode_btns[i] = lv_btn_create(cont_mode);
            lv_obj_set_size(mode_btns[i], 220, 44);
            lv_obj_set_pos(mode_btns[i], 30 + i * 240, 6);
            lv_obj_t *lbl = lv_label_create(mode_btns[i]);
            lv_label_set_text(lbl, mode_names[i]);
            lv_obj_set_style_text_color(lbl,
                                        lv_color_make(0xFF, 0xFF, 0xFF), 0);
            lv_obj_set_style_text_font(lbl, UI_SCANNOW_NOTE_FONT, 0);
            lv_obj_center(lbl);
        }

        scan_mode = (scan_mode_t)g_setting.source.scan_mode_initial;
        if (scan_mode > SCAN_MODE_AUTO) scan_mode = SCAN_MODE_HDZERO;
        update_mode_btn_focus();
    }
#endif

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
#if defined(HDZBOXPRO)
    snprintf(buf, sizeof(buf), "%s",
             _lang("Dial to pick mode, press Enter to scan"));
#else
    snprintf(buf, sizeof(buf), "%s\n %s\n %s",
             _lang("When scanning is complete, use the"),
             _lang("dial to select a channel and press"),
             _lang("the Enter button to choose"));
#endif
    lv_label_set_text(label2, buf);
    lv_obj_set_style_text_font(label2, UI_SCANNOW_NOTE_FONT, 0);
    lv_obj_set_style_text_align(label2, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label2, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(label2, UI_SCANNOW_NOTE_PAD, 0);
    lv_label_set_long_mode(label2, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(label2, LV_GRID_ALIGN_START, 2, 1,
                         LV_GRID_ALIGN_START, 0, 3);


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

#if defined(HDZBOXPRO)
    // Results list for Analog and Auto modes. Hidden by default. Created in
    // cont2 with absolute positioning so it covers the full container
    // regardless of the grid template used by the channel cells.
    auto_list = lv_list_create(cont2);
    lv_obj_set_pos(auto_list, 0, 0);
    lv_obj_set_size(auto_list, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(auto_list, LV_OBJ_FLAG_HIDDEN);
    set_results_widget_visibility();
#endif

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
static int compare_results_desc(const void *a, const void *b); // forward decl

static uint8_t analog_rssi_to_strength(uint16_t rssi_mv) {
    uint16_t cmin = g_setting.analog_rssi.calib_min;
    uint16_t cmax = g_setting.analog_rssi.calib_max;
    if (cmax <= cmin || rssi_mv <= cmin) return 0;
    if (rssi_mv >= cmax) return 100;
    return (uint8_t)(((uint32_t)(rssi_mv - cmin) * 100u) / (cmax - cmin));
}

// Scan all 48 analog channels and populate auto_results (sorted by strength).
// Results are rendered in auto_list — same widget Auto mode uses, just with
// analog-only entries.
static int8_t scan_now_analog(void) {
    char buf[128];
    uint16_t rssi_mv;
    bool valid;
    auto_result_count = 0;

    snprintf(buf, sizeof(buf), "%s...", _lang("Scanning"));
    lv_label_set_text(label, buf);
    lv_bar_set_range(progressbar, 0, 48);
    lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
    lv_timer_handler();

    rtc6715.init(1, 0);
    scan_core_notify_analog_powered_on();

    for (uint8_t i = 0; i < 48 && auto_result_count < AUTO_RESULT_MAX; i++) {
        scan_probe_analog(i, &rssi_mv, &valid);
        if (valid) {
            auto_result_t *out = &auto_results[auto_result_count++];
            out->freq_mhz       = scan_analog_idx_to_mhz[i];
            out->protocol       = PROTOCOL_ANALOG;
            out->hdz_channel    = -1;
            out->analog_channel = (int8_t)i;
            out->strength       = analog_rssi_to_strength(rssi_mv);
        }
        lv_bar_set_value(progressbar, i + 1, LV_ANIM_OFF);
        lv_timer_handler();
    }

    qsort(auto_results, auto_result_count, sizeof(auto_result_t),
          compare_results_desc);

    auto_focused_btn = NULL;
    if (auto_list) lv_obj_clean(auto_list);
    for (size_t i = 0; i < auto_result_count; i++) {
        char row[64];
        const auto_result_t *res = &auto_results[i];
        snprintf(row, sizeof(row), "%s   %3u%%",
                 channel2str_tagged(PROTOCOL_ANALOG,
                                    (uint8_t)res->analog_channel + 1),
                 res->strength);
        if (auto_list) lv_list_add_btn(auto_list, NULL, row);
    }

    auto_select_index = 0;
    auto_focused_btn = lv_obj_get_child(auto_list, 0);
    if (auto_focused_btn) lv_obj_add_state(auto_focused_btn, LV_STATE_FOCUSED);

    lv_label_set_text(label, _lang("Scanning done"));
    lv_bar_set_range(progressbar, 0, 14);

    return auto_result_count ? (int8_t)auto_result_count : -1;
}

static int compare_results_desc(const void *a, const void *b) {
    const auto_result_t *ra = a;
    const auto_result_t *rb = b;
    if (rb->strength != ra->strength)
        return (int)rb->strength - (int)ra->strength;
    return (int)ra->freq_mhz - (int)rb->freq_mhz;
}

static int8_t scan_now_auto(void) {
    char buf[128];
    auto_result_count = 0;

    // Power on both radios for the scan.
    rtc6715.init(1, 0);
    scan_core_notify_analog_powered_on();
    HDZero_open(g_setting.source.hdzero_bw);

    snprintf(buf, sizeof(buf), "%s...", _lang("Scanning"));
    lv_label_set_text(label, buf);
    lv_bar_set_range(progressbar, 0, (int32_t)scan_freq_table_len);
    lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
    lv_timer_handler();

    for (size_t i = 0; i < scan_freq_table_len; i++) {
        scan_result_t r = scan_probe_both(&scan_freq_table[i]);
        if (r.protocol != PROTOCOL_NONE && auto_result_count < AUTO_RESULT_MAX) {
            auto_result_t *out = &auto_results[auto_result_count++];
            out->freq_mhz       = scan_freq_table[i].freq_mhz;
            out->protocol       = r.protocol;
            out->hdz_channel    = scan_freq_table[i].hdz_channel;
            out->analog_channel = scan_freq_table[i].analog_channel;
            out->strength       = r.strength;
        }
        lv_bar_set_value(progressbar, (int32_t)(i + 1), LV_ANIM_OFF);
        lv_timer_handler();
    }

    qsort(auto_results, auto_result_count, sizeof(auto_result_t),
          compare_results_desc);

    // Render results into the list widget.
    auto_focused_btn = NULL;
    if (auto_list) lv_obj_clean(auto_list);
    for (size_t i = 0; i < auto_result_count; i++) {
        char row[64];
        const auto_result_t *res = &auto_results[i];
        uint8_t ch_idx = (res->protocol == PROTOCOL_HDZ)
                            ? (uint8_t)res->hdz_channel + 1
                            : (uint8_t)res->analog_channel + 1;
        snprintf(row, sizeof(row), "%s   %3u%%",
                 channel2str_tagged((int)res->protocol, ch_idx),
                 res->strength);
        if (auto_list) lv_list_add_btn(auto_list, NULL, row);
    }

    auto_select_index = 0;
    auto_focused_btn = lv_obj_get_child(auto_list, 0);
    if (auto_focused_btn) {
        lv_obj_add_state(auto_focused_btn, LV_STATE_FOCUSED);
    }
    lv_label_set_text(label, _lang("Scanning done"));

    // Restore progress bar range to the default used by HDZ/Analog modes.
    lv_bar_set_range(progressbar, 0, 14);

    return auto_result_count ? (int8_t)auto_result_count : -1;
}

static int8_t scan_now_dispatch(void) {
    switch (scan_mode) {
    case SCAN_MODE_HDZERO: return scan_now_hdzero();
    case SCAN_MODE_ANALOG: return scan_now_analog();
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

#if defined(HDZBOXPRO)
// Run a scan in the current scan_mode and transition the page into RESULTS
// state. Called from the click handler after the user picks a mode.
static void start_scan_in_current_mode(void) {
    auto_scaned_cnt = scan();
    LOGI("scan return :%d", auto_scaned_cnt);
    page_state = SCAN_PAGE_RESULTS;
    set_results_widget_visibility();
}
#endif

static void page_scannow_enter() {
#if defined(HDZBOXPRO)
    // Land in IDLE — user picks a mode with the dial, click runs the scan.
    page_state = SCAN_PAGE_IDLE;
    set_results_widget_visibility();
    update_mode_btn_focus();
    lv_label_set_text(label, _lang("Scan Ready"));
    lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
    auto_scaned_cnt = 0;
#else
    auto_scaned_cnt = scan();
    LOGI("scan return :%d", auto_scaned_cnt);

    if (auto_scaned_cnt == 1) {
        if (!g_autoscan_exit)
            g_autoscan_exit = true;

        app_state_push(APP_STATE_VIDEO);
        app_switch_to_hdzero(false);
    }

    if (auto_scaned_cnt == -1)
        submenu_exit();
#endif
}

static void page_scannow_exit() {
#if defined(HDZBOXPRO)
    if (page_state == SCAN_PAGE_RESULTS &&
        (scan_mode == SCAN_MODE_ANALOG || scan_mode == SCAN_MODE_AUTO)) {
        rtc6715.init(0, 0); // power down analog RX on exit
    }
    page_state = SCAN_PAGE_IDLE;
#endif
    // HDZero_Close() is idempotent (resets DM5680 baseband and clears
    // hdzero_open flag). Always call so a session that ran scan_now_hdzero
    // but exited in a different mode still tears down the HDZ pipeline.
    HDZero_Close();
}

static void page_scannow_on_roller(uint8_t key) {
#if defined(HDZBOXPRO)
    if (page_state == SCAN_PAGE_IDLE) {
        // Cycle through the 3 mode buttons.
        int new_mode = (int)scan_mode;
        if (key == DIAL_KEY_UP && new_mode + 1 < 3) {
            new_mode++;
        } else if (key == DIAL_KEY_DOWN && new_mode > 0) {
            new_mode--;
        }
        if (new_mode != (int)scan_mode) {
            scan_mode = (scan_mode_t)new_mode;
            update_mode_btn_focus();
        }
        return;
    }
    // RESULTS state.
    if (scan_mode == SCAN_MODE_ANALOG || scan_mode == SCAN_MODE_AUTO) {
        if (auto_result_count == 0) return;
        int new_index = auto_select_index;
        if (key == DIAL_KEY_UP && auto_select_index + 1 < (int)auto_result_count) {
            new_index = auto_select_index + 1;
        } else if (key == DIAL_KEY_DOWN && auto_select_index > 0) {
            new_index = auto_select_index - 1;
        }
        if (new_index != auto_select_index) {
            if (auto_focused_btn) {
                lv_obj_clear_state(auto_focused_btn, LV_STATE_FOCUSED);
            }
            auto_select_index = new_index;
            auto_focused_btn = lv_obj_get_child(auto_list, auto_select_index);
            if (auto_focused_btn) {
                lv_obj_add_state(auto_focused_btn, LV_STATE_FOCUSED);
                lv_obj_scroll_to_view(auto_focused_btn, LV_ANIM_ON);
            }
        }
        return;
    }
#endif
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
    if (page_state == SCAN_PAGE_IDLE) {
        // Click on a mode button: persist selection and trigger the scan.
        g_setting.source.scan_mode_initial = (uint8_t)scan_mode;
        ini_putl("source", "scan_mode_initial",
                 g_setting.source.scan_mode_initial, SETTING_INI);
        start_scan_in_current_mode();
        return;
    }
    // RESULTS state: click selects a scan result and enters video.
    apply_auto_detect_for_mode(scan_mode);
    if (scan_mode == SCAN_MODE_ANALOG || scan_mode == SCAN_MODE_AUTO) {
        if (auto_result_count == 0) return;
        const auto_result_t *res = &auto_results[auto_select_index];
        app_state_push(APP_STATE_VIDEO);
        if (res->protocol == PROTOCOL_HDZ) {
            g_setting.scan.channel = (uint8_t)res->hdz_channel + 1;
            ini_putl("scan", "channel", g_setting.scan.channel, SETTING_INI);
            app_switch_to_hdzero(true);
            g_source_info.source = SOURCE_HDZERO;
        } else if (res->protocol == PROTOCOL_ANALOG) {
            g_setting.source.analog_channel = (uint8_t)res->analog_channel + 1;
            ini_putl("source", "analog_channel",
                     g_setting.source.analog_channel, SETTING_INI);
            app_switch_to_analog(0);
            g_source_info.source = SOURCE_AV_MODULE;
        }
        dvr_select_audio_source(g_setting.record.audio_source);
        dvr_enable_line_out(true);
        return;
    }
    // HDZero mode RESULTS — fall through to existing HDZ select.
#endif
    app_state_push(APP_STATE_VIDEO);
    app_switch_to_hdzero(false);
#if defined(HDZBOXPRO)
    g_source_info.source = SOURCE_HDZERO;
    dvr_select_audio_source(g_setting.record.audio_source);
    dvr_enable_line_out(true);
#endif
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