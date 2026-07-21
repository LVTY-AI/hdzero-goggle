#include "ui/ui_image_setting.h"

#include <stdio.h>
#include <string.h>

#include <log/log.h>
#include <lvgl/lvgl.h>
#include <minIni.h>

#include "../conf/ui.h"
#include "core/app_state.h"
#include "core/common.hh"
#include "core/osd.h"
#include "driver/hardware.h"
#include "driver/screen.h"
#include "lang/language.h"
#include "ui/page_common.h"

///////////////////////////////////////////////////////////////////////////////
// locals
static lv_obj_t *canvas_ims;
static uint8_t canvas_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(IMS_CANVAS_WIDTH, IMS_CANVAS_HEIGHT)];
static lv_draw_line_dsc_t line_dsc;
static lv_draw_label_dsc_t label_dsc;
static ims_page_t ims_page;
static uint8_t ims_state;
static setting_image_source_t ims_source = SETTING_IMAGE_SOURCE_HDZERO;
static setting_image_source_t ims_active_source = SETTING_IMAGE_SOURCE_HDZERO;
static bool ims_analog_is_av_in;
// global
bool g_bShowIMS = false;

///////////////////////////////////////////////////////////////////////////////
//
enum {
    IMS_SOURCE,
    IMS_PANEL,
    IMS_BRIGHTNESS,
    IMS_SATURATION,
    IMS_CONTRAST,
    IMS_AUTO_OFF,
    IMS_BACK,
    IMS_RESET,
};

static const char *ims_source_name(setting_image_source_t source) {
    switch (source) {
    case SETTING_IMAGE_SOURCE_ANALOG:
        return _lang("Analog");
    case SETTING_IMAGE_SOURCE_HDZERO:
    default:
        return _lang("HDZero");
    }
}

static void ims_page_init(setting_image_source_t source) {
    int16_t x = 30;
    int16_t y = 5;
    char buf[64];
    const setting_image_video_t *video = settings_image_video(source);

    ims_page.items[IMS_SOURCE].x = x;
    ims_page.items[IMS_SOURCE].y = y;
    ims_page.items[IMS_SOURCE].type = 0;
    snprintf(buf, sizeof(buf), "%s: %s", _lang("Source"), ims_source_name(source));
    strcpy(ims_page.items[IMS_SOURCE].title, buf);
    ims_page.items[IMS_SOURCE].state = 1;

    ims_page.items[IMS_PANEL].x = x;
    ims_page.items[IMS_PANEL].y = y + 25;
    ims_page.items[IMS_PANEL].type = 1;
    snprintf(buf, sizeof(buf), "%s:", _lang("Panel"));
    strcpy(ims_page.items[IMS_PANEL].title, buf);
    ims_page.items[IMS_PANEL].range[0] = 0;
    ims_page.items[IMS_PANEL].range[1] = 12;
    ims_page.items[IMS_PANEL].value = g_setting.image.oled;
    ims_page.items[IMS_PANEL].state = 0;

    ims_page.items[IMS_BRIGHTNESS].x = x;
    ims_page.items[IMS_BRIGHTNESS].y = y + 50;
    ims_page.items[IMS_BRIGHTNESS].type = 1;
    snprintf(buf, sizeof(buf), "%s:", _lang("Brightness"));
    strcpy(ims_page.items[IMS_BRIGHTNESS].title, buf);
    ims_page.items[IMS_BRIGHTNESS].range[0] = 0;
    ims_page.items[IMS_BRIGHTNESS].range[1] = 78;
    ims_page.items[IMS_BRIGHTNESS].value = video->brightness;
    ims_page.items[IMS_BRIGHTNESS].state = 0;

    ims_page.items[IMS_SATURATION].x = x;
    ims_page.items[IMS_SATURATION].y = y + 75;
    ims_page.items[IMS_SATURATION].type = 1;
    snprintf(buf, sizeof(buf), "%s:", _lang("Saturation"));
    strcpy(ims_page.items[IMS_SATURATION].title, buf);
    ims_page.items[IMS_SATURATION].range[0] = 0;
    ims_page.items[IMS_SATURATION].range[1] = 47;
    ims_page.items[IMS_SATURATION].value = video->saturation;
    ims_page.items[IMS_SATURATION].state = 0;

    ims_page.items[IMS_CONTRAST].x = x;
    ims_page.items[IMS_CONTRAST].y = y + 100;
    ims_page.items[IMS_CONTRAST].type = 1;
    snprintf(buf, sizeof(buf), "%s:", _lang("Contrast"));
    strcpy(ims_page.items[IMS_CONTRAST].title, buf);
    ims_page.items[IMS_CONTRAST].range[0] = 0;
    ims_page.items[IMS_CONTRAST].range[1] = 47;
    ims_page.items[IMS_CONTRAST].value = video->contrast;
    ims_page.items[IMS_CONTRAST].state = 0;

    ims_page.items[IMS_AUTO_OFF].x = x;
    ims_page.items[IMS_AUTO_OFF].y = y + 125;
    ims_page.items[IMS_AUTO_OFF].type = 1;
    snprintf(buf, sizeof(buf), "Panel %s:", _lang("Auto Off"));
    strcpy(ims_page.items[IMS_AUTO_OFF].title, buf);
    ims_page.items[IMS_AUTO_OFF].range[0] = 0;
    ims_page.items[IMS_AUTO_OFF].range[1] = 4;
    ims_page.items[IMS_AUTO_OFF].value = g_setting.image.auto_off;
    ims_page.items[IMS_AUTO_OFF].state = 0;

    ims_page.items[IMS_BACK].x = x;
    ims_page.items[IMS_BACK].y = y + 150;
    ims_page.items[IMS_BACK].type = 0;
    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    strcpy(ims_page.items[IMS_BACK].title, buf);
    ims_page.items[IMS_BACK].state = 0;

    ims_page.items[IMS_RESET].x = x + 200;
    ims_page.items[IMS_RESET].y = y + 150;
    ims_page.items[IMS_RESET].type = 0;
    snprintf(buf, sizeof(buf), "%s", _lang("Reset All"));
    strcpy(ims_page.items[IMS_RESET].title, buf);
    ims_page.items[IMS_RESET].state = 0;

    ims_page.selection = IMS_SOURCE;
}

