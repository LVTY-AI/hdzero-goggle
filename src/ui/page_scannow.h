#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ui/ui_main_menu.h"
#include <lvgl/lvgl.h>

#define HDZERO_CHANNEL_NUM (g_setting.source.hdzero_band == RACE_BAND ? 12 : 8)
#define ANALOG_CHANNEL_NUM 48

typedef enum {
    RACE_BAND = 0,
    LOW_BAND = 1,
} band_t;

typedef enum {
    SCAN_MODE_HDZERO = 0,
    SCAN_MODE_ANALOG = 1,
    SCAN_MODE_AUTO   = 2,
} scan_mode_t;

int scan(void);
int scan_reinit(void);
void autoscan_exit(void);
void page_scannow_set_channel_label(void);

extern page_pack_t pp_scannow;

#ifdef __cplusplus
}
#endif
