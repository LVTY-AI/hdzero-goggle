#include "page_record.h"

#include <stdio.h>
#include <stdlib.h>

#include <minIni.h>

#include "../conf/ui.h"

#include "../core/common.hh"
#include "core/app_state.h"
#include "core/dvr.h"
#include "driver/rtc.h"
#include "lang/language.h"
#include "page_common.h"
#include "ui/ui_style.h"

#define STOP_DELAY_MAX 30
#define STOP_DELAY_STEP 5

static btn_group_t btn_group_record_mode;
static btn_group_t btn_group_format;
static btn_group_t btn_group_bitrate_scale;
static btn_group_t btn_group_record_osd;
static btn_group_t btn_group_file_naming;
static slider_group_t slider_group_stop_delay;

enum {
    ROW_RECORD_MODE = 0,
    ROW_RECORD_FORMAT,
    ROW_RECORD_BITRATE,
    ROW_RECORD_BITRATE_WRAP,
    ROW_RECORD_OSD,
    ROW_NAMING_SCHEME,
    ROW_STOP_DELAY,
    ROW_BACK,

    ROW_COUNT
};

static bool stop_delay_focused = false;
static bool stop_delay_changed = false;

static lv_coord_t col_dsc[] = {UI_RECORD_COLS};
static lv_coord_t row_dsc[] = {UI_RECORD_ROWS};

// Keep the menu in low-to-high order while preserving the numeric values
// already stored by older firmware (1 = 1/2, 2 = 1/4).
static const setting_record_bitrate_scale_t bitrate_btn_to_setting[] = {
    SETTING_RECORD_BITRATE_SCALE_NORMAL,
    SETTING_RECORD_BITRATE_SCALE_QUARTER,
    SETTING_RECORD_BITRATE_SCALE_HALF,
    SETTING_RECORD_BITRATE_SCALE_HIGH,
    SETTING_RECORD_BITRATE_SCALE_MAX,
};
#define BITRATE_BTN_COUNT ((int)(sizeof(bitrate_btn_to_setting) / sizeof(bitrate_btn_to_setting[0])))

static int bitrate_setting_to_btn(setting_record_bitrate_scale_t value) {
    for (int i = 0; i < BITRATE_BTN_COUNT; i++) {
        if (bitrate_btn_to_setting[i] == value)
            return i;
    }
    return 0;
}

static void update_stop_delay_label() {
    char buf[16];

    lv_slider_set_value(slider_group_stop_delay.slider, g_setting.record.stop_delay_seconds, LV_ANIM_OFF);
    if (g_setting.record.stop_delay_seconds == 0)
        snprintf(buf, sizeof(buf), "%s", _lang("Off"));
    else
        snprintf(buf, sizeof(buf), "%ds", g_setting.record.stop_delay_seconds);
    lv_label_set_text(slider_group_stop_delay.label, buf);
}

static void update_visibility() {
    btn_group_enable(&btn_group_file_naming, rtc_has_battery() == 0);

    if (rtc_has_battery() == 0) {
        lv_obj_add_flag(pp_record.p_arr.panel[ROW_NAMING_SCHEME], FLAG_SELECTABLE);
    } else {
        lv_obj_clear_flag(pp_record.p_arr.panel[ROW_NAMING_SCHEME], FLAG_SELECTABLE);
    }
}

static lv_obj_t *page_record_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[256];
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);

    snprintf(buf, sizeof(buf), "%s:", _lang("Record Option"));
    create_text(NULL, section, false, buf, LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *cont = lv_obj_create(section);
    lv_obj_set_size(cont, UI_PAGE_VIEW_SIZE);
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_GRID);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont, &style_context, LV_PART_MAIN);

    lv_obj_set_style_grid_column_dsc_array(cont, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(cont, row_dsc, 0);

    create_select_item(arr, cont);

    create_btn_group_item(&btn_group_record_mode, cont, 2, _lang("Record Mode"), _lang("Auto"), _lang("Manual"), "", "", ROW_RECORD_MODE);
    create_btn_group_item(&btn_group_format, cont, 2, _lang("Record Format"), "MP4", "TS", "", "", ROW_RECORD_FORMAT);
    create_btn_group_item2(&btn_group_bitrate_scale, cont, BITRATE_BTN_COUNT,
                           _lang("Record Bitrate"), _lang("Normal"), "1/4", "1/2", "1.5x", "2x", " ", ROW_RECORD_BITRATE);
    lv_obj_set_grid_cell(arr->panel[ROW_RECORD_BITRATE], LV_GRID_ALIGN_STRETCH, 0, 6,
                         LV_GRID_ALIGN_STRETCH, ROW_RECORD_BITRATE, 2);
    lv_obj_clear_flag(arr->panel[ROW_RECORD_BITRATE_WRAP], FLAG_SELECTABLE);
    create_btn_group_item(&btn_group_record_osd, cont, 2, _lang("Record OSD"), _lang("Yes"), _lang("No"), "", "", ROW_RECORD_OSD);
    create_btn_group_item(&btn_group_file_naming, cont, 3, _lang("Naming Scheme"), _lang("Digits"), _lang("Date"), "ELRS", "", ROW_NAMING_SCHEME);
    create_slider_item(&slider_group_stop_delay, cont, _lang("Auto DVR Stop Delay"), STOP_DELAY_MAX, g_setting.record.stop_delay_seconds, ROW_STOP_DELAY);
    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, ROW_BACK, 1);

    btn_group_set_sel(&btn_group_record_mode, g_setting.record.mode_manual ? 1 : 0);
    btn_group_set_sel(&btn_group_format, g_setting.record.format_ts ? 1 : 0);
    btn_group_set_sel(&btn_group_bitrate_scale, bitrate_setting_to_btn(g_setting.record.bitrate_scale));
    btn_group_set_sel(&btn_group_record_osd, g_setting.record.osd ? 0 : 1);
    btn_group_set_sel(&btn_group_file_naming, g_setting.record.naming);
    update_stop_delay_label();

    update_visibility();

    return page;
}

