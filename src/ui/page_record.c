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
static btn_group_t btn_group_rate_control;
static btn_group_t btn_group_record_osd;
static btn_group_t btn_group_file_naming;
static slider_group_t slider_group_stop_delay;
static slider_group_t slider_group_vbr_quality;
static slider_group_t slider_group_vbr_max_qp;

enum {
    ROW_RECORD_MODE = 0,
    ROW_RECORD_FORMAT,
    ROW_RECORD_BITRATE,
    ROW_RATE_CONTROL,
    ROW_VBR_QUALITY,
    ROW_VBR_MAX_QP,
    ROW_RECORD_OSD,
    ROW_NAMING_SCHEME,
    ROW_STOP_DELAY,
    ROW_BACK,
    ROW_RECORD_NOTE,

    ROW_COUNT
};

static bool stop_delay_focused = false;
static bool stop_delay_changed = false;
static bool vbr_quality_focused = false;
static bool vbr_quality_changed = false;
static bool vbr_max_qp_focused = false;
static bool vbr_max_qp_changed = false;

static lv_coord_t col_dsc[] = {UI_RECORD_COLS};
static lv_coord_t row_dsc[] = {UI_RECORD_ROWS};

static void update_stop_delay_label() {
    char buf[16];

    lv_slider_set_value(slider_group_stop_delay.slider, g_setting.record.stop_delay_seconds, LV_ANIM_OFF);
    if (g_setting.record.stop_delay_seconds == 0)
        snprintf(buf, sizeof(buf), "%s", _lang("Off"));
    else
        snprintf(buf, sizeof(buf), "%ds", g_setting.record.stop_delay_seconds);
    lv_label_set_text(slider_group_stop_delay.label, buf);
}

