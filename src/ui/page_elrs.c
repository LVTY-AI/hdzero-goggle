#include "page_elrs.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "../conf/ui.h"

#include "core/common.hh"
#include "core/elrs.h"
#include "driver/esp32.h"
#include "lang/language.h"
#include "page_version.h"
#include "ui/ui_style.h"

enum {
    POS_VTX,
    POS_VTX_CTRL,
    POS_AUTO_SEND,
    POS_SENT_OSD,
    POS_SENT_STYLE,
    POS_PWR,
    POS_WIFI,
    POS_BIND,
    POS_BACK,
    POS_MAX
};

static lv_coord_t col_dsc[] = {UI_ELRS_COLS};
static lv_coord_t row_dsc[] = {UI_ELRS_ROWS};
static lv_obj_t *btn_wifi;
static lv_obj_t *label_wifi_status;
static lv_obj_t *btn_bind;
static lv_obj_t *label_bind_status;
static lv_obj_t *cancel_label;
static lv_obj_t *btn_vtx_send;
static btn_group_t vtx_ctrl_group;
static btn_group_t auto_send_group;
static btn_group_t sent_osd_group;
static btn_group_t sent_style_group;
static lv_obj_t *sent_preview;
static lv_obj_t *sent_explain;
static btn_group_t elrs_group;
static bool binding = false;