static void page_record_exit_stop_delay() {
    lv_obj_add_style(slider_group_stop_delay.slider, &style_silder_main, LV_PART_MAIN);
    app_state_push(APP_STATE_SUBMENU);

    if (stop_delay_changed) {
        ini_putl("record", "stop_delay_seconds", g_setting.record.stop_delay_seconds, SETTING_INI);
        stop_delay_changed = false;
    }
    stop_delay_focused = false;
}

static void page_record_exit() {
    if (stop_delay_focused) {
        page_record_exit_stop_delay();
    }
}

static void page_record_on_roller(uint8_t key) {
    int value;

    if (!stop_delay_focused)
        return;

    value = g_setting.record.stop_delay_seconds;
    if (key == DIAL_KEY_UP && value > 0) {
        value -= STOP_DELAY_STEP;
    } else if (key == DIAL_KEY_DOWN && value < STOP_DELAY_MAX) {
        value += STOP_DELAY_STEP;
    } else {
        return;
    }

    g_setting.record.stop_delay_seconds = value;
    update_stop_delay_label();
    stop_delay_changed = true;
}

static void page_record_on_click(uint8_t key, int sel) {
    (void)key;

    if (stop_delay_focused) {
        page_record_exit_stop_delay();
        return;
    }

    if (sel == ROW_RECORD_MODE) {
        btn_group_toggle_sel(&btn_group_record_mode);
        g_setting.record.mode_manual = btn_group_get_sel(&btn_group_record_mode);
        settings_put_bool("record", "mode_manual", g_setting.record.mode_manual);
    } else if (sel == ROW_RECORD_FORMAT) {
        btn_group_toggle_sel(&btn_group_format);
        g_setting.record.format_ts = btn_group_get_sel(&btn_group_format);
        settings_put_bool("record", "format_ts", g_setting.record.format_ts);
    } else if (sel == ROW_RECORD_BITRATE) {
        btn_group_toggle_sel(&btn_group_bitrate_scale);
        g_setting.record.bitrate_scale = bitrate_btn_to_setting[btn_group_get_sel(&btn_group_bitrate_scale)];
        ini_putl("record", "bitrate_scale", g_setting.record.bitrate_scale, SETTING_INI);
    } else if (sel == ROW_RECORD_OSD) {
        btn_group_toggle_sel(&btn_group_record_osd);
        g_setting.record.osd = !btn_group_get_sel(&btn_group_record_osd);
        settings_put_bool("record", "osd", g_setting.record.osd);
    } else if (sel == ROW_NAMING_SCHEME) {
        if (rtc_has_battery() == 0) {
            btn_group_toggle_sel(&btn_group_file_naming);
            g_setting.record.naming = btn_group_get_sel(&btn_group_file_naming);
            ini_putl("record", "naming", g_setting.record.naming, SETTING_INI);
            if (g_setting.record.naming != SETTING_NAMING_ELRS) {
                dvr_clear_race_label();
            }
        }
    } else if (sel == ROW_STOP_DELAY) {
        stop_delay_focused = true;
        stop_delay_changed = false;
        app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
        lv_obj_add_style(slider_group_stop_delay.slider, &style_silder_select, LV_PART_MAIN);
    }
}

page_pack_t pp_record = {
    .p_arr = {
        .cur = 0,
        .max = ROW_BACK + 1,
    },
    .name = "Record Option",
    .create = page_record_create,
    .enter = NULL,
    .exit = page_record_exit,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = page_record_on_roller,
    .on_click = page_record_on_click,
    .on_right_button = NULL,
};
