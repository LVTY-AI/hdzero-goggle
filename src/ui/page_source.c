#include "page_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <minIni.h>

#include "../conf/ui.h"

#include "core/app_state.h"
#include "core/common.hh"
#include "core/dvr.h"
#include "core/osd.h"
#include "core/scan_core.h"
#include "core/settings.h"
#include "driver/beep.h"
#include "driver/hardware.h"
#include "driver/it66121.h"
#include "driver/rtc6715.h"
#include "driver/screen.h"
#include "lang/language.h"
#include "ui/page_common.h"
#include "ui/page_scannow.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_porting.h"
#include "ui/ui_style.h"
#include <log/log.h>

enum {

    ROW_GOGGLE_ANALOG_MODULE = -1,
    ROW_GOGGLE_HDZERO = 0,
    ROW_GOGGLE_ANALOG,
    ROW_GOGGLE_HDMI,
    ROW_GOGGLE_AV,
    ROW_GOGGLE_HDZ_WIDTH,
    ROW_GOGGLE_ANALOG_VIDEO,
    ROW_GOGGLE_ANALOG_RATIO,
    ROW_GOGGLE_TEST_PATTERN,
    ROW_GOGGLE_BACK,
    ROW_GOGGLE_COUNT
};

enum {

    ROW_BOXPRO_ANALOG_MODULE = -2,
    ROW_BOXPRO_ANALOG_VIDEO = -1,
    // Auto Detect is the headline source -- list it first, above HDZero.
    ROW_BOXPRO_AUTO_DETECT = 0,
    ROW_BOXPRO_HDZERO,
    ROW_BOXPRO_ANALOG,
    ROW_BOXPRO_HDMI,
    ROW_BOXPRO_AV,
    // No HDZ Band row: Race/Low is no longer a user toggle on BoxPro. The
    // band is derived automatically from the channel you tune (scan result,
    // Auto Detect dial, or crossover), so Lowband is just part of the flat
    // channel set.
    ROW_BOXPRO_HDZ_WIDTH,
    ROW_BOXPRO_ANALOG_RATIO,
    ROW_BOXPRO_TEST_PATTERN,
    ROW_BOXPRO_BACK,
    ROW_BOXPRO_COUNT
};

enum {
    // Auto Detect is the headline source -- list it first, above HDZero.
    ROW_GOGGLE2_AUTO_DETECT = 0,
    ROW_GOGGLE2_HDZERO,
    ROW_GOGGLE2_ANALOG,
    ROW_GOGGLE2_HDMI,
    ROW_GOGGLE2_AV,
    ROW_GOGGLE2_HDZ_WIDTH,
    ROW_GOGGLE2_ANALOG_MODULE,
    ROW_GOGGLE2_ANALOG_RATIO,
    ROW_GOGGLE2_TEST_PATTERN,
    ROW_GOGGLE2_BACK,
    ROW_GOGGLE2_COUNT
};

#if defined(HDZGOGGLE)
#define ROW_HDZERO       ROW_GOGGLE_HDZERO
#define ROW_ANALOG       ROW_GOGGLE_ANALOG
#define ROW_HDMI         ROW_GOGGLE_HDMI
#define ROW_AV           ROW_GOGGLE_AV
#define ROW_HDZ_WIDTH    ROW_GOGGLE_HDZ_WIDTH
#define ROW_ANALOG_VIDEO ROW_GOGGLE_ANALOG_VIDEO
#define ROW_ANALOG_RATIO ROW_GOGGLE_ANALOG_RATIO
#define ROW_TEST_PATTERN ROW_GOGGLE_TEST_PATTERN
#define ROW_BACK         ROW_GOGGLE_BACK
#define ROW_COUNT        ROW_GOGGLE_COUNT
#elif defined(HDZBOXPRO)
#define ROW_HDZERO       ROW_BOXPRO_HDZERO
#define ROW_ANALOG       ROW_BOXPRO_ANALOG
#define ROW_HDMI         ROW_BOXPRO_HDMI
#define ROW_AV           ROW_BOXPRO_AV
#define ROW_HDZ_WIDTH    ROW_BOXPRO_HDZ_WIDTH
#define ROW_ANALOG_RATIO ROW_BOXPRO_ANALOG_RATIO
#define ROW_AUTO_DETECT  ROW_BOXPRO_AUTO_DETECT
#define ROW_TEST_PATTERN ROW_BOXPRO_TEST_PATTERN
#define ROW_BACK         ROW_BOXPRO_BACK
#define ROW_COUNT        ROW_BOXPRO_COUNT
#elif defined(HDZGOGGLE2)
#define ROW_HDZERO        ROW_GOGGLE2_HDZERO
#define ROW_ANALOG        ROW_GOGGLE2_ANALOG
#define ROW_HDMI          ROW_GOGGLE2_HDMI
#define ROW_AV            ROW_GOGGLE2_AV
#define ROW_AUTO_DETECT   ROW_GOGGLE2_AUTO_DETECT
#define ROW_HDZ_WIDTH     ROW_GOGGLE2_HDZ_WIDTH
#define ROW_ANALOG_MODULE ROW_GOGGLE2_ANALOG_MODULE
#define ROW_ANALOG_RATIO  ROW_GOGGLE2_ANALOG_RATIO
#define ROW_TEST_PATTERN  ROW_GOGGLE2_TEST_PATTERN
#define ROW_BACK          ROW_GOGGLE2_BACK
#define ROW_COUNT         ROW_GOGGLE2_COUNT
#endif

