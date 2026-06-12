#include "page_audio.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <minIni.h>

#include "../conf/ui.h"

#include "core/app_state.h"
#include "core/common.hh"
#include "core/dvr.h"
#include "lang/language.h"
#include "page_common.h"
#include "ui/ui_style.h"
#include "util/filesystem.h"
#include "util/system.h"
#include "wav_test.h"

#define AUDIO_VOLUME_MIN 0
#define DVR_AUDIO_VOLUME_MAX 8
#define LIVE_AUDIO_VOLUME_MAX 10
#define RECORD_GAIN_MAX 7
#define AUDIO_TEST_SAMPLE "/mnt/app/app/audio/dvr_playback_volume_test.wav"
#define AUDIO_TEST_SAMPLE_SDCARD "/mnt/extsd/dvr_playback_volume_test.wav"
#define AUDIO_TEST_CAPTURE "/tmp/hdzero_audio_test.wav"
#define AUDIO_TEST_APLAY "/mnt/app/app/record/audio/aplay"
#define AUDIO_TEST_ARECORD "/mnt/app/app/record/audio/arecord"
#define AUDIO_TEST_COUNT 4
#define AUDIO_TEST_SECONDS 5            // live stream / record length
#define AUDIO_TEST_DVR_SAMPLE_MS 10000  // bundled test WAV length
// content width inside the 3px button border, used by the progress fill
#define AUDIO_TEST_FILL_MAX_W (UI_AUDIO_TEST_BUTTON_WIDTH - 6)

static btn_group_t btn_group_record_audio;
static btn_group_t btn_group_audio_source;
static slider_group_t slider_group_dvr_audio_volume;
static slider_group_t slider_group_live_audio_volume;
static slider_group_t slider_group_mic_gain;
static slider_group_t slider_group_linein_gain;
static lv_obj_t *test_container;
static lv_obj_t *test_btn[AUDIO_TEST_COUNT];
static lv_obj_t *test_label[AUDIO_TEST_COUNT];
static lv_obj_t *test_fill[AUDIO_TEST_COUNT];

enum {
    ROW_RECORD_AUDIO = 0,
    ROW_AUDIO_SOURCE,
    ROW_VOLUME_HEADER,
    ROW_DVR_AUDIO_VOLUME,
    ROW_LIVE_AUDIO_VOLUME,
    ROW_MIC_GAIN,
    ROW_LINEIN_GAIN,
    ROW_RUN_TEST, // last item of the Volume Control section
    ROW_BACK,
    ROW_NOTE,

    ROW_COUNT
};

typedef enum {
    AUDIO_TEST_DVR = 0,
    AUDIO_TEST_LIVE,
    AUDIO_TEST_MIC,
    AUDIO_TEST_LINE_AV,
} audio_test_mode_t;

typedef enum {
    AUDIO_TEST_PHASE_IDLE = 0,
    AUDIO_TEST_PHASE_RECORDING,
    AUDIO_TEST_PHASE_PLAYING,
} audio_test_phase_t;

static int selected_slider_row = -1;
static bool selected_slider_changed;
static bool selected_test_active;
static volatile bool audio_test_running;
static volatile audio_test_phase_t audio_test_phase;
static volatile uint32_t audio_test_phase_duration_ms; // 0 = indeterminate
static volatile audio_test_mode_t active_test_mode = AUDIO_TEST_DVR;
static volatile audio_test_mode_t selected_test_mode = AUDIO_TEST_DVR;
static uint32_t audio_test_update_ms;

static lv_coord_t col_dsc[] = {UI_AUDIO_COLS};
static lv_coord_t row_dsc[] = {UI_AUDIO_ROWS};

static void update_visibility() {
    btn_group_enable(&btn_group_audio_source, btn_group_record_audio.current == 0);

    if (btn_group_record_audio.current == 0) {
        lv_obj_add_flag(pp_audio.p_arr.panel[ROW_AUDIO_SOURCE], FLAG_SELECTABLE);
    } else {
        lv_obj_clear_flag(pp_audio.p_arr.panel[ROW_AUDIO_SOURCE], FLAG_SELECTABLE);
    }

    lv_obj_clear_flag(pp_audio.p_arr.panel[ROW_VOLUME_HEADER], FLAG_SELECTABLE);
    lv_obj_clear_flag(pp_audio.p_arr.panel[ROW_NOTE], FLAG_SELECTABLE);
}

