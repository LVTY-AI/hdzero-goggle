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
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)
typedef enum {
    SCAN_MODE_HDZERO = 0,
    SCAN_MODE_ANALOG = 1,
    SCAN_MODE_AUTO   = 2,
} scan_mode_t;

// Modes offered by the picker. BoxPro and Goggle 2 have the built-in analog
// receiver, so they get HDZero / Analog / Dual. Goggle 1 has no built-in
// analog, so its picker is a single "Scan" button (plus "Choose from Last
// Scan" once results exist).
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
#define SCAN_MODE_COUNT 3
#else
#define SCAN_MODE_COUNT 1
#endif

// Two-state UI: page lands in IDLE (user picks Mode), click runs a scan and
// transitions to RESULTS. Right button returns to the menu (existing exit).
typedef enum {
    SCAN_PAGE_IDLE    = 0,
    SCAN_PAGE_RESULTS = 1,
} scan_page_state_t;

static scan_mode_t scan_mode = SCAN_MODE_HDZERO;
static scan_page_state_t page_state = SCAN_PAGE_IDLE;
static lv_obj_t *mode_btns[3];     // 0=HDZero, 1=Analog, 2=Auto/Both
static bool page_focused = false;  // true only while the page holds input focus;
                                   // gates the mode-picker green so it does not
                                   // show while merely hovered in the sidebar.
static int idle_sel = 0;           // picker cursor: 0..SCAN_MODE_COUNT-1 pick a
                                   // mode; SCAN_MODE_COUNT = "Choose from Last Scan".
static lv_obj_t *last_scan_btn = NULL;
static lv_obj_t *exit_note = NULL;
static bool results_receiver_parked = false; // HDZ RX is parked on the focused
                                             // result (right after a scan), so a
                                             // pick enters video near-instantly

