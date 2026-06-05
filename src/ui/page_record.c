#include "page_record.h"

#include <stdio.h>
#include <stdlib.h>

#include <minIni.h>

#include "../conf/ui.h"

#include "core/app_state.h"
#include "../core/common.hh"
#include "core/dvr.h"
#include "driver/rtc.h"
#include "lang/language.h"
#include "page_common.h"
#include "ui/ui_style.h"

#define AUDIO_VOLUME_MIN 0
#define DVR_AUDIO_VOLUME_MAX 8
#define LIVE_AUDIO_VOLUME_MAX 10

static btn_group_t btn_group_record_mode;
static btn_group_t btn_group_format;
static btn_group_t btn_group_bitrate_scale;
static btn_group_t btn_group_record_osd;
static btn_group_t btn_group_record_audio;
static btn_group_t btn_group_audio_source;
static slider_group_t slider_group_dvr_audio_volume;
static slider_group_t slider_group_live_audio_volume;
static btn_group_t btn_group_file_naming;

enum {
    ROW_RECORD_MODE = 0,
    ROW_RECORD_FORMAT,
    ROW_RECORD_BITRATE,
    ROW_RECORD_OSD,
    ROW_RECORD_AUDIO,
    ROW_AUDIO_SOURCE,
    ROW_DVR_AUDIO_VOLUME,
    ROW_LIVE_AUDIO_VOLUME,
    ROW_NAMING_SCHEME,
    ROW_BACK,

    ROW_COUNT
};

static int selected_audio_volume_row = -1;
static bool selected_audio_volume_changed;

static lv_coord_t col_dsc[] = {UI_RECORD_COLS};
static lv_coord_t row_dsc[] = {UI_RECORD_ROWS};

static void update_visibility() {
    btn_group_enable(&btn_group_audio_source, btn_group_record_audio.current == 0);
    slider_enable(&slider_group_dvr_audio_volume, true);
    slider_enable(&slider_group_live_audio_volume, true);
    lv_obj_add_flag(pp_record.p_arr.panel[ROW_DVR_AUDIO_VOLUME], FLAG_SELECTABLE);
    lv_obj_add_flag(pp_record.p_arr.panel[ROW_LIVE_AUDIO_VOLUME], FLAG_SELECTABLE);

    if (btn_group_record_audio.current == 0) {
        lv_obj_add_flag(pp_record.p_arr.panel[ROW_AUDIO_SOURCE], FLAG_SELECTABLE);
    } else {
        lv_obj_clear_flag(pp_record.p_arr.panel[ROW_AUDIO_SOURCE], FLAG_SELECTABLE);
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
    create_btn_group_item(&btn_group_record_osd, cont, 2, _lang("Record OSD"), _lang("Yes"), _lang("No"), "", "", ROW_RECORD_OSD);
    create_btn_group_item(&btn_group_record_audio, cont, 2, _lang("Record Audio"), _lang("Yes"), _lang("No"), "", "", ROW_RECORD_AUDIO);
    create_btn_group_item(&btn_group_audio_source, cont, 3, _lang("Audio Source"), _lang("Mic"), _lang("Line In"), _lang("A/V In"), "", ROW_AUDIO_SOURCE);
    create_slider_item(&slider_group_dvr_audio_volume, cont, _lang("DVR Volume"), DVR_AUDIO_VOLUME_MAX, g_setting.record.dvr_audio_volume, ROW_DVR_AUDIO_VOLUME);
    lv_obj_set_grid_cell(slider_group_dvr_audio_volume.slider, LV_GRID_ALIGN_STRETCH, 2, 3,
                         LV_GRID_ALIGN_CENTER, ROW_DVR_AUDIO_VOLUME, 1);
    lv_obj_set_grid_cell(slider_group_dvr_audio_volume.label, LV_GRID_ALIGN_START, 5, 1,
                         LV_GRID_ALIGN_CENTER, ROW_DVR_AUDIO_VOLUME, 1);
    create_slider_item(&slider_group_live_audio_volume, cont, _lang("Live Volume"), LIVE_AUDIO_VOLUME_MAX, g_setting.record.live_audio_volume, ROW_LIVE_AUDIO_VOLUME);
    lv_obj_set_grid_cell(slider_group_live_audio_volume.slider, LV_GRID_ALIGN_STRETCH, 2, 3,
                         LV_GRID_ALIGN_CENTER, ROW_LIVE_AUDIO_VOLUME, 1);
    lv_obj_set_grid_cell(slider_group_live_audio_volume.label, LV_GRID_ALIGN_START, 5, 1,
                         LV_GRID_ALIGN_CENTER, ROW_LIVE_AUDIO_VOLUME, 1);
    create_btn_group_item(&btn_group_file_naming, cont, 2, _lang("Naming Scheme"), _lang("Digits"), _lang("Date"), "", "", ROW_NAMING_SCHEME);
    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, ROW_BACK, 1);

    btn_group_set_sel(&btn_group_record_mode, g_setting.record.mode_manual ? 1 : 0);
    btn_group_set_sel(&btn_group_format, g_setting.record.format_ts ? 1 : 0);
    btn_group_set_sel(&btn_group_bitrate_scale, g_setting.record.bitrate_scale);
    btn_group_set_sel(&btn_group_record_osd, g_setting.record.osd ? 0 : 1);
    btn_group_set_sel(&btn_group_record_audio, g_setting.record.audio ? 0 : 1);
    btn_group_set_sel(&btn_group_audio_source, g_setting.record.audio_source);
    update_slider_item_with_value(&slider_group_dvr_audio_volume, g_setting.record.dvr_audio_volume);
    update_slider_item_with_value(&slider_group_live_audio_volume, g_setting.record.live_audio_volume);
    btn_group_set_sel(&btn_group_file_naming, g_setting.record.naming);

    update_visibility();

    return page;
}