static void show_ims_slider(uint8_t index) {
    ims_slider_t *p_slider = &ims_page.items[index];

    char buf[32];
    lv_point_t points[2];

    if (p_slider->state == 0) { // 0=not selected, 1=selected, 2=slider bar selected
        label_dsc.color = DARK_GRAY;
        line_dsc.color = DARK_GRAY;
    } else if (p_slider->state == 1) {
        label_dsc.color = LIGHT_WHITE;
        line_dsc.color = LIGHT_WHITE;
    } else {
        label_dsc.color = p_slider->type == 0 ? LIGHT_GREEN : LIGHT_WHITE;
        line_dsc.color = LIGHT_GREEN;
    }

    lv_canvas_draw_text(canvas_ims, p_slider->x, p_slider->y, 200, &label_dsc, p_slider->title);
    if (p_slider->type == 0) {
        return;
    }

    switch (index) {
    case IMS_AUTO_OFF: { // auto off
        if (p_slider->value == 4)
            snprintf(buf, sizeof(buf), "%s", _lang("Never"));
        else
            snprintf(buf, sizeof(buf), "%d %s", (p_slider->value << 1) + 1, _lang("min"));
        break;
    }

    default:
        snprintf(buf, sizeof(buf), "%d", p_slider->value);
        break;
    }

    lv_canvas_draw_text(canvas_ims, 340 + p_slider->x, p_slider->y, 200, &label_dsc, buf);

    line_dsc.width = 3;
    points[0].x = SLIDER_XSTART;
    points[1].x = SLIDER_XEND;
    points[0].y = points[1].y = p_slider->y + 6;
    lv_canvas_draw_line(canvas_ims, points, 2, &line_dsc);

    line_dsc.width = 12;
    points[0].y = points[1].y = p_slider->y + 6;
    points[0].x = SLIDER_XSTART + p_slider->value * (SLIDER_XEND - SLIDER_XSTART - SLIDER_WIDTH) / (p_slider->range[1] - p_slider->range[0]);
    points[1].x = points[0].x + SLIDER_WIDTH;
    lv_canvas_draw_line(canvas_ims, points, 2, &line_dsc);
}