// local
static lv_coord_t col_dsc[] = {UI_SOURCE_COLS};
static lv_coord_t row_dsc[] = {UI_SOURCE_ROWS};

static lv_obj_t *label[6] = {NULL};
static uint8_t oled_tst_mode = 0; // 0=Normal, 1=CB, 2=Grid, 3=All Black, 4=All White, 5=Boot logo
static bool in_sourcepage = false;
static btn_group_t btn_group0, btn_group2, btn_group3;
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
static lv_obj_t *auto_detect_label = NULL;
#endif

static lv_obj_t *page_source_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[128];

    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);

    snprintf(buf, sizeof(buf), "%s:", _lang("Source"));
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

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    // Created first so creation order matches the grid row order (Auto Detect
    // is row 0, the top of the menu).
    auto_detect_label = create_label_item(cont, _lang("Auto Detect"),
                                          1, ROW_AUTO_DETECT, 3);
#endif

    label[0] = create_label_item(cont, "HDZero", 1, ROW_HDZERO, 3);
    snprintf(buf, sizeof(buf), "%s", _lang("Analog"));
    label[1] = create_label_item(cont, buf, 1, ROW_ANALOG, 3);
    snprintf(buf, sizeof(buf), "HDMI %s", _lang("In"));
    label[2] = create_label_item(cont, buf, 1, ROW_HDMI, 3);
    snprintf(buf, sizeof(buf), "AV %s", _lang("In"));
    label[3] = create_label_item(cont, buf, 1, ROW_AV, 3);

    // Auto: Scan, Auto Detect, and the live receiver sweep both bandwidths
    // (slower) so a VTX is found/held regardless of its bandwidth.
    create_btn_group_item(&btn_group2, cont, 3, _lang("HDZero BW"), _lang("Wide"), _lang("Narrow"), _lang("Auto"), "", ROW_HDZ_WIDTH);
    btn_group_set_sel(&btn_group2, g_setting.source.hdzero_bw);

#if defined(HDZGOGGLE)
    create_btn_group_item(&btn_group0, cont, 3, _lang("Analog Video"), "NTSC", "PAL", _lang("Auto"), "", ROW_ANALOG_VIDEO);
    btn_group_set_sel(&btn_group0, g_setting.source.analog_auto ? 2 : g_setting.source.analog_format);
#elif defined(HDZGOGGLE2)
    create_btn_group_item(&btn_group0, cont, 2, _lang("Analog Module"), _lang("Built-in"), _lang("Expansion"), "", "", ROW_ANALOG_MODULE);
    btn_group_set_sel(&btn_group0, g_setting.source.analog_module);
#endif

    create_btn_group_item(&btn_group3, cont, 2, _lang("Analog Ratio"), _lang("4:3"), _lang("16:9"), "", "", ROW_ANALOG_RATIO);
    btn_group_set_sel(&btn_group3, g_setting.source.analog_ratio);

    if (g_setting.storage.selftest) {
        label[4] = create_label_item(cont, "Display Pattern: Normal", 1, ROW_TEST_PATTERN, 3);
    }

    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, ROW_BACK - 1 + g_setting.storage.selftest, 3);
    pp_source.p_arr.max = ROW_COUNT - 1 + g_setting.storage.selftest;

#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    label[5] = create_label_item(cont, _lang("Analog input requires Expansion Module"), 1, ROW_COUNT, 3);
    lv_obj_set_style_text_font(label[5], UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_pad_top(label[5], UI_PAGE_TEXT_PAD, 0);
#endif
    LOGI("pp_source.p_arr.max: %d", pp_source.p_arr.max);
    return page;
}

char *state2string(uint8_t status) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "#%s %s#", status ? "00FF00" : "C0C0C0", status ? _lang("Signal detected") : _lang("No signal"));
    return buf;
}