static void create_test_button(lv_obj_t *parent, audio_test_mode_t mode, const char *name) {
    test_btn[mode] = lv_btn_create(parent);
    lv_obj_set_size(test_btn[mode], UI_AUDIO_TEST_BUTTON_WIDTH, UI_AUDIO_TEST_BUTTON_HEIGHT);
    lv_obj_set_pos(test_btn[mode], mode * (UI_AUDIO_TEST_BUTTON_WIDTH + UI_AUDIO_TEST_BUTTON_GAP), 0);
    lv_obj_set_style_bg_opa(test_btn[mode], LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(test_btn[mode], 0, 0);
    lv_obj_set_style_border_width(test_btn[mode], 3, 0);
    lv_obj_set_style_border_color(test_btn[mode], lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_radius(test_btn[mode], 0, 0);
    lv_obj_set_style_pad_all(test_btn[mode], 0, 0);

    // Progress fill: grows left-to-right behind the label while a test runs
    // (red while recording, green while playing back). Created before the
    // label so the text stays on top.
    test_fill[mode] = lv_obj_create(test_btn[mode]);
    lv_obj_remove_style_all(test_fill[mode]);
    lv_obj_set_size(test_fill[mode], 0, UI_AUDIO_TEST_BUTTON_HEIGHT - 6);
    lv_obj_align(test_fill[mode], LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(test_fill[mode], LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(test_fill[mode], LV_OBJ_FLAG_SCROLLABLE);

    test_label[mode] = lv_label_create(test_btn[mode]);
    lv_label_set_text(test_label[mode], name);
    lv_obj_set_style_text_font(test_label[mode], UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_color(test_label[mode], lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_center(test_label[mode]);
}

static lv_obj_t *page_audio_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[256];
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);

    snprintf(buf, sizeof(buf), "%s:", _lang("Audio"));
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

    create_btn_group_item(&btn_group_record_audio, cont, 2, _lang("Record Audio"), _lang("Yes"), _lang("No"), "", "", ROW_RECORD_AUDIO);
    create_btn_group_item(&btn_group_audio_source, cont, 3, _lang("Audio Source"), _lang("Mic"), _lang("Line In"), _lang("A/V In"), "", ROW_AUDIO_SOURCE);
    snprintf(buf, sizeof(buf), "%s:", _lang("Volume Control"));
    create_label_item(cont, buf, 0, ROW_VOLUME_HEADER, 2);
    create_label_item(cont, _lang("Run Test"), 1, ROW_RUN_TEST, 1);

    test_container = lv_obj_create(cont);
    lv_obj_set_size(test_container,
                    AUDIO_TEST_COUNT * UI_AUDIO_TEST_BUTTON_WIDTH + (AUDIO_TEST_COUNT - 1) * UI_AUDIO_TEST_BUTTON_GAP,
                    UI_AUDIO_TEST_BUTTON_HEIGHT);
    lv_obj_clear_flag(test_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(test_container, &style_context, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(test_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(test_container, 0, 0);
    lv_obj_set_style_pad_all(test_container, 0, 0);
    lv_obj_set_grid_cell(test_container, LV_GRID_ALIGN_START, 2, 4,
                         LV_GRID_ALIGN_CENTER, ROW_RUN_TEST, 1);
    create_test_button(test_container, AUDIO_TEST_DVR, _lang("DVR"));
    create_test_button(test_container, AUDIO_TEST_LIVE, _lang("Live"));
    create_test_button(test_container, AUDIO_TEST_MIC, _lang("Mic"));
    create_test_button(test_container, AUDIO_TEST_LINE_AV, _lang("Line/AV"));

    create_slider_item(&slider_group_dvr_audio_volume, cont, _lang("DVR Playback"), DVR_AUDIO_VOLUME_MAX, g_setting.record.dvr_audio_volume, ROW_DVR_AUDIO_VOLUME);
    lv_obj_set_grid_cell(slider_group_dvr_audio_volume.name, LV_GRID_ALIGN_START, 1, 1,
                         LV_GRID_ALIGN_CENTER, ROW_DVR_AUDIO_VOLUME, 1);
    lv_obj_set_grid_cell(slider_group_dvr_audio_volume.slider, LV_GRID_ALIGN_STRETCH, 2, 3,
                         LV_GRID_ALIGN_CENTER, ROW_DVR_AUDIO_VOLUME, 1);
    lv_obj_set_grid_cell(slider_group_dvr_audio_volume.label, LV_GRID_ALIGN_START, 5, 1,
                         LV_GRID_ALIGN_CENTER, ROW_DVR_AUDIO_VOLUME, 1);
    create_slider_item(&slider_group_live_audio_volume, cont, _lang("Live Volume"), LIVE_AUDIO_VOLUME_MAX, g_setting.record.live_audio_volume, ROW_LIVE_AUDIO_VOLUME);
    lv_obj_set_grid_cell(slider_group_live_audio_volume.name, LV_GRID_ALIGN_START, 1, 1,
                         LV_GRID_ALIGN_CENTER, ROW_LIVE_AUDIO_VOLUME, 1);
    lv_obj_set_grid_cell(slider_group_live_audio_volume.slider, LV_GRID_ALIGN_STRETCH, 2, 3,
                         LV_GRID_ALIGN_CENTER, ROW_LIVE_AUDIO_VOLUME, 1);
    lv_obj_set_grid_cell(slider_group_live_audio_volume.label, LV_GRID_ALIGN_START, 5, 1,
                         LV_GRID_ALIGN_CENTER, ROW_LIVE_AUDIO_VOLUME, 1);
    create_slider_item(&slider_group_mic_gain, cont, _lang("Mic Gain"), RECORD_GAIN_MAX, g_setting.record.mic_gain, ROW_MIC_GAIN);
    lv_obj_set_grid_cell(slider_group_mic_gain.name, LV_GRID_ALIGN_START, 1, 1,
                         LV_GRID_ALIGN_CENTER, ROW_MIC_GAIN, 1);
    lv_obj_set_grid_cell(slider_group_mic_gain.slider, LV_GRID_ALIGN_STRETCH, 2, 3,
                         LV_GRID_ALIGN_CENTER, ROW_MIC_GAIN, 1);
    lv_obj_set_grid_cell(slider_group_mic_gain.label, LV_GRID_ALIGN_START, 5, 1,
                         LV_GRID_ALIGN_CENTER, ROW_MIC_GAIN, 1);
    create_slider_item(&slider_group_linein_gain, cont, _lang("Line/AV Gain"), RECORD_GAIN_MAX, g_setting.record.linein_gain, ROW_LINEIN_GAIN);
    lv_obj_set_grid_cell(slider_group_linein_gain.name, LV_GRID_ALIGN_START, 1, 1,
                         LV_GRID_ALIGN_CENTER, ROW_LINEIN_GAIN, 1);
    lv_obj_set_grid_cell(slider_group_linein_gain.slider, LV_GRID_ALIGN_STRETCH, 2, 3,
                         LV_GRID_ALIGN_CENTER, ROW_LINEIN_GAIN, 1);
    lv_obj_set_grid_cell(slider_group_linein_gain.label, LV_GRID_ALIGN_START, 5, 1,
                         LV_GRID_ALIGN_CENTER, ROW_LINEIN_GAIN, 1);
    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, ROW_BACK, 1);
    lv_obj_t *note = lv_label_create(cont);
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    // "Active", not "wired": a received analog signal (e.g. a transmitting
    // drone) feeds the tests too, not just the AV jack.
    lv_label_set_text(note, _lang("1. Mic: record 5s, auto playback.\n2. Line/AV: record 5s, auto playback.\n3. Test fill: pulsing = loading, red = recording, green = playback.\n4. Live & Line/AV need an active analog audio source."));
#else
    // The faint clicking is menu-mode electrical interference coupling into
    // the codec converters (confirmed by waveform analysis: ~130 analog
    // impulses/s in menu captures, zero in video-mode recordings on the same
    // unit). Not reachable from software; flights and DVR playback are clean.
    lv_label_set_text(note, _lang("1. Mic: record 5s, auto playback.\n2. Line/AV: record 5s, auto playback.\n3. Test fill: pulsing = loading, red = recording, green = playback.\n4. Clicking or static during tests is normal in the menu, flight recordings are unaffected."));
#endif
    lv_obj_set_style_text_font(note, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(note, 0, 0);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_grid_cell(note, LV_GRID_ALIGN_START, 1, 5, LV_GRID_ALIGN_START, ROW_NOTE, 1);

    btn_group_set_sel(&btn_group_record_audio, g_setting.record.audio ? 0 : 1);
    btn_group_set_sel(&btn_group_audio_source, g_setting.record.audio_source);
    update_slider_item_with_value(&slider_group_dvr_audio_volume, g_setting.record.dvr_audio_volume);
    update_slider_item_with_value(&slider_group_live_audio_volume, g_setting.record.live_audio_volume);
    update_slider_item_with_value(&slider_group_mic_gain, g_setting.record.mic_gain);
    update_slider_item_with_value(&slider_group_linein_gain, g_setting.record.linein_gain);

    update_visibility();

    return page;
}

static void page_audio_update_test_indicator(uint32_t delta_ms) {
    static audio_test_phase_t prev_phase = AUDIO_TEST_PHASE_IDLE;
    static bool prev_running = false;
    static uint32_t phase_elapsed_ms = 0;
    bool blink_on;
    lv_color_t record_color = lv_color_make(0xff, 0x20, 0x20);
    lv_color_t play_color = lv_color_make(0, 0xc0, 0);
    lv_color_t idle_color = lv_color_hex(TEXT_COLOR_DEFAULT);
    lv_color_t selected_color = lv_color_make(0xff, 0xff, 0xff);
    lv_color_t focused_color = lv_color_make(0xff, 0, 0);

    audio_test_update_ms += delta_ms;
    blink_on = (audio_test_update_ms / 250) % 2 == 0;

    // Track how long the current phase has been showing so the fill can grow
    // in step with the known phase duration.
    audio_test_phase_t phase = audio_test_phase;
    bool running = audio_test_running;
    if (phase != prev_phase || running != prev_running) {
        phase_elapsed_ms = 0;
        prev_phase = phase;
        prev_running = running;
    } else {
        phase_elapsed_ms += delta_ms;
    }

    for (int i = 0; i < AUDIO_TEST_COUNT; i++) {
        lv_color_t color = i == (int)selected_test_mode ? selected_color : idle_color;
        lv_color_t border_color = color;
        lv_opa_t bg_opa = i == (int)selected_test_mode ? LV_OPA_20 : LV_OPA_TRANSP;
        lv_coord_t fill_w = 0;
        lv_color_t fill_color = idle_color;
        lv_opa_t fill_opa = LV_OPA_TRANSP;

        if (selected_test_active && i == (int)selected_test_mode)
            border_color = focused_color;

        if (running && i == (int)active_test_mode) {
            color = selected_color;
            if (phase == AUDIO_TEST_PHASE_RECORDING || phase == AUDIO_TEST_PHASE_PLAYING) {
                // Scan-bar style: red fills while recording, green while
                // playing back, growing left-to-right over the phase length.
                uint32_t dur = audio_test_phase_duration_ms;
                uint32_t el = (dur && phase_elapsed_ms > dur) ? dur : phase_elapsed_ms;
                fill_color = (phase == AUDIO_TEST_PHASE_RECORDING) ? record_color : play_color;
                fill_opa = LV_OPA_70;
                fill_w = dur ? (lv_coord_t)((uint64_t)AUDIO_TEST_FILL_MAX_W * el / dur)
                             : AUDIO_TEST_FILL_MAX_W;
                border_color = fill_color;
            } else {
                // Setting up (audio routing before the first phase starts,
                // noticeable on the Mic test): pulse a dim full-width fill so
                // the wait reads as activity, not a hang.
                fill_color = selected_color;
                fill_opa = blink_on ? LV_OPA_30 : LV_OPA_10;
                fill_w = AUDIO_TEST_FILL_MAX_W;
            }
        }
        lv_obj_set_style_text_color(test_label[i], color, 0);
        lv_obj_set_style_border_color(test_btn[i], border_color, 0);
        lv_obj_set_style_bg_opa(test_btn[i], bg_opa, 0);
        if (test_fill[i]) {
            lv_obj_set_width(test_fill[i], fill_w);
            lv_obj_set_style_bg_color(test_fill[i], fill_color, 0);
            lv_obj_set_style_bg_opa(test_fill[i], fill_opa, 0);
        }
    }
}

static slider_group_t *page_audio_selected_slider() {
    if (selected_slider_row == ROW_DVR_AUDIO_VOLUME)
        return &slider_group_dvr_audio_volume;
    if (selected_slider_row == ROW_LIVE_AUDIO_VOLUME)
        return &slider_group_live_audio_volume;
    if (selected_slider_row == ROW_MIC_GAIN)
        return &slider_group_mic_gain;
    if (selected_slider_row == ROW_LINEIN_GAIN)
        return &slider_group_linein_gain;
    return NULL;
}

static int page_audio_selected_slider_max() {
    if (selected_slider_row == ROW_DVR_AUDIO_VOLUME)
        return DVR_AUDIO_VOLUME_MAX;
    if (selected_slider_row == ROW_LIVE_AUDIO_VOLUME)
        return LIVE_AUDIO_VOLUME_MAX;
    return RECORD_GAIN_MAX;
}

static int *page_audio_selected_setting() {
    if (selected_slider_row == ROW_DVR_AUDIO_VOLUME)
        return &g_setting.record.dvr_audio_volume;
    if (selected_slider_row == ROW_LIVE_AUDIO_VOLUME)
        return &g_setting.record.live_audio_volume;
    if (selected_slider_row == ROW_MIC_GAIN)
        return &g_setting.record.mic_gain;
    if (selected_slider_row == ROW_LINEIN_GAIN)
        return &g_setting.record.linein_gain;
    return NULL;
}

static void page_audio_update_slider(int value) {
    slider_group_t *slider_group = page_audio_selected_slider();
    int *setting_value = page_audio_selected_setting();
    int max_value = page_audio_selected_slider_max();

    if (slider_group == NULL || setting_value == NULL)
        return;

    if (value < AUDIO_VOLUME_MIN)
        value = AUDIO_VOLUME_MIN;
    else if (value > max_value)
        value = max_value;

    if (*setting_value == value)
        return;

    *setting_value = value;
    update_slider_item_with_value(slider_group, *setting_value);
    selected_slider_changed = true;
}

static void page_audio_exit_slider() {
    slider_group_t *slider_group = page_audio_selected_slider();

    if (slider_group != NULL)
        lv_obj_add_style(slider_group->slider, &style_silder_main, LV_PART_MAIN);

    app_state_push(APP_STATE_SUBMENU);

    if (selected_slider_changed) {
        if (selected_slider_row == ROW_DVR_AUDIO_VOLUME) {
            ini_putl("record", "dvr_audio_volume_v2", g_setting.record.dvr_audio_volume, SETTING_INI);
        } else if (selected_slider_row == ROW_LIVE_AUDIO_VOLUME) {
            ini_putl("record", "live_audio_volume", g_setting.record.live_audio_volume, SETTING_INI);
            dvr_set_live_audio_volume(g_setting.record.live_audio_volume);
        } else if (selected_slider_row == ROW_MIC_GAIN) {
            ini_putl("record", "mic_gain", g_setting.record.mic_gain, SETTING_INI);
            if (g_setting.record.audio_source == SETTING_RECORD_AUDIO_SOURCE_MIC)
                dvr_set_mic_gain(g_setting.record.mic_gain);
        } else if (selected_slider_row == ROW_LINEIN_GAIN) {
            ini_putl("record", "linein_gain", g_setting.record.linein_gain, SETTING_INI);
            if (g_setting.record.audio_source != SETTING_RECORD_AUDIO_SOURCE_MIC)
                dvr_set_linein_gain(g_setting.record.linein_gain);
        }
        selected_slider_changed = false;
    }

    selected_slider_row = -1;
}

static void page_audio_exit() {
    if (selected_slider_row != -1) {
        page_audio_exit_slider();
    }
    selected_test_active = false;
}

static void page_audio_on_roller(uint8_t key) {
    int32_t value;
    slider_group_t *slider_group = page_audio_selected_slider();

    if (selected_test_active) {
        if (key == DIAL_KEY_UP) {
            selected_test_mode = selected_test_mode == 0 ? AUDIO_TEST_COUNT - 1 : selected_test_mode - 1;
        } else if (key == DIAL_KEY_DOWN) {
            selected_test_mode = (selected_test_mode + 1) % AUDIO_TEST_COUNT;
        }
        return;
    }

    if (slider_group == NULL)
        return;

    value = lv_slider_get_value(slider_group->slider);
    if (key == DIAL_KEY_UP && value > AUDIO_VOLUME_MIN) {
        value--;
    } else if (key == DIAL_KEY_DOWN && value < page_audio_selected_slider_max()) {
        value++;
    }

    page_audio_update_slider(value);
}

static void page_audio_play_wav(const char *path) {
#if defined(HDZGOGGLE)
    // MPI AO engine, not aplay: the ALSA hw path renders static-laden audio
    // on the G1's kernel no matter how it is buffered, while the engine that
    // plays normal DVR files is clean on every target.
    wav_test_play(path);
#else
    // BoxPro/G2 keep the ALSA transport: plughw with deep buffers (0.5s,
    // 125ms periods) is clean on their kernel, and these tests shipped
    // working this way.
    char buf[256];
    snprintf(buf, sizeof(buf), "%s -D plughw:audiocodec -B 500000 -F 125000 %s",
             AUDIO_TEST_APLAY, path);
    system_exec(buf);
#endif
}

static void page_audio_enable_dac_playback(void) {
    char buf[128];

    snprintf(buf, sizeof(buf), "%s out_on", AUDIO_SEL_SH);
    system_exec(buf);
#if defined(HDZGOGGLE)
    // Replicate the Playback menu's codec state exactly -- the only playback
    // route that is clean on the G1. It never enables the Output Mixer DAC
    // path (out_dac_on): the DAC reaches LINEOUT through the LINEOUT mux
    // alone. Every static-laden test playback since the first version had
    // out_dac_on in common, across both the ALSA and the MPI transports.
    dvr_mute_live_audio(); // cut LINEIN from the output mixer, like adec2ao
    dvr_set_dvr_audio_volume(g_setting.record.dvr_audio_volume);
#else
    snprintf(buf, sizeof(buf), "%s out_linein_off", AUDIO_SEL_SH);
    system_exec(buf);
    // Cut the remaining analog inputs to the output mixer too: a MIC boost
    // stage left enabled by the board's boot state feeds open-mic noise to
    // the lineout whenever it is on (nothing in the app ever switches these).
    snprintf(buf, sizeof(buf), "%s out_mic1_off", AUDIO_SEL_SH);
    system_exec(buf);
    snprintf(buf, sizeof(buf), "%s out_mic2_off", AUDIO_SEL_SH);
    system_exec(buf);
    snprintf(buf, sizeof(buf), "%s out_dac_on", AUDIO_SEL_SH);
    system_exec(buf);
    // aplay drives AIF1 timeslot 0 only; force the documented clean playback
    // route (record/audio/audiocodec/audio-setup.txt). The MPI AO engine sets
    // its own codec route up (which is why normal DVR playback is clean), but
    // a raw aplay inherits whatever DAC-mixer state the board booted with --
    // an enabled ADC sidetone or an idle timeslot-1 input mixes noise
    // straight into the DAC, heard as static under the test sample.
    system_exec("amixer cset name='DACL Mixer AIF1DA0L Switch' 1");
    system_exec("amixer cset name='DACR Mixer AIF1DA0R Switch' 1");
    system_exec("amixer cset name='DACL Mixer ADCL Switch' 0");
    system_exec("amixer cset name='DACR Mixer ADCR Switch' 0");
    dvr_set_dvr_audio_volume(g_setting.record.dvr_audio_volume);
    // dvr_set_dvr_audio_volume raises both timeslot volumes; keep the unused
    // timeslot 1 silent while the test plays (the next real playback or test
    // restores it).
    system_exec("amixer cset name='AIF1 DAC timeslot 1 volume' 0,0");
#endif
}

static void page_audio_disable_dac_playback(bool live_audio_was_enabled) {
    char buf[128];

#if !defined(HDZGOGGLE)
    // G1 never enabled the Output Mixer DAC path (see
    // page_audio_enable_dac_playback), so there is nothing to switch off.
    snprintf(buf, sizeof(buf), "%s out_dac_off", AUDIO_SEL_SH);
    system_exec(buf);
#endif

    if (live_audio_was_enabled) {
        dvr_restore_live_audio();
    } else {
        snprintf(buf, sizeof(buf), "%s out_off", AUDIO_SEL_SH);
        system_exec(buf);
    }
}

static const char *page_audio_test_sample_path(void) {
    if (fs_file_exists(AUDIO_TEST_SAMPLE_SDCARD))
        return AUDIO_TEST_SAMPLE_SDCARD;
    return AUDIO_TEST_SAMPLE;
}

static const char *page_audio_capture_path(void) {
    return AUDIO_TEST_CAPTURE;
}

static void page_audio_capture_wav(setting_record_audio_source_t source) {
    dvr_select_audio_source(source);
    audio_test_phase_duration_ms = AUDIO_TEST_SECONDS * 1000;
    audio_test_phase = AUDIO_TEST_PHASE_RECORDING;
#if defined(HDZGOGGLE)
    // MPI AI engine, not arecord: the captured clips themselves carried the
    // static on the G1 (confirmed by playing them on a computer) regardless
    // of ALSA device or buffering, while normal DVR recordings -- made by
    // this engine -- are clean.
    wav_test_record(page_audio_capture_path(), AUDIO_TEST_SECONDS);
#else
    // BoxPro/G2: ALSA capture is clean on their kernel (see
    // page_audio_play_wav).
    char buf[256];
    snprintf(buf, sizeof(buf), "%s -D plughw:audiocodec -B 500000 -F 125000 -t wav -f S16_LE -c2 -r 48000 -d %d %s",
             AUDIO_TEST_ARECORD, AUDIO_TEST_SECONDS, page_audio_capture_path());
    system_exec(buf);
#endif
}

static void *page_audio_test_thread(void *arg) {
    audio_test_mode_t mode = (audio_test_mode_t)(intptr_t)arg;
    setting_record_audio_source_t previous_source = g_setting.record.audio_source;
    bool live_audio_was_enabled = dvr_live_audio_is_enabled();

    // No RX handling on BoxPro/G2: the internal analog module's audio only
    // reaches the codec in analog video mode (a wired AV-in source plays
    // through these tests in the menu, a powered+tuned RX does not), so
    // powering it up here just made heat. From the menu the Live/Line-AV
    // tests cover the wired Line In / A/V In path.

    active_test_mode = mode;
    switch (mode) {
    case AUDIO_TEST_MIC:
        dvr_mute_live_audio();
        page_audio_capture_wav(SETTING_RECORD_AUDIO_SOURCE_MIC);
        page_audio_enable_dac_playback();
        audio_test_phase_duration_ms = AUDIO_TEST_SECONDS * 1000;
        audio_test_phase = AUDIO_TEST_PHASE_PLAYING;
        page_audio_play_wav(page_audio_capture_path());
        page_audio_disable_dac_playback(live_audio_was_enabled);
        dvr_select_audio_source(previous_source);
        break;
    case AUDIO_TEST_LIVE: {
        dvr_select_audio_source(SETTING_RECORD_AUDIO_SOURCE_LINE_IN);
        dvr_enable_line_out(true);
        dvr_set_live_audio_volume(g_setting.record.live_audio_volume);
        audio_test_phase_duration_ms = AUDIO_TEST_SECONDS * 1000;
        audio_test_phase = AUDIO_TEST_PHASE_PLAYING;
        sleep(AUDIO_TEST_SECONDS);
        if (!live_audio_was_enabled)
            dvr_enable_line_out(false);
        else
            dvr_set_live_audio_volume(g_setting.record.live_audio_volume);
        dvr_select_audio_source(previous_source);
        break;
    }
    case AUDIO_TEST_LINE_AV:
        dvr_mute_live_audio();
        page_audio_capture_wav(SETTING_RECORD_AUDIO_SOURCE_LINE_IN);
        page_audio_enable_dac_playback();
        audio_test_phase_duration_ms = AUDIO_TEST_SECONDS * 1000;
        audio_test_phase = AUDIO_TEST_PHASE_PLAYING;
        page_audio_play_wav(page_audio_capture_path());
        page_audio_disable_dac_playback(live_audio_was_enabled);
        dvr_select_audio_source(previous_source);
        break;
    case AUDIO_TEST_DVR:
        dvr_mute_live_audio();
        page_audio_enable_dac_playback();
        audio_test_phase_duration_ms = AUDIO_TEST_DVR_SAMPLE_MS;
        audio_test_phase = AUDIO_TEST_PHASE_PLAYING;
        page_audio_play_wav(page_audio_test_sample_path());
        page_audio_disable_dac_playback(live_audio_was_enabled);
        break;
    }

    audio_test_phase = AUDIO_TEST_PHASE_IDLE;
    audio_test_running = false;
    return NULL;
}

static void page_audio_run_test(void) {
    pthread_t tid;
    audio_test_mode_t mode = selected_test_mode;

    // tests reroute the record audio mux/gains, which would corrupt an active recording
    if (audio_test_running || dvr_is_recording)
        return;

    active_test_mode = mode;
    audio_test_phase = AUDIO_TEST_PHASE_IDLE;
    audio_test_update_ms = 0;
    audio_test_running = true;
    if (pthread_create(&tid, NULL, page_audio_test_thread, (void *)(intptr_t)mode) == 0) {
        pthread_detach(tid);
    } else {
        audio_test_running = false;
    }
}

static void page_audio_on_click(uint8_t key, int sel) {
    (void)key;

    if (selected_slider_row != -1) {
        page_audio_exit_slider();
        return;
    }

    if (selected_test_active) {
        if (audio_test_running)
            return;
        selected_test_active = false;
        app_state_push(APP_STATE_SUBMENU);
        page_audio_run_test();
        return;
    }

    if (sel == ROW_RECORD_AUDIO) {
        btn_group_toggle_sel(&btn_group_record_audio);
        g_setting.record.audio = !btn_group_get_sel(&btn_group_record_audio);
        settings_put_bool("record", "audio", g_setting.record.audio);
        update_visibility();
    } else if (sel == ROW_AUDIO_SOURCE) {
        btn_group_toggle_sel(&btn_group_audio_source);
        g_setting.record.audio_source = btn_group_get_sel(&btn_group_audio_source);
        ini_putl("record", "audio_source", g_setting.record.audio_source, SETTING_INI);
    } else if (sel == ROW_RUN_TEST) {
        if (audio_test_running || dvr_is_recording)
            return;
        selected_test_active = true;
        app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
    } else if (sel == ROW_DVR_AUDIO_VOLUME || sel == ROW_LIVE_AUDIO_VOLUME ||
               sel == ROW_MIC_GAIN || sel == ROW_LINEIN_GAIN) {
        slider_group_t *slider_group;

        selected_slider_row = sel;
        selected_slider_changed = false;
        slider_group = page_audio_selected_slider();

        app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
        if (slider_group != NULL)
            lv_obj_add_style(slider_group->slider, &style_silder_select, LV_PART_MAIN);
    }
}

page_pack_t pp_audio = {
    .p_arr = {
        .cur = 0,
        .max = ROW_BACK + 1,
    },
    .name = "Audio",
    .create = page_audio_create,
    .enter = NULL,
    .exit = page_audio_exit,
    .on_created = NULL,
    .on_update = page_audio_update_test_indicator,
    .on_roller = page_audio_on_roller,
    .on_click = page_audio_on_click,
    .on_right_button = NULL,
};