void ims_update() {
    if (!g_bShowIMS) {
        lv_obj_add_flag(canvas_ims, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Clear background
    lv_obj_clear_flag(canvas_ims, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_fill_bg(canvas_ims, DARK, LV_OPA_COVER);

    // Draw each items
    for (uint8_t i = 0; i < IMS_ITEM_COUNT; i++) {
        show_ims_slider(i);
    }
}

void image_settings_apply(setting_image_source_t source) {
    const setting_image_video_t *video = settings_image_video(source);

    screen.brightness(g_setting.image.oled);
    Set_Brightness(video->brightness);
    Set_Saturation(video->saturation);
    Set_Contrast(video->contrast);
}

void ims_set_source(setting_image_source_t source) {
    if (source == SETTING_IMAGE_SOURCE_ANALOG &&
        (g_source_info.source == SOURCE_AV_IN || g_source_info.source == SOURCE_AV_MODULE))
        ims_analog_is_av_in = g_source_info.source == SOURCE_AV_IN;

    ims_source = source;
    ims_active_source = source;
    ims_state = 0;
    ims_page_init(source);
    image_settings_apply(source);
}

setting_image_source_t ims_get_source(void) {
    return ims_source;
}

static void save_image_source(setting_image_source_t source, const setting_image_video_t *video) {
    const char *prefix;
    char key[32];

    switch (source) {
    case SETTING_IMAGE_SOURCE_ANALOG:
        prefix = "analog_";
        break;
    case SETTING_IMAGE_SOURCE_HDZERO:
    default:
        prefix = "hdzero_";
        break;
    }

    snprintf(key, sizeof(key), "%sbrightness", prefix);
    ini_putl("image", key, video->brightness, SETTING_INI);
    snprintf(key, sizeof(key), "%ssaturation", prefix);
    ini_putl("image", key, video->saturation, SETTING_INI);
    snprintf(key, sizeof(key), "%scontrast", prefix);
    ini_putl("image", key, video->contrast, SETTING_INI);

    // Keep the legacy values current for older firmware versions.
    ini_putl("image", "brightness", video->brightness, SETTING_INI);
    ini_putl("image", "saturation", video->saturation, SETTING_INI);
    ini_putl("image", "contrast", video->contrast, SETTING_INI);
}

static void ims_switch_input(setting_image_source_t source) {
    switch (source) {
    case SETTING_IMAGE_SOURCE_ANALOG:
        app_switch_to_analog(ims_analog_is_av_in);
        g_source_info.source = ims_analog_is_av_in ? SOURCE_AV_IN : SOURCE_AV_MODULE;
        break;
    case SETTING_IMAGE_SOURCE_HDZERO:
    default:
        app_switch_to_hdzero(true);
        g_source_info.source = SOURCE_HDZERO;
        break;
    }

    app_state_push(APP_STATE_IMS);
}

void ims_switch_source(setting_image_source_t source) {
    if (source == ims_source)
        return;

    if (ims_source == ims_active_source)
        ims_save();

    ims_source = source;
    ims_page_init(source);
    ims_state = 2;
    ims_page.items[IMS_SOURCE].state = 2;
}

static void ims_confirm_source(void) {
    if (ims_source != ims_active_source) {
        ims_switch_input(ims_source);
        ims_set_source(ims_source);
    }

    ims_page.items[IMS_SOURCE].state = 1;
    ims_state = 1;
}

void ims_init(void) {
    canvas_ims = lv_canvas_create(lv_scr_act());
    lv_obj_clear_flag(canvas_ims, LV_OBJ_FLAG_SCROLLABLE);

    lv_canvas_set_buffer(canvas_ims, canvas_buf, IMS_CANVAS_WIDTH, IMS_CANVAS_HEIGHT, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(canvas_ims, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(canvas_ims, LV_OBJ_FLAG_HIDDEN);
    lv_draw_line_dsc_init(&line_dsc);
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.font = UI_MENU_LABEL_FONT;
    g_bShowIMS = false;
    ims_set_source(SETTING_IMAGE_SOURCE_HDZERO);
}

void ims_save() {
    setting_image_video_t *video = settings_image_video(ims_source);

    g_setting.image.oled = ims_page.items[IMS_PANEL].value;
    ini_putl("image", "oled", g_setting.image.oled, SETTING_INI);
    g_setting.image.auto_off = ims_page.items[IMS_AUTO_OFF].value;
    ini_putl("image", "auto_off", g_setting.image.auto_off, SETTING_INI);

    video->brightness = ims_page.items[IMS_BRIGHTNESS].value;
    video->saturation = ims_page.items[IMS_SATURATION].value;
    video->contrast = ims_page.items[IMS_CONTRAST].value;
    save_image_source(ims_source, video);
    image_settings_apply(ims_source);

    osd_update_element_positions();
}

void change_oled_brightness(uint8_t key) {
    if (key == DIAL_KEY_UP) {
        if (g_setting.image.oled != MAX_SCREEN_BRIGHTNESS) {
            g_setting.image.oled += 1;
        } else {
            return;
        }
    } else if (key == DIAL_KEY_DOWN) {
        if (g_setting.image.oled != MIN_SCREEN_BRIGHTNESS) {
            g_setting.image.oled -= 1;
        } else {
            return;
        }
    }

    ini_putl("image", "oled", g_setting.image.oled, SETTING_INI);
    screen.brightness(g_setting.image.oled);
}

///////////////////////////////////////////////////////////////////////////////
// key:
//   1 = dial up
//   2 = dial down
//   3 = click
// return
//   1 = ims done
//   0 = ongoing
uint8_t ims_key(uint8_t key) {
    int16_t value;
    uint8_t ret = 0;
    LOGI("ims_key key: %d state: %d selection: %d", key, ims_state, ims_page.selection);

    if (ims_state == 0) {
        ims_state = 1;
    }

    if (ims_state == 1) { // select between items
        g_bShowIMS = true;

        switch (key) {
        case DIAL_KEY_UP:
            ims_page.items[ims_page.selection].state = 0;
            ims_page.selection++;
            if (ims_page.selection == IMS_ITEM_COUNT)
                ims_page.selection = 0;
            ims_page.items[ims_page.selection].state = 1;
            break;

        case DIAL_KEY_DOWN:
            ims_page.items[ims_page.selection].state = 0;
            if (ims_page.selection == 0)
                ims_page.selection = IMS_ITEM_COUNT - 1;
            else
                ims_page.selection--;
            ims_page.items[ims_page.selection].state = 1;
            break;

        case DIAL_KEY_CLICK:
            if (ims_page.selection == IMS_BACK) { //"<Back"
                ims_state = 0;
                g_bShowIMS = false;
                ims_save();
                ret = 1;
            } else if (ims_page.selection == IMS_RESET) { //"Reset All"
                g_setting.image.analog = g_setting_defaults.image.analog;
                g_setting.image.hdzero = g_setting_defaults.image.hdzero;
                save_image_source(SETTING_IMAGE_SOURCE_ANALOG, &g_setting.image.analog);
                save_image_source(SETTING_IMAGE_SOURCE_HDZERO, &g_setting.image.hdzero);
                ims_page.items[IMS_PANEL].value = g_setting_defaults.image.oled;
                ims_page.items[IMS_BRIGHTNESS].value = settings_image_video_defaults(ims_source)->brightness;
                ims_page.items[IMS_SATURATION].value = settings_image_video_defaults(ims_source)->saturation;
                ims_page.items[IMS_CONTRAST].value = settings_image_video_defaults(ims_source)->contrast;
                ims_page.items[IMS_AUTO_OFF].value = g_setting_defaults.image.auto_off;
                ims_save();
            } else {
                ims_page.items[ims_page.selection].state = 2;
                ims_state = 2;
            }
            break;

        default:
            LOGE("ims_key unhandled key %d", key);
            ims_state = 0;
            break;
        }
    } else if (ims_state == 2) { // tune up/down values
        g_bShowIMS = true;
        value = 0;

        if (ims_page.selection == IMS_SOURCE) {
            if (key == DIAL_KEY_DOWN) {
                if (ims_source == SETTING_IMAGE_SOURCE_HDZERO)
                    ims_switch_source(SETTING_IMAGE_SOURCE_ANALOG);
                else
                    ims_switch_source((setting_image_source_t)(ims_source + 1));
                ims_page.items[IMS_SOURCE].state = 2;
            } else if (key == DIAL_KEY_UP) {
                if (ims_source == SETTING_IMAGE_SOURCE_ANALOG)
                    ims_switch_source(SETTING_IMAGE_SOURCE_HDZERO);
                else
                    ims_switch_source((setting_image_source_t)(ims_source - 1));
                ims_page.items[IMS_SOURCE].state = 2;
            } else if (key == DIAL_KEY_CLICK) {
                ims_confirm_source();
            }
            return ret;
        }

        switch (key) {
        case DIAL_KEY_DOWN:
            value = ims_page.items[ims_page.selection].value;
            if (value != ims_page.items[ims_page.selection].range[1]) {
                value++;
                ims_page.items[ims_page.selection].value = value;
            }
            break;

        case DIAL_KEY_UP:
            value = ims_page.items[ims_page.selection].value;
            if (value != ims_page.items[ims_page.selection].range[0]) {
                value--;
                ims_page.items[ims_page.selection].value = value;
            }
            break;

        case DIAL_KEY_CLICK:
            value = ims_page.items[ims_page.selection].value;
            ims_page.items[ims_page.selection].state = 1;
            ims_state = 1;
            break;

        default:
            break;
        }

        switch (ims_page.selection) {
        case IMS_PANEL:
            screen.brightness(value);
            break;

        case IMS_BRIGHTNESS:
            Set_Brightness(value);
            break;

        case IMS_SATURATION:
            Set_Saturation(value);
            break;

        case IMS_CONTRAST:
            Set_Contrast(value);
            break;

        case IMS_AUTO_OFF:
            g_setting.image.auto_off = ims_page.items[IMS_AUTO_OFF].value;
            ini_putl("image", "auto_off", g_setting.image.auto_off, SETTING_INI);
            break;

        default:
            LOGE("ims_key unhandled selection %d", ims_page.selection);
            ims_state = 0;
            g_bShowIMS = false;
            break;
        }
    }
    return ret;
}