void source_status_timer() {
    char buf[64];
    int ch;

    if (!in_sourcepage)
        return;

    ch = g_setting.scan.channel & 0x7F;
    if (g_setting.source.hdzero_band == SETTING_SOURCES_HDZERO_BAND_RACEBAND) {
        if (ch <= 8) {
            snprintf(buf, sizeof(buf), "HDZero: R%d", ch);
        } else if (ch <= 12) {
            snprintf(buf, sizeof(buf), "HDZero: F%d", (ch - 8) * 2);
        } else {
            g_setting.scan.channel = 1;
            snprintf(buf, sizeof(buf), "HDZero: R1");
        }
    } else {
        if (ch > 8) {
            g_setting.scan.channel = 1;
        }
        snprintf(buf, sizeof(buf), "HDZero: L%d", ch);
    }
    lv_label_set_text(label[0], buf);

#if defined(HDZGOGGLE)
    snprintf(buf, sizeof(buf), "%s: %s", _lang("Analog"), state2string(g_source_info.av_bay_status));
#elif defined(HDZBOXPRO)
    snprintf(buf, sizeof(buf), "%s: %s", _lang("Analog"), channel2str(0, 0, g_setting.source.analog_channel));
#elif defined(HDZGOGGLE2)
    if (g_setting.source.analog_module == SETTING_SOURCES_ANALOG_MODULE_INTERNAL) {
        snprintf(buf, sizeof(buf), "%s: %s", _lang("Analog"), channel2str(0, 0, g_setting.source.analog_channel));
    } else {
        snprintf(buf, sizeof(buf), "%s: %s", _lang("Analog"), _lang("Expansion Module"));
    }
#endif
    lv_label_set_text(label[1], buf);

    snprintf(buf, sizeof(buf), "HDMI %s: %s", _lang("In"), state2string(g_source_info.hdmi_in_status));
    lv_label_set_text(label[2], buf);

    snprintf(buf, sizeof(buf), "AV %s: %s", _lang("In"), state2string(g_source_info.av_in_status));
    lv_label_set_text(label[3], buf);

    if (g_setting.storage.selftest && label[3]) {
        uint8_t oled_tm = oled_tst_mode & 0x0F;
        char *pattern_label[6] = {"Normal", "Color Bar", "Grid", "All Black", "All White", "Boot logo"};
        char str[32];
        snprintf(str, sizeof(buf), "Display Pattern: %s", pattern_label[oled_tm]);
        lv_label_set_text(label[4], str);
    }
}

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
// Picking a specific source explicitly disables auto-detect; without this,
// the next dial click in video would silently switch protocols again.
static void disable_auto_protocol_detect(void) {
    if (g_setting.source.auto_protocol_detect) {
        g_setting.source.auto_protocol_detect = false;
        settings_put_bool("source", "auto_protocol_detect", false);
    }
}
#endif

static void page_source_select_hdzero() {
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    disable_auto_protocol_detect();
#endif
    progress_bar.start = 1;
    // BW=Auto opens at the last-detected bandwidth -- as fast as Wide/Narrow,
    // no entry sweep. If the VTX is actually on the other bandwidth, the live
    // re-acquire watchdog (scan_core_hdz_bw_tick) corrects it within a couple
    // seconds, blanking the screen while it searches. (The old entry sweep ran
    // two full HDZero_open re-inits up front, ~2-3x slower, for no benefit now
    // that the watchdog handles detection.)
    app_switch_to_hdzero(true);
    app_state_push(APP_STATE_VIDEO);
    g_source_info.source = SOURCE_HDZERO;
    dvr_select_audio_source(g_setting.record.audio_source);
    dvr_enable_line_out(true);
}

static void page_source_select_hdmi() {
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    disable_auto_protocol_detect();
#endif
    if (g_source_info.hdmi_in_status)
        app_switch_to_hdmi_in();
}

static void page_source_select_av_in() {
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    disable_auto_protocol_detect();
#endif
    app_switch_to_analog(1);
    app_state_push(APP_STATE_VIDEO);
    g_source_info.source = SOURCE_AV_IN;
    dvr_select_audio_source(g_setting.record.audio_source);
    dvr_enable_line_out(true);
}