static void update_vbr_quality_label() {
    char buf[16];
    lv_slider_set_value(slider_group_vbr_quality.slider, g_setting.record.vbr_quality, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%d", g_setting.record.vbr_quality);
    lv_label_set_text(slider_group_vbr_quality.label, buf);
}

static void update_vbr_max_qp_label() {
    char buf[16];
    lv_slider_set_value(slider_group_vbr_max_qp.slider, g_setting.record.vbr_max_qp, LV_ANIM_OFF);
    if (g_setting.record.vbr_max_qp == VBR_MAX_QP_RECOMMENDED)
        snprintf(buf, sizeof(buf), "%d *", g_setting.record.vbr_max_qp);
    else
        snprintf(buf, sizeof(buf), "%d", g_setting.record.vbr_max_qp);
    lv_label_set_text(slider_group_vbr_max_qp.label, buf);
}

static void update_visibility() {
    bool const vbr = (btn_group_rate_control.current == SETTING_RECORD_RC_VBR);
    slider_enable(&slider_group_vbr_quality, vbr);
    slider_enable(&slider_group_vbr_max_qp, vbr);
    if (vbr) {
        lv_obj_add_flag(pp_record.p_arr.panel[ROW_VBR_QUALITY], FLAG_SELECTABLE);
        lv_obj_add_flag(pp_record.p_arr.panel[ROW_VBR_MAX_QP], FLAG_SELECTABLE);
    } else {
        lv_obj_clear_flag(pp_record.p_arr.panel[ROW_VBR_QUALITY], FLAG_SELECTABLE);
        lv_obj_clear_flag(pp_record.p_arr.panel[ROW_VBR_MAX_QP], FLAG_SELECTABLE);
    }

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
    create_btn_group_item(&btn_group_bitrate_scale, cont, 3, _lang("Record Bitrate"), _lang("Normal"), "1/2", "1/4", "", ROW_RECORD_BITRATE);
    create_btn_group_item(&btn_group_rate_control, cont, 2, _lang("Rate Control"), "CBR", "VBR", "", "", ROW_RATE_CONTROL);
    create_slider_item(&slider_group_vbr_quality, cont, _lang("VBR Quality"), VBR_QUALITY_MAX, g_setting.record.vbr_quality, ROW_VBR_QUALITY);
    create_slider_item(&slider_group_vbr_max_qp, cont, _lang("VBR Max QP"), VBR_MAX_QP_MAX, g_setting.record.vbr_max_qp, ROW_VBR_MAX_QP);
    create_btn_group_item(&btn_group_record_osd, cont, 2, _lang("Record OSD"), _lang("Yes"), _lang("No"), "", "", ROW_RECORD_OSD);
    create_btn_group_item(&btn_group_file_naming, cont, 3, _lang("Naming Scheme"), _lang("Digits"), _lang("Date"), "ELRS", "", ROW_NAMING_SCHEME);
    create_slider_item(&slider_group_stop_delay, cont, _lang("Auto DVR Stop Delay"), STOP_DELAY_MAX, g_setting.record.stop_delay_seconds, ROW_STOP_DELAY);
    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, ROW_BACK, 1);

    lv_obj_t *note = lv_label_create(cont);
    snprintf(buf, sizeof(buf), "%s.\n%s.",
             _lang("CBR keeps file size steady but quality varies with scene complexity; VBR treats bitrate as a ceiling for steadier quality and smaller files, with higher write peaks"),
             _lang("VBR Quality uses the encoder scale 0-13; Max QP is the quality floor (lower is better but creates larger files), and 40 is the suggested value"));
    lv_label_set_text(note, buf);
    lv_obj_set_style_text_font(note, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(note, UI_PAGE_TEXT_PAD, 0);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(note, LV_GRID_ALIGN_START, 1, 4,
                         LV_GRID_ALIGN_START, ROW_RECORD_NOTE, 2);
    lv_obj_clear_flag(arr->panel[ROW_RECORD_NOTE], FLAG_SELECTABLE);

    btn_group_set_sel(&btn_group_record_mode, g_setting.record.mode_manual ? 1 : 0);
    btn_group_set_sel(&btn_group_format, g_setting.record.format_ts ? 1 : 0);
    btn_group_set_sel(&btn_group_bitrate_scale, g_setting.record.bitrate_scale);
    btn_group_set_sel(&btn_group_rate_control, g_setting.record.rc_mode);
    update_vbr_quality_label();
    update_vbr_max_qp_label();
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

static void page_record_exit_vbr_quality() {
    lv_obj_add_style(slider_group_vbr_quality.slider, &style_silder_main, LV_PART_MAIN);
    app_state_push(APP_STATE_SUBMENU);

    if (vbr_quality_changed) {
        ini_putl("record", "vbr_quality", g_setting.record.vbr_quality, SETTING_INI);
        vbr_quality_changed = false;
    }
    vbr_quality_focused = false;
}

static void page_record_exit_vbr_max_qp() {
    lv_obj_add_style(slider_group_vbr_max_qp.slider, &style_silder_main, LV_PART_MAIN);
    app_state_push(APP_STATE_SUBMENU);

    if (vbr_max_qp_changed) {
        ini_putl("record", "vbr_max_qp", g_setting.record.vbr_max_qp, SETTING_INI);
        vbr_max_qp_changed = false;
    }
    vbr_max_qp_focused = false;
}

static void page_record_exit() {
    if (stop_delay_focused) {
        page_record_exit_stop_delay();
    }
    if (vbr_quality_focused) {
        page_record_exit_vbr_quality();
    }
    if (vbr_max_qp_focused) {
        page_record_exit_vbr_max_qp();
    }
}

static void page_record_on_roller(uint8_t key) {
    int value;

    if (vbr_max_qp_focused) {
        value = g_setting.record.vbr_max_qp;
        if (key == DIAL_KEY_UP && value > 0) {
            value--;
        } else if (key == DIAL_KEY_DOWN && value < VBR_MAX_QP_MAX) {
            value++;
        } else {
            return;
        }
        g_setting.record.vbr_max_qp = value;
        update_vbr_max_qp_label();
        vbr_max_qp_changed = true;
        return;
    }

    if (vbr_quality_focused) {
        value = g_setting.record.vbr_quality;
        if (key == DIAL_KEY_UP && value > VBR_QUALITY_MIN) {
            value--;
        } else if (key == DIAL_KEY_DOWN && value < VBR_QUALITY_MAX) {
            value++;
        } else {
            return;
        }
        g_setting.record.vbr_quality = value;
        update_vbr_quality_label();
        vbr_quality_changed = true;
        return;
    }

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
    if (vbr_quality_focused) {
        page_record_exit_vbr_quality();
        return;
    }
    if (vbr_max_qp_focused) {
        page_record_exit_vbr_max_qp();
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
        g_setting.record.bitrate_scale = btn_group_get_sel(&btn_group_bitrate_scale);
        ini_putl("record", "bitrate_scale", g_setting.record.bitrate_scale, SETTING_INI);
    } else if (sel == ROW_RATE_CONTROL) {
        btn_group_toggle_sel(&btn_group_rate_control);
        g_setting.record.rc_mode = btn_group_get_sel(&btn_group_rate_control);
        ini_putl("record", "rc_mode", g_setting.record.rc_mode, SETTING_INI);
        update_visibility();
    } else if (sel == ROW_VBR_QUALITY) {
        vbr_quality_focused = true;
        vbr_quality_changed = false;
        app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
        lv_obj_add_style(slider_group_vbr_quality.slider, &style_silder_select, LV_PART_MAIN);
    } else if (sel == ROW_VBR_MAX_QP) {
        vbr_max_qp_focused = true;
        vbr_max_qp_changed = false;
        app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
        lv_obj_add_style(slider_group_vbr_max_qp.slider, &style_silder_select, LV_PART_MAIN);
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