static void update_sent_preview() {
    if (!sent_preview || !sent_explain)
        return;

    const bool subtle = sent_style_group.current == SETTING_VTX_SENT_STYLE_SUBTLE;
    if (subtle) {
        lv_label_set_text(sent_preview, "VTX SENT");
        lv_obj_set_style_bg_opa(sent_preview, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(sent_preview, lv_color_make(0x00, 0xB0, 0x00), 0);
    } else {
        lv_label_set_text(sent_preview, "  R5  VTX SENT  ");
        lv_obj_set_style_bg_opa(sent_preview, LV_OPA_100, 0);
        lv_obj_set_style_text_color(sent_preview, lv_color_make(0x00, 0xFF, 0x00), 0);
    }

    const char *when;
    switch (sent_osd_group.current) {
    case SETTING_VTX_SENT_OSD_DELIBERATE:
        when = _lang("Shown only after a dial long press or an explicit Send VTX");
        break;
    case SETTING_VTX_SENT_OSD_OFF:
        when = _lang("Never shown");
        break;
    default:
        when = _lang("Shown after every send");
        break;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "%s.\n%s.", when,
             subtle ? _lang("Subtle: label only, dimmer, no background box, and it clears faster")
                    : _lang("Normal: channel and label in bright green on a black box"));
    lv_label_set_text(sent_explain, buf);
}

static void update_visibility() {
    const bool backpackIsActive = elrs_group.current == 0;
    const bool vtxSendAllowed = backpackIsActive && g_setting.elrs.vtx_send_enable;

    btn_group_enable(&vtx_ctrl_group, backpackIsActive);
    btn_group_enable(&auto_send_group, vtxSendAllowed);
    btn_group_enable(&sent_osd_group, vtxSendAllowed);
    btn_group_enable(&sent_style_group, vtxSendAllowed);

    if (vtxSendAllowed) {
        lv_obj_clear_state(btn_vtx_send, STATE_DISABLED);
        lv_obj_add_flag(pp_elrs.p_arr.panel[POS_VTX], FLAG_SELECTABLE);
        lv_obj_add_flag(pp_elrs.p_arr.panel[POS_AUTO_SEND], FLAG_SELECTABLE);
        lv_obj_add_flag(pp_elrs.p_arr.panel[POS_SENT_OSD], FLAG_SELECTABLE);
        lv_obj_add_flag(pp_elrs.p_arr.panel[POS_SENT_STYLE], FLAG_SELECTABLE);
    } else {
        lv_obj_add_state(btn_vtx_send, STATE_DISABLED);
        lv_obj_clear_flag(pp_elrs.p_arr.panel[POS_VTX], FLAG_SELECTABLE);
        lv_obj_clear_flag(pp_elrs.p_arr.panel[POS_AUTO_SEND], FLAG_SELECTABLE);
        lv_obj_clear_flag(pp_elrs.p_arr.panel[POS_SENT_OSD], FLAG_SELECTABLE);
        lv_obj_clear_flag(pp_elrs.p_arr.panel[POS_SENT_STYLE], FLAG_SELECTABLE);
    }

    if (backpackIsActive) {
        lv_obj_clear_state(btn_wifi, STATE_DISABLED);
        lv_obj_clear_state(label_wifi_status, STATE_DISABLED);
        lv_obj_clear_state(btn_bind, STATE_DISABLED);
        lv_obj_clear_state(label_bind_status, STATE_DISABLED);

        lv_obj_add_flag(pp_elrs.p_arr.panel[POS_VTX_CTRL], FLAG_SELECTABLE);
        lv_obj_add_flag(pp_elrs.p_arr.panel[POS_WIFI], FLAG_SELECTABLE);
        lv_obj_add_flag(pp_elrs.p_arr.panel[POS_BIND], FLAG_SELECTABLE);
    } else {
        lv_obj_add_state(btn_wifi, STATE_DISABLED);
        lv_obj_add_state(label_wifi_status, STATE_DISABLED);
        lv_obj_add_state(btn_bind, STATE_DISABLED);
        lv_obj_add_state(label_bind_status, STATE_DISABLED);

        lv_obj_clear_flag(pp_elrs.p_arr.panel[POS_VTX_CTRL], FLAG_SELECTABLE);
        lv_obj_clear_flag(pp_elrs.p_arr.panel[POS_WIFI], FLAG_SELECTABLE);
        lv_obj_clear_flag(pp_elrs.p_arr.panel[POS_BIND], FLAG_SELECTABLE);
    }
}

static lv_obj_t *page_elrs_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[128];
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);

    create_text(NULL, section, false, "ELRS:", LV_MENU_ITEM_BUILDER_VARIANT_2);

    lv_obj_t *cont = lv_obj_create(section);
    lv_obj_set_size(cont, UI_PAGE_VIEW_SIZE);
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_GRID);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont, &style_context, LV_PART_MAIN);

    lv_obj_set_style_grid_column_dsc_array(cont, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(cont, row_dsc, 0);

    create_select_item(arr, cont);

    create_btn_group_item(&elrs_group, cont, 2, _lang("Backpack"), _lang("On"), _lang("Off"), "", "", POS_PWR);
    btn_group_set_sel(&elrs_group, !g_setting.elrs.enable);
    snprintf(buf, sizeof(buf), "%s VTX", _lang("Send"));
    btn_vtx_send = create_label_item(cont, buf, 1, POS_VTX, 1);
    snprintf(buf, sizeof(buf), "VTX %s", _lang("Control"));
    create_btn_group_item(&vtx_ctrl_group, cont, 2, buf, _lang("On"), _lang("Off"), "", "", POS_VTX_CTRL);
    btn_group_set_sel(&vtx_ctrl_group, !g_setting.elrs.vtx_send_enable);
    snprintf(buf, sizeof(buf), "%s VTX", _lang("Auto Send"));
    create_btn_group_item(&auto_send_group, cont, 2, buf, _lang("On"), _lang("Off"), "", "", POS_AUTO_SEND);
    btn_group_set_sel(&auto_send_group, !g_setting.elrs.auto_send_vtx);
    snprintf(buf, sizeof(buf), "%s OSD", _lang("VTX Sent"));
    create_btn_group_item(&sent_osd_group, cont, 3, buf, _lang("On"), _lang("Long press"), _lang("Off"), "", POS_SENT_OSD);
    btn_group_set_sel(&sent_osd_group, g_setting.elrs.vtx_sent_osd);
    snprintf(buf, sizeof(buf), "%s %s", _lang("VTX Sent"), _lang("Style"));
    create_btn_group_item(&sent_style_group, cont, 2, buf, _lang("Normal"), _lang("Subtle"), "", "", POS_SENT_STYLE);
    btn_group_set_sel(&sent_style_group, g_setting.elrs.vtx_sent_style);
    btn_wifi = create_label_item(cont, "WiFi", 1, POS_WIFI, 1);
    label_wifi_status = create_label_item(cont, _lang("Click to start"), 2, POS_WIFI, 1);
    btn_bind = create_label_item(cont, _lang("Bind"), 1, POS_BIND, 1);
    label_bind_status = create_label_item(cont, _lang("Click to start"), 2, POS_BIND, 1);
    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, POS_BACK, 1);

    sent_preview = lv_label_create(cont);
    lv_obj_set_style_bg_color(sent_preview, lv_color_hex(0x010101), LV_PART_MAIN);
    lv_obj_set_style_radius(sent_preview, 50, 0);
    lv_obj_set_style_pad_all(sent_preview, 4, 0);
    lv_obj_set_style_text_font(sent_preview, UI_PAGE_TEXT_FONT, 0);
    lv_obj_set_grid_cell(sent_preview, LV_GRID_ALIGN_START, 1, 3, LV_GRID_ALIGN_CENTER, POS_MAX, 1);

    sent_explain = lv_label_create(cont);
    lv_obj_set_width(sent_explain, LV_PCT(90));
    lv_obj_set_style_text_font(sent_explain, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_color(sent_explain, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(sent_explain, UI_PAGE_TEXT_PAD, 0);
    lv_label_set_long_mode(sent_explain, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(sent_explain, LV_GRID_ALIGN_START, 1, 4, LV_GRID_ALIGN_START, POS_MAX + 1, 2);

    cancel_label = lv_label_create(cont);
    lv_obj_add_flag(cancel_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(cancel_label, _lang("* Press right button to cancel binding"));
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(cancel_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(cancel_label, UI_PAGE_TEXT_PAD, 0);
    lv_label_set_long_mode(cancel_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(cancel_label, LV_GRID_ALIGN_START, 1, 3, LV_GRID_ALIGN_START, POS_MAX + 3, 2);

    update_sent_preview();
    update_visibility();

    return page;
}

static void page_elrs_reset() {
    lv_label_set_text(label_wifi_status, _lang("Click to start"));
    lv_label_set_text(label_bind_status, _lang("Click to start"));
}

static void page_elrs_on_roller(uint8_t key) {
    page_elrs_reset();
}

static void elrs_status_timer(struct _lv_timer_t *timer) {
    char label[80];
    uint8_t status[7] = {0};
    uint16_t size = sizeof(status);

    if (!msp_read_resposne(MSP_GET_BP_STATUS, &size, status)) {
        msp_send_packet(MSP_GET_BP_STATUS, MSP_PACKET_COMMAND, 0, NULL);
        return;
    }
    lv_timer_del(timer);
    if (status[0] & 4) {
        snprintf(label, sizeof(label), "UID: %d,%d,%d,%d,%d,%d", status[1], status[2], status[3], status[4], status[5], status[6]);
        lv_label_set_text(label_bind_status, label);
    }
}

static void request_uid() {
    g_elrs_msp_busy_until = time(NULL) + 6; // covers the ~5s status poll burst below
    msp_send_packet(MSP_GET_BP_STATUS, MSP_PACKET_COMMAND, 0, NULL);
    lv_timer_t *timer = lv_timer_create(elrs_status_timer, 250, NULL);
    lv_timer_set_repeat_count(timer, 20);
}

static void elrs_enable_timer(struct _lv_timer_t *timer) {
    if (g_setting.elrs.enable)
        enable_esp32();
    lv_timer_del(timer);
    request_uid();
}

static void page_elrs_enter() {
    lv_label_set_text(label_wifi_status, _lang("Click to start"));
    lv_label_set_text(label_bind_status, _lang("Click to start"));
    if (elrs_group.current == 0) {
        request_uid();
    } else {
        pp_elrs.p_arr.cur = POS_PWR;
    }
}

static void page_elrs_on_click(uint8_t key, int sel) {
    char buf[128];
    page_elrs_reset();
    if (sel == POS_PWR) {
        btn_group_toggle_sel(&elrs_group);
        g_setting.elrs.enable = btn_group_get_sel(&elrs_group) == 0;
        settings_put_bool("elrs", "enable", g_setting.elrs.enable);
        if (g_setting.elrs.enable)
            enable_esp32();
        else
            disable_esp32();

        update_visibility();
    } else if (sel == POS_VTX) // Send VTX freq
    {
        if (msp_channel_update())
            vtx_sent_osd_show(true);
    } else if (sel == POS_VTX_CTRL) // master switch: may VTX commands be sent at all
    {
        btn_group_toggle_sel(&vtx_ctrl_group);
        g_setting.elrs.vtx_send_enable = btn_group_get_sel(&vtx_ctrl_group) == 0;
        settings_put_bool("elrs", "vtx_send_enable", g_setting.elrs.vtx_send_enable);
        update_visibility();
    } else if (sel == POS_AUTO_SEND) // dial click sends too, not just a long press
    {
        btn_group_toggle_sel(&auto_send_group);
        g_setting.elrs.auto_send_vtx = btn_group_get_sel(&auto_send_group) == 0;
        settings_put_bool("elrs", "auto_send_vtx", g_setting.elrs.auto_send_vtx);
    } else if (sel == POS_SENT_OSD) // when to show the VTX SENT banner
    {
        btn_group_toggle_sel(&sent_osd_group);
        g_setting.elrs.vtx_sent_osd = btn_group_get_sel(&sent_osd_group);
        ini_putl("elrs", "vtx_sent_osd", g_setting.elrs.vtx_sent_osd, SETTING_INI);
        update_sent_preview();
    } else if (sel == POS_SENT_STYLE) // how the banner looks
    {
        btn_group_toggle_sel(&sent_style_group);
        g_setting.elrs.vtx_sent_style = btn_group_get_sel(&sent_style_group);
        ini_putl("elrs", "vtx_sent_style", g_setting.elrs.vtx_sent_style, SETTING_INI);
        update_sent_preview();
    } else if (sel == POS_WIFI) // start ESP Wifi
    {
        snprintf(buf, sizeof(buf), "%s...", _lang("Starting"));
        lv_label_set_text(label_wifi_status, buf);
        g_elrs_msp_busy_until = time(NULL) + 2;
        msp_send_packet(MSP_SET_MODE, MSP_PACKET_COMMAND, 1, (uint8_t *)"W");
        lv_timer_handler();
        if (msp_await_resposne(MSP_SET_MODE, 1, (uint8_t *)"P", 1000) != AWAIT_SUCCESS) {
            snprintf(buf, sizeof(buf), "#FF0000 %s#", _lang("FAILED"));
            lv_label_set_text(label_wifi_status, buf);
        } else {
            snprintf(buf, sizeof(buf), "#00FF00 %s#", _lang("Success"));
            lv_label_set_text(label_wifi_status, buf);
        }
    } else if (sel == POS_BIND) // start ESP bind
    {
        snprintf(buf, sizeof(buf), "%s...", _lang("Starting"));
        lv_label_set_text(label_bind_status, buf);
        g_elrs_msp_busy_until = time(NULL) + 125; // covers the start ack + up to 120s bind wait
        msp_send_packet(MSP_SET_MODE, MSP_PACKET_COMMAND, 1, (uint8_t *)"B");
        lv_timer_handler();
        if (msp_await_resposne(MSP_SET_MODE, 1, (uint8_t *)"P", 1000) != AWAIT_SUCCESS) {
            snprintf(buf, sizeof(buf), "#FF0000 %s#", _lang("FAILED"));
            lv_label_set_text(label_bind_status, buf);
        } else {
            snprintf(buf, sizeof(buf), "%s...", _lang("Binding"));
            lv_label_set_text(label_bind_status, buf);
            lv_obj_clear_flag(cancel_label, LV_OBJ_FLAG_HIDDEN);
            binding = true;
            lv_timer_handler();
            pthread_mutex_unlock(&lvgl_mutex);
            mspAwaitResposne_e response = msp_await_resposne(MSP_SET_MODE, 1, (uint8_t *)"O", 120000);
            pthread_mutex_lock(&lvgl_mutex);
            switch (response) {
            case AWAIT_SUCCESS:
                snprintf(buf, sizeof(buf), "#00FF00 %s#", _lang("Success"));
                lv_label_set_text(label_bind_status, buf);
                request_uid();
                break;
            case AWAIT_TIMEDOUT:
                snprintf(buf, sizeof(buf), "#FEBE00 %s#", _lang("Timeout"));
                lv_label_set_text(label_bind_status, buf);
                break;
            case AWAIT_FAILED:
                snprintf(buf, sizeof(buf), "#FF0000 %s#", _lang("FAILED"));
                lv_label_set_text(label_bind_status, buf);
                break;
            case AWAIT_CANCELLED:
                snprintf(buf, sizeof(buf), "#FEBE00 %s#", _lang("Cancelled"));
                lv_label_set_text(label_bind_status, buf);
                // repower the module and re-request binding info
                disable_esp32();
                if (g_setting.elrs.enable) {
                    lv_timer_create(elrs_enable_timer, 100, NULL);
                }
                break;
            }
            lv_obj_add_flag(cancel_label, LV_OBJ_FLAG_HIDDEN);
            binding = false;
        }
    }
}

static void page_elrs_on_rbtn(bool is_short) {
    if (binding && is_short) {
        msp_cancel_await();
    }
}

page_pack_t pp_elrs = {
    .p_arr = {
        .cur = 0,
        .max = POS_MAX,
    },
    .name = "ELRS",
    .create = page_elrs_create,
    .enter = page_elrs_enter,
    .exit = NULL,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = page_elrs_on_roller,
    .on_click = page_elrs_on_click,
    .on_right_button = page_elrs_on_rbtn,
};