static void page_source_select_analog() {
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    disable_auto_protocol_detect();
#endif
    app_switch_to_analog(0);
    app_state_push(APP_STATE_VIDEO);
    g_source_info.source = SOURCE_AV_MODULE;
    dvr_select_audio_source(g_setting.record.audio_source);
    dvr_enable_line_out(true);
}

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
// Probes the user's current channel on both protocols and enters video on
// whichever has signal. Defaults to HDZ if neither responds. After this
// runs, auto_protocol_detect is on, so the next dial click in video will
// probe both protocols at the next freq-table entry.
static void page_source_select_auto_detect() {
    g_setting.source.auto_protocol_detect = true;
    settings_put_bool("source", "auto_protocol_detect", true);

    // Show the loading bar immediately so the user gets the same feedback
    // they'd see when picking HDZero directly. Flush the LV timer so the bar
    // is actually rendered before we enter the blocking probe. Auto Detect
    // entry (probe + protocol/bandwidth settle) takes longer than a direct
    // HDZero pick, so fill at half rate (~3s longer) to track the operation
    // instead of maxing out and sitting full.
    progress_bar.step = 2;
    progress_bar.start = 1;
    lv_timer_handler();

    // Power both radios for the probe. Use the resolved bandwidth (never the
    // "Both" sentinel); the probe below reads HDZ at this open bandwidth.
    HDZero_open(hdzero_effective_bw());
    rtc6715.init(1, 0);
    scan_core_notify_analog_powered_on();

    // Find the freq-table entry matching the active source's current channel.
    // The previous version probed analog at g_setting.source.analog_channel
    // — an unrelated stored value — so an analog VTX at the user's HDZ
    // frequency was never detected because the probe was tuned elsewhere.
    int8_t band = (int8_t)g_setting.source.hdzero_band;
    int8_t hdz_ch = (int8_t)((g_setting.scan.channel - 1) & 0x7F);
    int8_t analog_ch = (int8_t)((int)g_setting.source.analog_channel - 1);
    const scan_freq_entry_t *entry = NULL;
    bool source_is_analog = (g_source_info.source == SOURCE_AV_MODULE);
    for (size_t i = 0; i < scan_freq_table_len; i++) {
        if (source_is_analog) {
            if (scan_freq_table[i].analog_channel == analog_ch) {
                entry = &scan_freq_table[i];
                break;
            }
        } else {
            if (scan_freq_table[i].hdz_band == band &&
                scan_freq_table[i].hdz_channel == hdz_ch) {
                entry = &scan_freq_table[i];
                break;
            }
        }
    }

    // Probe the current channel at the already-open bandwidth only -- no
    // Wide+Narrow sweep. Each bandwidth change re-inits the DM6302 (~2s), and
    // sweeping both here (on top of the open above and the switch below) made
    // Auto Detect entry take ~12s longer than the loading bar. If the VTX is on
    // the other bandwidth, the live bw-reacquire watchdog corrects it within a
    // couple seconds after entering video.
    scan_result_t r = { PROTOCOL_NONE, 0, 0, 0, 0 };
    if (entry) r = scan_probe_both(entry);

    if (r.protocol == PROTOCOL_ANALOG) {
        // If we crossed protocols, persist the analog channel that's at the
        // current frequency so the analog source switches to it directly.
        if (entry && entry->analog_channel >= 0) {
            uint8_t new_ch = (uint8_t)entry->analog_channel + 1;
            if (g_setting.source.analog_channel != new_ch) {
                g_setting.source.analog_channel = new_ch;
                ini_putl("source", "analog_channel",
                         g_setting.source.analog_channel, SETTING_INI);
            }
        }
        app_switch_to_analog(0);
        app_state_push(APP_STATE_VIDEO);
        g_source_info.source = SOURCE_AV_MODULE;
    } else {
        // PROTOCOL_HDZ or PROTOCOL_NONE: default to HDZ. HDZero_open
        // already ran; app_switch_to_hdzero(true) tunes to current channel.
        // If we crossed protocols, persist the HDZ channel matching the
        // active analog frequency.
        if (r.protocol == PROTOCOL_HDZ && entry &&
            entry->hdz_channel >= 0 && source_is_analog) {
            uint8_t new_ch = (uint8_t)entry->hdz_channel + 1;
            if (g_setting.scan.channel != new_ch) {
                g_setting.scan.channel = new_ch;
                ini_putl("scan", "channel",
                         g_setting.scan.channel, SETTING_INI);
            }
        }
        app_switch_to_hdzero(true);
        app_state_push(APP_STATE_VIDEO);
        g_source_info.source = SOURCE_HDZERO;
    }
    dvr_select_audio_source(g_setting.record.audio_source);
    dvr_enable_line_out(true);
}
#endif