static slider_group_t *page_record_selected_audio_slider() {
    if (selected_audio_volume_row == ROW_DVR_AUDIO_VOLUME)
        return &slider_group_dvr_audio_volume;
    if (selected_audio_volume_row == ROW_LIVE_AUDIO_VOLUME)
        return &slider_group_live_audio_volume;
    return NULL;
}

static void page_record_update_audio_volume(int value) {
    slider_group_t *slider_group = page_record_selected_audio_slider();
    int *setting_value = NULL;
    int max_value = LIVE_AUDIO_VOLUME_MAX;

    if (selected_audio_volume_row == ROW_DVR_AUDIO_VOLUME) {
        setting_value = &g_setting.record.dvr_audio_volume;
        max_value = DVR_AUDIO_VOLUME_MAX;
    } else if (selected_audio_volume_row == ROW_LIVE_AUDIO_VOLUME) {
        setting_value = &g_setting.record.live_audio_volume;
    }

    if (slider_group == NULL || setting_value == NULL)
        return;

    if (value < AUDIO_VOLUME_MIN)
        value = AUDIO_VOLUME_MIN;
    else if (value > max_value)
        value = max_value;

    if (*setting_value == value) {
        return;
    }

    *setting_value = value;
    update_slider_item_with_value(slider_group, *setting_value);
    selected_audio_volume_changed = true;
}

static void page_record_exit_audio_volume() {
    slider_group_t *slider_group = page_record_selected_audio_slider();

    if (slider_group != NULL)
        lv_obj_add_style(slider_group->slider, &style_silder_main, LV_PART_MAIN);

    app_state_push(APP_STATE_SUBMENU);

    if (selected_audio_volume_changed) {
        if (selected_audio_volume_row == ROW_DVR_AUDIO_VOLUME) {
            ini_putl("record", "dvr_audio_volume_v2", g_setting.record.dvr_audio_volume, SETTING_INI);
        } else if (selected_audio_volume_row == ROW_LIVE_AUDIO_VOLUME) {
            ini_putl("record", "live_audio_volume", g_setting.record.live_audio_volume, SETTING_INI);
            dvr_set_live_audio_volume(g_setting.record.live_audio_volume);
        }
        selected_audio_volume_changed = false;
    }

    selected_audio_volume_row = -1;
}

static void page_record_exit() {
    if (selected_audio_volume_row != -1) {
        page_record_exit_audio_volume();
    }
}

static void page_record_on_roller(uint8_t key) {
    int32_t value;
    int max_value;

    slider_group_t *slider_group = page_record_selected_audio_slider();

    if (slider_group == NULL) {
        return;
    }

    max_value = selected_audio_volume_row == ROW_DVR_AUDIO_VOLUME ? DVR_AUDIO_VOLUME_MAX : LIVE_AUDIO_VOLUME_MAX;
    value = lv_slider_get_value(slider_group->slider);
    if (key == DIAL_KEY_UP && value > AUDIO_VOLUME_MIN) {
        value--;
    } else if (key == DIAL_KEY_DOWN && value < max_value) {
        value++;
    }

    page_record_update_audio_volume(value);
}

static void page_record_on_click(uint8_t key, int sel) {
    if (selected_audio_volume_row != -1) {
        page_record_exit_audio_volume();
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
        if (g_setting.record.format_ts)
            ini_puts("record", "type", "ts", REC_CONF);
        else
            ini_puts("record", "type", "mp4", REC_CONF);
    } else if (sel == ROW_RECORD_BITRATE) {
        btn_group_toggle_sel(&btn_group_bitrate_scale);
        g_setting.record.bitrate_scale = btn_group_get_sel(&btn_group_bitrate_scale);
        ini_putl("record", "bitrate_scale", g_setting.record.bitrate_scale, SETTING_INI);
    } else if (sel == ROW_RECORD_OSD) {
        btn_group_toggle_sel(&btn_group_record_osd);
        g_setting.record.osd = !btn_group_get_sel(&btn_group_record_osd);
        settings_put_bool("record", "osd", g_setting.record.osd);
    } else if (sel == ROW_RECORD_AUDIO) {
        btn_group_toggle_sel(&btn_group_record_audio);
        g_setting.record.audio = !btn_group_get_sel(&btn_group_record_audio);
        settings_put_bool("record", "audio", g_setting.record.audio);
        update_visibility();
    } else if (sel == ROW_AUDIO_SOURCE) {
        btn_group_toggle_sel(&btn_group_audio_source);
        g_setting.record.audio_source = btn_group_get_sel(&btn_group_audio_source);
        ini_putl("record", "audio_source", g_setting.record.audio_source, SETTING_INI);
    } else if (sel == ROW_DVR_AUDIO_VOLUME || sel == ROW_LIVE_AUDIO_VOLUME) {
        slider_group_t *slider_group;

        selected_audio_volume_row = sel;
        selected_audio_volume_changed = false;
        slider_group = page_record_selected_audio_slider();

        app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
        if (slider_group != NULL)
            lv_obj_add_style(slider_group->slider, &style_silder_select, LV_PART_MAIN);
    } else if (sel == ROW_NAMING_SCHEME) {
        if (rtc_has_battery() == 0) {
            btn_group_toggle_sel(&btn_group_file_naming);
            g_setting.record.naming = btn_group_get_sel(&btn_group_file_naming);
            ini_putl("record", "naming", g_setting.record.naming, SETTING_INI);
        }
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
