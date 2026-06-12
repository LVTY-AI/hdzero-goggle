#include "page_autoscan.h"

#include <minIni.h>

#include "../conf/ui.h"

#include "core/settings.h"
#include "lang/language.h"
#include "ui/ui_style.h"

static lv_coord_t col_dsc[] = {UI_AUTOSCAN_COLS};
static lv_coord_t row_dsc[] = {UI_AUTOSCAN_ROWS};

static btn_group_t btn_group0;
static btn_group_t btn_group1;
static btn_group_t btn_group2; // Load from Boot: Yes / No

static lv_obj_t *page_autoscan_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[128];
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);

    snprintf(buf, sizeof(buf), "%s:", _lang("Auto Scan"));
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
    lv_obj_set_grid_cell(pp_autoscan.p_arr.panel[1], LV_GRID_ALIGN_STRETCH, 0, 6,
                         LV_GRID_ALIGN_STRETCH, 1, 2);
    lv_obj_clear_flag(pp_autoscan.p_arr.panel[2], FLAG_SELECTABLE);

    btn_group_t btn_group;
    create_btn_group_item(&btn_group0, cont, 3, _lang("Auto Scan"), _lang("On"), _lang("Last"), _lang("Off"), "", 0);
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    // Matches setting_autoscan_source_t order; "Dual" (= Auto Detect, value 5)
    // is BoxPro/G2 only (built-in analog).
    create_btn_group_item2(&btn_group1, cont, 6, _lang("Default"), _lang("Last"), _lang("HDZero"), _lang("Analog"), _lang("AV In"), _lang("HDMI In"), _lang("Dual"), 1); // 2 rows
#else
    create_btn_group_item2(&btn_group1, cont, 5, _lang("Default"), _lang("Last"), _lang("HDZero"), _lang("Analog"), _lang("AV In"), _lang("HDMI In"), " ", 1); // 2 rows
#endif
    // With Auto Scan=On: Yes = boot straight into the Default source, No =
    // run a Scan Now sweep for its protocol first (HDZero's historical
    // behavior, now offered for Analog and Dual too).
    create_btn_group_item(&btn_group2, cont, 2, _lang("Load from Boot"), _lang("Yes"), _lang("No"), "", "", 3);
    // "Load from Boot" is wider than the label column and bleeds into the
    // column where the option grid normally starts, so the selection arrow of
    // "Yes" would draw on top of the text. Shift both options one column
    // right (under the picker's middle/right columns).
    lv_obj_set_grid_cell(btn_group2.btn_a[0].container, LV_GRID_ALIGN_START, 3, 1,
                         LV_GRID_ALIGN_CENTER, 3, 1);
    lv_obj_set_grid_cell(btn_group2.btn_a[1].container, LV_GRID_ALIGN_START, 4, 1,
                         LV_GRID_ALIGN_CENTER, 3, 1);
    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, 4, 1);

    lv_obj_t *label2 = lv_label_create(cont);
    char note[256];
    snprintf(note, sizeof(note), "%s\n%s",
             _lang("*if Auto Scan is 'Last', goggles will default to show last tuned channel"),
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
             _lang("*'Load from Boot' does not affect AV In or HDMI In; they always load from boot"));
#else
             _lang("*'Load from Boot' does not affect Analog, AV In or HDMI In; they always load from boot"));
#endif
    lv_label_set_text(label2, note);
    lv_obj_set_style_text_font(label2, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_align(label2, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label2, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(label2, UI_PAGE_TEXT_PAD, 0);
    lv_label_set_long_mode(label2, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(label2, LV_GRID_ALIGN_START, 1, 4,
                         LV_GRID_ALIGN_START, 5, 2);

    btn_group_set_sel(&btn_group0, g_setting.autoscan.status);
    btn_group_set_sel(&btn_group1, g_setting.autoscan.source);
    btn_group_set_sel(&btn_group2, g_setting.autoscan.load_from_boot ? 0 : 1);
    return page;
}

static void page_autoscan_on_click(uint8_t key, int sel) {
    if (sel == 0) {
        btn_group_toggle_sel(&btn_group0);
        g_setting.autoscan.status = btn_group_get_sel(&btn_group0);
        ini_putl("autoscan", "status", g_setting.autoscan.status, SETTING_INI);
    } else if (sel < 3) {
        btn_group_toggle_sel(&btn_group1);
        g_setting.autoscan.source = btn_group_get_sel(&btn_group1);
        ini_putl("autoscan", "source", g_setting.autoscan.source, SETTING_INI);
    } else if (sel == 3) {
        btn_group_toggle_sel(&btn_group2);
        g_setting.autoscan.load_from_boot = (btn_group_get_sel(&btn_group2) == 0);
        settings_put_bool("autoscan", "load_from_boot", g_setting.autoscan.load_from_boot);
    }
}

page_pack_t pp_autoscan = {
    .p_arr = {
        .cur = 0,
        .max = 5,
    },
    .name = "Auto Scan",
    .create = page_autoscan_create,
    .enter = NULL,
    .exit = NULL,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = NULL,
    .on_click = page_autoscan_on_click,
    .on_right_button = NULL,
};