void source_toggle() {
    beep_dur(BEEP_SHORT);
    switch (g_source_info.source) {
    case SOURCE_HDZERO:
        page_source_select_analog();
        break;
    case SOURCE_AV_MODULE:
        page_source_select_hdzero();
        break;
    case SOURCE_AV_IN:
        page_source_select_hdzero();
        break;
    case SOURCE_HDMI_IN:
        page_source_select_hdzero();
        break;
    }
    Analog_Module_Power(0);
}

void source_cycle() {
    beep_dur(BEEP_SHORT);
    switch (g_source_info.source) {
    case SOURCE_HDZERO:
        if (g_source_info.hdmi_in_status) {
            page_source_select_hdmi();
        } else {
            page_source_select_av_in();
        }
        break;
    case SOURCE_AV_MODULE:
        page_source_select_hdzero();
        break;
    case SOURCE_AV_IN:
        page_source_select_analog();
        break;
    case SOURCE_HDMI_IN:
        page_source_select_av_in();
        break;
    }
    Analog_Module_Power(0);
}

static void page_source_on_click(uint8_t key, int sel) {
    switch (sel) {
    case ROW_HDZERO:
        page_source_select_hdzero();
        break;
    case ROW_ANALOG:
        page_source_select_analog();
        break;
    case ROW_HDMI:
        page_source_select_hdmi();
        break;
    case ROW_AV:
        page_source_select_av_in();
        break;
    case ROW_HDZ_WIDTH:
        btn_group_toggle_sel(&btn_group2);
        g_setting.source.hdzero_bw = btn_group_get_sel(&btn_group2);
        ini_putl("source", "hdzero_bw", g_setting.source.hdzero_bw, SETTING_INI);
        // Selecting "Both" makes the live bandwidth resolve via
        // g_hdz_detected_bw; seed it from whatever is currently open so a live
        // re-open (e.g. an OSD change) doesn't flip the picture's bandwidth.
        if (g_setting.source.hdzero_bw == SETTING_SOURCES_HDZERO_BW_BOTH)
            g_hdz_detected_bw = g_hw_stat.hdz_bw ? 1 : 0;
        break;
#if defined(HDZGOGGLE)
    case ROW_ANALOG_VIDEO: {
        btn_group_toggle_sel(&btn_group0);
        int av_sel = btn_group_get_sel(&btn_group0);
        // 0=NTSC, 1=PAL (manual), 2=Auto. Auto keeps the current active format
        // as its starting point; AV_in_detect adjusts it from there.
        g_setting.source.analog_auto = (av_sel == 2);
        ini_putl("source", "analog_auto", g_setting.source.analog_auto, SETTING_INI);
        if (av_sel < 2) {
            g_setting.source.analog_format = av_sel;
            ini_putl("source", "analog_format", g_setting.source.analog_format, SETTING_INI);
        }
        break;
    }
#elif defined(HDZGOGGLE2)
    case ROW_ANALOG_MODULE:
        btn_group_toggle_sel(&btn_group0);
        g_setting.source.analog_module = btn_group_get_sel(&btn_group0);
        ini_putl("source", "analog_module", g_setting.source.analog_module, SETTING_INI);
        break;
#endif
    case ROW_ANALOG_RATIO:
        btn_group_toggle_sel(&btn_group3);
        g_setting.source.analog_ratio = btn_group_get_sel(&btn_group3);
        ini_putl("source", "analog_ratio", g_setting.source.analog_ratio, SETTING_INI);
        break;
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    case ROW_AUTO_DETECT:
        page_source_select_auto_detect();
        break;
#endif
    case ROW_TEST_PATTERN:
        if (g_setting.storage.selftest && label[4]) {
            uint8_t oled_te = (oled_tst_mode != 0);
            uint8_t oled_tm = (oled_tst_mode & 0x0F) - 1;
            // LOGI("OLED TE=%d,TM=%d",oled_te,oled_tm);
            screen.pattern(oled_te, oled_tm, 4);
            if (++oled_tst_mode > 5) {
                oled_tst_mode = 0;
            }
        }
        break;
    }
    Analog_Module_Power(0);
}

static void page_source_enter() {
    in_sourcepage = true;
}

static void page_source_exit() {
    // LOGI("page_source_exit %d",oled_tst_mode);
    if ((oled_tst_mode != 0) && g_setting.storage.selftest) {
        screen.pattern(0, 0, 4);
        oled_tst_mode = 0;
    }
    in_sourcepage = false;
}

page_pack_t pp_source = {
    .p_arr = {
        .cur = 0,
    },

    .name = "Source",
    .create = page_source_create,
    .enter = page_source_enter,
    .exit = page_source_exit,
    .on_created = NULL,
    .on_update = NULL,
    .on_roller = NULL,
    .on_click = page_source_on_click,
    .on_right_button = NULL,
};