static void update_mode_btn_focus(void) {
    // Theme button background defaults are very light on this skin and the
    // label text ends up invisible. Style each button explicitly: dark grey
    // background for unfocused, green for focused, white text always.
    for (int i = 0; i < SCAN_MODE_COUNT; i++) {
        if (!mode_btns[i]) continue;
        bool is_focused = page_focused && (i == idle_sel);
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
    if (last_scan_btn) {
        bool sel = page_focused && (idle_sel == SCAN_MODE_COUNT);
        lv_obj_set_style_bg_color(last_scan_btn,
                                  sel ? lv_color_make(0, 0xA0, 0)
                                      : lv_color_make(0x40, 0x40, 0x40),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(last_scan_btn, LV_OPA_100, LV_PART_MAIN);
        lv_obj_set_style_border_width(last_scan_btn, sel ? 2 : 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(last_scan_btn,
                                      lv_color_make(0xFF, 0xFF, 0xFF),
                                      LV_PART_MAIN);
        if (sel)
            lv_obj_add_state(last_scan_btn, LV_STATE_FOCUSED);
        else
            lv_obj_clear_state(last_scan_btn, LV_STATE_FOCUSED);
    }
}

#define AUTO_RESULT_MAX 64 // up to 48 analog channels or ~55 freq-table rows

typedef struct {
    uint16_t freq_mhz;
    scan_protocol_t protocol;
    int8_t   hdz_band;    // 0=Race, 1=Low, -1=n/a (analog row)
    int8_t   hdz_channel;
    int8_t   analog_channel;
    int8_t   hdz_bw;      // 0=Wide, 1=Narrow, -1=n/a (analog row)
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

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
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
#endif

// Style a freshly-added auto_list row. The theme default would render the
// row's bg and label as near-white, hiding the text entirely. is_focused
// gives the selected row a distinct background so the dial cursor is visible.
static void style_auto_list_row(lv_obj_t *btn, bool is_focused) {
    if (!btn) return;
    lv_obj_set_style_bg_color(btn,
                              is_focused ? lv_color_make(0, 0xA0, 0)
                                         : lv_color_make(0x30, 0x30, 0x30),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_100, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
    // lv_list_add_btn wraps the label in the first child; reach in to color it.
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) {
        lv_obj_set_style_text_color(lbl, lv_color_make(0xFF, 0xFF, 0xFF), 0);
        lv_obj_set_style_text_font(lbl, UI_SCANNOW_NOTE_FONT, 0);
    }
}

static void set_results_widget_visibility(void) {
    // All three modes (HDZero, Analog, Auto/Both) now render into auto_list,
    // so the legacy signal-bar grid stays hidden. The list stays on screen
    // whenever a previous scan left results -- including the IDLE picker, so
    // the channel list is visible while choosing a mode / Rescan / "Choose
    // from Last Scan". Only an empty history hides it.
    bool show_list = (page_state == SCAN_PAGE_RESULTS) || (auto_result_count > 0);

    if (auto_list) {
        if (show_list) {
            lv_obj_clear_flag(auto_list, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(auto_list, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Grid cells are unused on BoxPro now; keep them hidden always.
    for (int i = 0; i < BASE_CH_NUM; i++) {
        if (channel_tb[i].img0)  lv_obj_add_flag(channel_tb[i].img0,  LV_OBJ_FLAG_HIDDEN);
        if (channel_tb[i].label) lv_obj_add_flag(channel_tb[i].label, LV_OBJ_FLAG_HIDDEN);
        if (channel_tb[i].img1)  lv_obj_add_flag(channel_tb[i].img1,  LV_OBJ_FLAG_HIDDEN);
    }

    // "Choose from Last Scan" is offered only when a previous scan left results.
    if (last_scan_btn) {
        if (auto_result_count > 0)
            lv_obj_clear_flag(last_scan_btn, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(last_scan_btn, LV_OBJ_FLAG_HIDDEN);
    }
#if SCAN_MODE_COUNT == 1
    // ... and so is G1's Rescan button (without results the page scans on
    // entry, so the picker never shows), and the bottom exit hint, which on
    // G1 only applies while the picker is on offer.
    if (mode_btns[0]) {
        if (auto_result_count > 0)
            lv_obj_clear_flag(mode_btns[0], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(mode_btns[0], LV_OBJ_FLAG_HIDDEN);
    }
    if (exit_note) {
        if (auto_result_count > 0)
            lv_obj_clear_flag(exit_note, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(exit_note, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}
#endif

static lv_obj_t *page_scannow_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[256];

    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_scan, LV_PART_MAIN);
    lv_obj_set_style_pad_top(page, UI_SCANNOW_PAGE_PAD, 0);

#if SCAN_MODE_COUNT > 1
    // Mode selector at the top: one button per scan mode (BoxPro/G2). G1 has
    // a single protocol, so instead of mode buttons it scans on page entry
    // and gets a "Rescan" button (created next to "Choose from Last Scan"
    // below) once results exist. Sits in its own absolute-positioned
    // container so it isn't constrained by cont1's grid.
    {
        lv_obj_t *cont_mode = lv_obj_create(page);
        lv_obj_set_size(cont_mode, 780, 56);
        lv_obj_clear_flag(cont_mode, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(cont_mode, &style_scan, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(cont_mode, 0, 0);
        lv_obj_set_style_border_width(cont_mode, 0, 0);
        lv_obj_set_style_pad_all(cont_mode, 0, 0);

        static const char *mode_names[3] = {"HDZero", "Analog", "Dual"};
        for (int i = 0; i < SCAN_MODE_COUNT; i++) {
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
        if ((int)scan_mode >= SCAN_MODE_COUNT) scan_mode = SCAN_MODE_HDZERO;
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
#if SCAN_MODE_COUNT > 1
    snprintf(buf, sizeof(buf), "%s",
             _lang("Dial to pick mode, press Enter to scan"));
#else
    // G1 scans on page entry; this note only shows in the picker, where the
    // choice is Rescan vs Choose from Last Scan.
    snprintf(buf, sizeof(buf), "%s",
             _lang("Dial to pick, press Enter to select"));
#endif
    lv_label_set_text(label2, buf);
    lv_obj_set_style_text_font(label2, UI_SCANNOW_NOTE_FONT, 0);
    lv_obj_set_style_text_align(label2, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label2, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(label2, UI_SCANNOW_NOTE_PAD, 0);
    lv_label_set_long_mode(label2, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(label2, LV_GRID_ALIGN_START, 2, 1,
                         LV_GRID_ALIGN_START, 0, 1);

    // "Choose from Last Scan" -- re-opens the persisted results without
    // rescanning -- and, on G1 only, a "Rescan" button beside it. They live
    // in a flex row in the scanner's right column in line with the progress
    // bar, shown only when a previous scan produced results, and reached by
    // dialing past the last mode button. Content-sized WIDTH and
    // START-aligned: the stretched right grid column runs past the scanner
    // container on G1/G2, which clipped a stretched button. The HEIGHT is
    // fixed: an LV_SIZE_CONTENT-tall button with a centered label computes
    // slightly short and clips its own bottom edge.
#if defined(HDZBOXPRO)
#define PICKER_BTN_HEIGHT 32 // fits the 40px scanner grid row
#else
#define PICKER_BTN_HEIGHT 48 // fits the 60px scanner grid row (G1/G2)
#endif
    // The row is fixed-height too, 8px taller than its top-aligned children:
    // content-sized containers compute their height slightly short of the
    // children and clip their bottoms, so leave the shortfall in empty slack
    // below the buttons instead.
    lv_obj_t *btn_row = lv_obj_create(cont1);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_SIZE_CONTENT, PICKER_BTN_HEIGHT + 8);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(btn_row, 12, 0);
    lv_obj_set_grid_cell(btn_row, LV_GRID_ALIGN_START, 2, 1,
                         LV_GRID_ALIGN_CENTER, 1, 1);

#if SCAN_MODE_COUNT == 1
    // G1's single "mode button" is a Rescan action (idle_sel 0). With no
    // results the page scans on entry instead, so this button only ever
    // appears next to "Choose from Last Scan".
    mode_btns[0] = lv_btn_create(btn_row);
    lv_obj_set_size(mode_btns[0], LV_SIZE_CONTENT, PICKER_BTN_HEIGHT);
    lv_obj_set_style_pad_hor(mode_btns[0], 12, 0);
    lv_obj_set_style_pad_ver(mode_btns[0], 0, 0);
    {
        lv_obj_t *rs_lbl = lv_label_create(mode_btns[0]);
        lv_label_set_text(rs_lbl, _lang("Rescan"));
        lv_obj_set_style_text_color(rs_lbl, lv_color_make(0xFF, 0xFF, 0xFF), 0);
        lv_obj_set_style_text_font(rs_lbl, UI_SCANNOW_NOTE_FONT, 0);
        lv_obj_center(rs_lbl);
    }
    lv_obj_add_flag(mode_btns[0], LV_OBJ_FLAG_HIDDEN);
#endif

    last_scan_btn = lv_btn_create(btn_row);
    lv_obj_set_size(last_scan_btn, LV_SIZE_CONTENT, PICKER_BTN_HEIGHT);
    lv_obj_set_style_pad_hor(last_scan_btn, 12, 0);
    lv_obj_set_style_pad_ver(last_scan_btn, 0, 0);
    {
        lv_obj_t *ls_lbl = lv_label_create(last_scan_btn);
        lv_label_set_text(ls_lbl, _lang("Choose from Last Scan"));
        lv_obj_set_style_text_color(ls_lbl, lv_color_make(0xFF, 0xFF, 0xFF), 0);
        lv_obj_set_style_text_font(ls_lbl, UI_SCANNOW_NOTE_FONT, 0);
        lv_obj_center(ls_lbl);
    }
    lv_obj_add_flag(last_scan_btn, LV_OBJ_FLAG_HIDDEN);
    // Apply the explicit dark/unfocused styling now: on G1 the page can land
    // straight in RESULTS (scan on entry) without any picker interaction, and
    // until update_mode_btn_focus runs the theme default renders both buttons
    // white-on-white.
    update_mode_btn_focus();

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

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)
    // Scrollable results list. Hidden by default. Created in cont2 with absolute
    // positioning so it covers the full container regardless of the grid
    // template used by the (now hidden) legacy channel cells.
    auto_list = lv_list_create(cont2);
    lv_obj_set_pos(auto_list, 0, 0);
    lv_obj_set_size(auto_list, lv_pct(100), lv_pct(100));
    // Theme default has near-white list bg + near-white label text, so
    // populated rows render invisibly. Force dark background.
    lv_obj_set_style_bg_color(auto_list, lv_color_make(0x20, 0x20, 0x20),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(auto_list, LV_OPA_100, LV_PART_MAIN);
    lv_obj_set_style_border_width(auto_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(auto_list, 4, LV_PART_MAIN);
    lv_obj_add_flag(auto_list, LV_OBJ_FLAG_HIDDEN);
    set_results_widget_visibility();
#endif

    // Exit hint at the page bottom, mirroring the playback page's note style:
    // some users thought they were stuck after a scan or in the mode picker.
    exit_note = lv_label_create(page);
    snprintf(buf, sizeof(buf), "*%s", _lang("Long press the Enter button to exit"));
    lv_label_set_text(exit_note, buf);
    lv_obj_set_style_text_font(exit_note, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_align(exit_note, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(exit_note, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_label_set_long_mode(exit_note, LV_LABEL_LONG_WRAP);
#if SCAN_MODE_COUNT == 1
    // G1 backs out automatically when a scan finds nothing, so the hint only
    // applies when the picker (Rescan / Choose from Last Scan) is on offer;
    // set_results_widget_visibility toggles it with the buttons.
    lv_obj_add_flag(exit_note, LV_OBJ_FLAG_HIDDEN);
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

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)
static int compare_results_desc(const void *a, const void *b) {
    const auto_result_t *ra = a;
    const auto_result_t *rb = b;
    if (rb->strength != ra->strength)
        return (int)rb->strength - (int)ra->strength;
    return (int)ra->freq_mhz - (int)rb->freq_mhz;
}

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
static uint8_t analog_rssi_to_strength(uint16_t rssi_mv) {
    uint16_t cmin = g_setting.analog_rssi.calib_min;
    uint16_t cmax = g_setting.analog_rssi.calib_max;
    if (cmax <= cmin || rssi_mv <= cmin) return 0;
    if (rssi_mv >= cmax) return 100;
    return (uint8_t)(((uint32_t)(rssi_mv - cmin) * 100u) / (cmax - cmin));
}
#endif

// DM6302 gain table is 0..60; normalize to 0..100 to match the analog scale.
static uint8_t hdz_gain_to_strength(uint8_t gain) {
    return gain >= 60 ? 100 : (uint8_t)((uint32_t)gain * 100u / 60u);
}

// Results are keyed by frequency so a channel found in two bandwidth passes
// (or as both protocols at one freq) collapses to a single row. HDZ always
// wins a frequency over analog; within a protocol the stronger reading wins.
static int find_result_idx_by_freq(uint16_t freq) {
    for (size_t i = 0; i < auto_result_count; i++)
        if (auto_results[i].freq_mhz == freq)
            return (int)i;
    return -1;
}

static void upsert_hdz(const scan_freq_entry_t *e, uint8_t bw, uint8_t strength) {
    int idx = find_result_idx_by_freq(e->freq_mhz);
    if (idx >= 0) {
        auto_result_t *x = &auto_results[idx];
        if (x->protocol == PROTOCOL_HDZ && x->strength >= strength)
            return; // keep the stronger HDZ reading
    } else {
        if (auto_result_count >= AUTO_RESULT_MAX)
            return;
        idx = (int)auto_result_count++;
    }
    auto_result_t *o = &auto_results[idx];
    o->freq_mhz       = e->freq_mhz;
    o->protocol       = PROTOCOL_HDZ;
    o->hdz_band       = e->hdz_band;
    o->hdz_channel    = e->hdz_channel;
    o->analog_channel = -1;
    o->hdz_bw         = (int8_t)bw;
    o->strength       = strength;
}

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
static void upsert_analog(uint16_t freq, int8_t analog_ch, uint8_t strength) {
    int idx = find_result_idx_by_freq(freq);
    if (idx >= 0) {
        auto_result_t *x = &auto_results[idx];
        if (x->protocol == PROTOCOL_HDZ)
            return; // HDZ wins this frequency; never downgrade to analog
        if (x->strength >= strength)
            return;
    } else {
        if (auto_result_count >= AUTO_RESULT_MAX)
            return;
        idx = (int)auto_result_count++;
    }
    auto_result_t *o = &auto_results[idx];
    o->freq_mhz       = freq;
    o->protocol       = PROTOCOL_ANALOG;
    o->hdz_band       = -1;
    o->hdz_channel    = -1;
    o->analog_channel = analog_ch;
    o->hdz_bw         = -1;
    o->strength       = strength;
}
#endif

// Render the (already sorted) auto_results into auto_list. Naming is
// band-aware: HDZ rows use their own entry band, not the global setting, so a
// Lowband row reads "L3/HDZ" even while the live band is Raceband. Focuses
// row 0.
static void render_auto_results_list(void) {
    auto_focused_btn = NULL;
    if (!auto_list) return;
    lv_obj_clean(auto_list);

    for (size_t i = 0; i < auto_result_count; i++) {
        const auto_result_t *res = &auto_results[i];
        char name[20];
        if (res->protocol == PROTOCOL_HDZ) {
            uint8_t lb = (res->hdz_band == 1) ? 1 : 0;
            if (g_setting.source.hdzero_bw == SETTING_SOURCES_HDZERO_BW_BOTH) {
                // Both mode: tag which bandwidth this channel was found at.
                snprintf(name, sizeof(name), "%s/HDZ %c",
                         channel2str(1, lb, (uint8_t)res->hdz_channel + 1),
                         (res->hdz_bw == 1) ? 'N' : 'W');
            } else {
                snprintf(name, sizeof(name), "%s/HDZ",
                         channel2str(1, lb, (uint8_t)res->hdz_channel + 1));
            }
        } else {
            snprintf(name, sizeof(name), "%s/ANA",
                     channel2str(0, 0, (uint8_t)res->analog_channel + 1));
        }
        char row[64];
        snprintf(row, sizeof(row), "%s   %3u%%", name, res->strength);
        lv_obj_t *btn = lv_list_add_btn(auto_list, NULL, row);
        style_auto_list_row(btn, i == 0);
    }

    auto_select_index = 0;
    auto_focused_btn = lv_obj_get_child(auto_list, 0);
    if (auto_focused_btn) lv_obj_add_state(auto_focused_btn, LV_STATE_FOCUSED);
}

// After a BW=Both scan, leave the receiver on the bandwidth of the focused
// (strongest, row 0) result and remember it as the detected BW. Picking the
// default highlighted channel then enters video without a bandwidth reopen --
// HDZero_open() resets the baseband (a black + green flash) only when the BW
// changes, and a Both scan otherwise ends parked on Narrow (the last sweep
// pass), so a Wide pick would always flash. Falls back to Wide (the common
// default) when nothing was found or the top row is analog.
static void scan_settle_focused_bw(void) {
    if (g_setting.source.hdzero_bw != SETTING_SOURCES_HDZERO_BW_BOTH)
        return;
    uint8_t bw = SETTING_SOURCES_HDZERO_BW_WIDE;
    if (auto_result_count > 0 &&
        auto_results[0].protocol == PROTOCOL_HDZ &&
        auto_results[0].hdz_bw >= 0)
        bw = (uint8_t)auto_results[0].hdz_bw;
    g_hdz_detected_bw = bw;
    HDZero_open(bw);
}

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
// Scan all 48 analog channels into auto_results (sorted by strength).
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
            out->hdz_band       = -1;
            out->hdz_channel    = -1;
            out->analog_channel = (int8_t)i;
            out->hdz_bw         = -1;
            out->strength       = analog_rssi_to_strength(rssi_mv);
        }
        lv_bar_set_value(progressbar, i + 1, LV_ANIM_OFF);
        lv_timer_handler();
    }

    qsort(auto_results, auto_result_count, sizeof(auto_result_t),
          compare_results_desc);
    render_auto_results_list();

    lv_label_set_text(label, _lang("Scanning done"));
    lv_bar_set_range(progressbar, 0, 14);
    return auto_result_count ? (int8_t)auto_result_count : -1;
}
#endif

// Scan every HDZ channel across BOTH bands (Race R1-R8/E1/F1/F2/F4 + Low
// L1-L8 = 20) by walking the freq table's HDZ entries. Replaces the legacy
// fixed band-toggle grid; results render in the shared list.
static int8_t scan_now_hdzero_list(void) {
    char buf[128];
    uint8_t gain;
    bool valid;
    auto_result_count = 0;

    uint8_t bws[2];
    int nbw = scan_hdz_bw_list(bws); // 1, or 2 when BW=Both

    snprintf(buf, sizeof(buf), "%s...", _lang("Scanning"));
    lv_label_set_text(label, buf);
    lv_bar_set_range(progressbar, 0, (int32_t)scan_freq_table_len * nbw);
    lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
    lv_timer_handler();

    int32_t prog = 0;
    for (int b = 0; b < nbw; b++) {
        HDZero_open(bws[b]);
        usleep(200000); // let the baseband settle once per bandwidth pass
        for (size_t i = 0; i < scan_freq_table_len; i++) {
            const scan_freq_entry_t *e = &scan_freq_table[i];
            if (e->hdz_channel >= 0 && e->hdz_band >= 0) {
                scan_probe_hdzero((uint8_t)e->hdz_band, (uint8_t)e->hdz_channel,
                                  &gain, &valid);
                if (valid)
                    upsert_hdz(e, bws[b], hdz_gain_to_strength(gain));
            }
            lv_bar_set_value(progressbar, ++prog, LV_ANIM_OFF);
            lv_timer_handler();
        }
    }

    qsort(auto_results, auto_result_count, sizeof(auto_result_t),
          compare_results_desc);
    render_auto_results_list();

    lv_label_set_text(label, _lang("Scanning done"));
    lv_bar_set_range(progressbar, 0, 14);
    return auto_result_count ? (int8_t)auto_result_count : -1;
}

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
static int8_t scan_now_auto(void) {
    char buf[128];
    uint16_t rssi_mv;
    uint8_t gain;
    bool valid;
    auto_result_count = 0;

    uint8_t bws[2];
    int nbw = scan_hdz_bw_list(bws); // 1, or 2 when BW=Both

    rtc6715.init(1, 0);
    scan_core_notify_analog_powered_on();

    snprintf(buf, sizeof(buf), "%s...", _lang("Scanning"));
    lv_label_set_text(label, buf);
    // One analog pass + nbw HDZ passes over the table.
    lv_bar_set_range(progressbar, 0, (int32_t)scan_freq_table_len * (nbw + 1));
    lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
    lv_timer_handler();

    int32_t prog = 0;

    // Analog pass — bandwidth-independent.
    for (size_t i = 0; i < scan_freq_table_len; i++) {
        const scan_freq_entry_t *e = &scan_freq_table[i];
        if (e->analog_channel >= 0) {
            scan_probe_analog((uint8_t)e->analog_channel, &rssi_mv, &valid);
            if (valid)
                upsert_analog(e->freq_mhz, e->analog_channel,
                              analog_rssi_to_strength(rssi_mv));
        }
        lv_bar_set_value(progressbar, ++prog, LV_ANIM_OFF);
        lv_timer_handler();
    }

    // HDZ pass(es) — one per selected bandwidth. upsert_hdz gives HDZ priority
    // over analog at the same frequency.
    for (int b = 0; b < nbw; b++) {
        HDZero_open(bws[b]);
        usleep(200000); // settle once per bandwidth pass
        for (size_t i = 0; i < scan_freq_table_len; i++) {
            const scan_freq_entry_t *e = &scan_freq_table[i];
            if (e->hdz_channel >= 0 && e->hdz_band >= 0) {
                scan_probe_hdzero((uint8_t)e->hdz_band, (uint8_t)e->hdz_channel,
                                  &gain, &valid);
                if (valid)
                    upsert_hdz(e, bws[b], hdz_gain_to_strength(gain));
            }
            lv_bar_set_value(progressbar, ++prog, LV_ANIM_OFF);
            lv_timer_handler();
        }
    }

    qsort(auto_results, auto_result_count, sizeof(auto_result_t),
          compare_results_desc);
    render_auto_results_list();

    lv_label_set_text(label, _lang("Scanning done"));
    lv_bar_set_range(progressbar, 0, 14);
    return auto_result_count ? (int8_t)auto_result_count : -1;
}
#endif

static int8_t scan_now_dispatch(void) {
    // Cast to int: on G1/G2 only the HDZERO case is compiled, and an enum
    // switch missing the other values would trip -Wswitch.
    switch ((int)scan_mode) {
    case SCAN_MODE_HDZERO: return scan_now_hdzero_list();
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    case SCAN_MODE_ANALOG: return scan_now_analog();
    case SCAN_MODE_AUTO:   return scan_now_auto();
#endif
    }
    return -1;
}
#endif

int scan_reinit(void) {
    lv_label_set_text(label, _lang("Scanning ready"));
    lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
    user_clear_signal();
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)
    // Called when the menu reopens from video with Scan Now as the current
    // page (after selecting a scan result into video). Drop the focused-green
    // presentation -- mode button and selected row -- the same way page exit
    // does; the results list itself stays visible. Also park the state in
    // IDLE so a later page exit doesn't tear down the analog RX that is now
    // the live video receiver.
    page_state = SCAN_PAGE_IDLE;
    page_focused = false;
    results_receiver_parked = false; // video retuned the receiver
    update_mode_btn_focus();
    if (auto_focused_btn) {
        lv_obj_clear_state(auto_focused_btn, LV_STATE_FOCUSED);
        style_auto_list_row(auto_focused_btn, false);
    }
#endif
    lv_timer_handler();
    return 0;
}

int scan(void) {
    g_scanning = true;
    // SOURCE_HDZERO is set only for the HDZ mode path; Analog/Auto manage source on click.
    if (scan_mode == SCAN_MODE_HDZERO) {
        g_source_info.source = SOURCE_HDZERO;
    }
    int8_t ret = scan_now_dispatch();
    // Park the HDZ receiver on the focused result's bandwidth so the default
    // pick enters video without a Wide<->Narrow reopen flash (a Both scan
    // otherwise ends on Narrow). Skip pure analog scans -- they never open the
    // HDZ baseband, and settling would needlessly power it on.
    if (scan_mode != SCAN_MODE_ANALOG) {
        scan_settle_focused_bw();
        results_receiver_parked = true;
    }
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

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)
// Run a scan in the current scan_mode and transition the page into RESULTS
// state. Called from the click handler after the user picks a mode.
static void start_scan_in_current_mode(void) {
    auto_scaned_cnt = scan();
    LOGI("scan return :%d", auto_scaned_cnt);
    if (auto_result_count == 0) {
#if SCAN_MODE_COUNT == 1
        // G1: there is no mode picker to fall back to, so back out to the
        // main menu -- the next click on Scan Now starts a fresh scan
        // directly. Boot autoscan stays on the page so its retry loop keeps
        // working. The "no signals" text is set after submenu_exit() because
        // the page's exit handler resets the label to "Scan Ready".
        if (g_autoscan_exit) {
            submenu_exit();
            // The empty scan wiped the previous results; hide the stale
            // Rescan / Choose from Last Scan buttons and list with it.
            set_results_widget_visibility();
            lv_label_set_text(label, _lang("Scanning Done. No Signals Found."));
            lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
            return;
        }
#endif
        // Nothing found: there is no channel list to land in, so drop straight
        // back to the picker -- the user can rescan (or leave) immediately
        // instead of having to back out of an empty results view first.
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
        if (scan_mode == SCAN_MODE_ANALOG || scan_mode == SCAN_MODE_AUTO) {
            rtc6715.init(0, 0); // same analog RX teardown as leaving RESULTS
        }
#endif
        page_state = SCAN_PAGE_IDLE;
        set_results_widget_visibility();
        update_mode_btn_focus();
        lv_label_set_text(label, _lang("Scanning Done. No Signals Found."));
        return;
    }
    page_state = SCAN_PAGE_RESULTS;
    set_results_widget_visibility();
    lv_label_set_text(label, _lang("Scanning Done"));
}
#endif

static void page_scannow_enter() {
    page_focused = true;
    // Boot-time Auto Scan re-enters this page in a loop until a signal is
    // found; scan immediately instead of parking on the picker.
    if (!g_autoscan_exit) {
        start_scan_in_current_mode();
        return;
    }
#if SCAN_MODE_COUNT == 1
    // G1 has a single protocol, so entering the page IS the scan action --
    // the first press scans. Only when a previous scan left results does the
    // page land in the picker (Rescan / Choose from Last Scan) instead, so
    // those results aren't clobbered by an unwanted rescan.
    if (auto_result_count == 0) {
        start_scan_in_current_mode();
        return;
    }
#endif
    // Land in IDLE — user picks a mode (G1: the single Scan button) and the
    // click runs the scan.
    page_state = SCAN_PAGE_IDLE;
    idle_sel = (int)scan_mode;
    set_results_widget_visibility();
    update_mode_btn_focus();
    lv_label_set_text(label, _lang("Scan Ready"));
    lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
    auto_scaned_cnt = 0;
}

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)
// Intercepts the long-press-Enter back gesture. When the page is showing
// scan RESULTS, return to the IDLE mode-picker instead of leaving the page,
// so the user can re-scan in a different mode without re-navigating the menu.
static bool page_scannow_on_back(void) {
    if (page_state == SCAN_PAGE_RESULTS) {
#if SCAN_MODE_COUNT > 1
        // Tear down analog RX the same way exit would, so a subsequent IDLE
        // scan power-on cycle starts from a clean state.
        if (scan_mode == SCAN_MODE_ANALOG || scan_mode == SCAN_MODE_AUTO) {
            rtc6715.init(0, 0);
        }
#endif
        page_state = SCAN_PAGE_IDLE;
        // Keep the results list on screen -- just de-green the selected row --
        // so the channels stay visible from the picker; "Choose from Last
        // Scan" re-focuses them without a rescan.
        if (auto_focused_btn) {
            lv_obj_clear_state(auto_focused_btn, LV_STATE_FOCUSED);
            style_auto_list_row(auto_focused_btn, false);
        }
        update_mode_btn_focus();
        lv_label_set_text(label, _lang("Scan Ready"));
        lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
        auto_scaned_cnt = 0;
        return true; // absorbed -> back to the mode picker
    }
    return false; // IDLE -> exit to main menu
}
#endif

static void page_scannow_exit() {
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)
    if (page_state == SCAN_PAGE_RESULTS &&
        (scan_mode == SCAN_MODE_ANALOG || scan_mode == SCAN_MODE_AUTO)) {
        rtc6715.init(0, 0); // power down analog RX on exit
    }
    page_state = SCAN_PAGE_IDLE;
    results_receiver_parked = false; // other pages may retune the receiver
    // Drop focus and grey the picker, but leave the results list on screen so
    // the user can still glance at the last scan from the sidebar (and reach it
    // via "Choose from Last Scan"). Just de-highlight the selected row so no
    // green lingers while the page is unfocused.
    page_focused = false;
    update_mode_btn_focus();
    if (auto_focused_btn) {
        lv_obj_clear_state(auto_focused_btn, LV_STATE_FOCUSED);
        style_auto_list_row(auto_focused_btn, false);
    }
    lv_label_set_text(label, _lang("Scan Ready"));
    lv_bar_set_value(progressbar, 0, LV_ANIM_OFF);
#endif
    // HDZero_Close() is idempotent (resets DM5680 baseband and clears
    // hdzero_open flag). Always call so a session that ran scan_now_hdzero
    // but exited in a different mode still tears down the HDZ pipeline.
    HDZero_Close();
}

static void page_scannow_on_roller(uint8_t key) {
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)
    if (page_state == SCAN_PAGE_IDLE) {
        // Cycle the picker: modes 0..SCAN_MODE_COUNT-1, then "Choose from Last
        // Scan" (index SCAN_MODE_COUNT) when a previous scan left results.
        int maxsel = (last_scan_btn && auto_result_count > 0)
                         ? SCAN_MODE_COUNT
                         : SCAN_MODE_COUNT - 1;
        int new_sel = idle_sel;
        if (key == DIAL_KEY_UP && idle_sel < maxsel) {
            new_sel = idle_sel + 1;
        } else if (key == DIAL_KEY_DOWN && idle_sel > 0) {
            new_sel = idle_sel - 1;
        }
        if (new_sel != idle_sel) {
            idle_sel = new_sel;
            if (idle_sel < SCAN_MODE_COUNT)
                scan_mode = (scan_mode_t)idle_sel;
            update_mode_btn_focus();
        }
        return;
    }
    // RESULTS state — every mode (HDZero, Analog, Auto/Both) navigates the
    // shared results list.
    {
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
                style_auto_list_row(auto_focused_btn, false);
            }
            auto_select_index = new_index;
            auto_focused_btn = lv_obj_get_child(auto_list, auto_select_index);
            if (auto_focused_btn) {
                lv_obj_add_state(auto_focused_btn, LV_STATE_FOCUSED);
                style_auto_list_row(auto_focused_btn, true);
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
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)
    if (page_state == SCAN_PAGE_IDLE) {
        if (idle_sel == SCAN_MODE_COUNT) {
            // "Choose from Last Scan": re-show the persisted results, no rescan.
            if (auto_result_count > 0) {
                render_auto_results_list();
                page_state = SCAN_PAGE_RESULTS;
                set_results_widget_visibility();
                lv_label_set_text(label, _lang("Last scan"));
            }
            return;
        }
        // Click on a mode button: persist selection and trigger the scan.
        g_setting.source.scan_mode_initial = (uint8_t)scan_mode;
        ini_putl("source", "scan_mode_initial",
                 g_setting.source.scan_mode_initial, SETTING_INI);
        start_scan_in_current_mode();
        return;
    }
    // RESULTS state: click selects a scan result and enters video. All three
    // modes now feed the same auto_results list. (An empty scan never lands in
    // RESULTS -- start_scan_in_current_mode falls back to the picker.)
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    apply_auto_detect_for_mode(scan_mode);
#endif
    {
        if (auto_result_count == 0) return;
        const auto_result_t *res = &auto_results[auto_select_index];
        // Picks from a fresh scan are near-instant (receiver parked on the
        // focused result). Re-opened "Choose from Last Scan" results have to
        // retune/reopen from scratch -- show the loading bar so the wait
        // doesn't look like a hang. HDZ picks only: analog comes up fast,
        // and app_switch_to_analog never clears the bar (only the HDZ path
        // does), so starting it for analog left it stuck on screen.
        if (!results_receiver_parked && res->protocol == PROTOCOL_HDZ)
            progress_bar.start = 1;
        results_receiver_parked = false;
        app_state_push(APP_STATE_VIDEO);
        if (res->protocol == PROTOCOL_HDZ) {
            // Commit the result's band so app_switch_to_hdzero tunes the
            // correct Race/Low frequency, not whatever band was last set.
            if (res->hdz_band >= 0) {
                g_setting.source.hdzero_band = (uint8_t)res->hdz_band;
                ini_putl("source", "hdzero_band",
                         g_setting.source.hdzero_band, SETTING_INI);
            }
            // Remember the bandwidth this result locked at so the live open
            // (hdzero_effective_bw) uses it when BW=Both.
            if (res->hdz_bw >= 0)
                g_hdz_detected_bw = (uint8_t)res->hdz_bw;
            g_setting.scan.channel = (uint8_t)res->hdz_channel + 1;
            ini_putl("scan", "channel", g_setting.scan.channel, SETTING_INI);
            // No loading bar: the receiver is already parked on this result's
            // bandwidth (scan_settle_focused_bw), so HDZ comes up near-instantly.
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
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2) || defined(HDZGOGGLE)
    .on_back = page_scannow_on_back,
#endif
};