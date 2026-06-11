#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include <lvgl/lvgl.h>

#include "defines.h"

#define OSD_VNUM       32
#define OSD_HNUM       16
#define OSD_WIDTH_HD   24
#define OSD_HEIGHT_HD  36
#define OSD_WIDTH_FHD  36
#define OSD_HEIGHT_FHD 54
#define OSD_BOUNDRY_0  0
#define OSD_BOUNDRY_1  6

typedef enum {
    OSD_RESOURCE_720 = 0,
    OSD_RESOURCE_1080,

    OSD_RESOURCE_TOTAL
} osd_resource_t;

typedef enum {
    OSD_CLOCK_DATE = 0,
    OSD_CLOCK_TIME,
    OSD_CLOCK_FORMAT,

    OSD_CLOCK_TOTAL
} osd_clock_t;

typedef struct {
    lv_obj_t *topfan_speed[2];
    lv_obj_t *vtx_temp[2];
    lv_obj_t *battery_low[2];
    lv_obj_t *battery_voltage[2];
    lv_obj_t *vrx_temp[2];
    lv_obj_t *latency_lock[2];
    lv_obj_t *channel[2];
    lv_obj_t *sd_rec[2];
    lv_obj_t *vlq[2];
    lv_obj_t *ant0[2];
    lv_obj_t *ant1[2];
    lv_obj_t *ant2[2];
    lv_obj_t *ant3[2];
    lv_obj_t *osd_tempe[2][3]; // top,left,bot
    lv_obj_t *clock[2][OSD_CLOCK_TOTAL];
} osd_hdzero_t;

typedef struct
{
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} __attribute__((packed)) bitMAPFILEHEADER;

typedef struct
{
    uint32_t biSize;
    uint32_t biWidth;
    uint32_t biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    uint32_t biXPelsPerMeter;
    uint32_t biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} __attribute__((packed)) bitMAPINFOHEADER;

typedef struct
{
    bitMAPFILEHEADER file;
    bitMAPINFOHEADER info;
} __attribute__((packed)) bmpFileHead;

typedef struct {
    lv_img_dsc_t data[512];
} osd_font_t;

extern uint8_t channel_osd_mode;
// Override for the channel preview namespace (only consulted when
// channel_osd_mode has the 0x80 "preview" bit set). 0 = use current source;
// 1 = force HDZ naming; 2 = force analog naming. Used by the BoxPro
// auto-detect dial to show readable "To R5/ANA?" previews even when the
// previewed entry's protocol differs from the active source.
extern uint8_t channel_osd_preview_proto;
// Override for the HDZ band used when formatting a channel preview (only
// consulted when previewing an HDZ channel). 0xFF = use g_setting.source
// .hdzero_band; 0 = force Raceband naming; 1 = force Lowband naming. Lets the
// BoxPro dial preview the correct R*/L* name when walking across both bands
// before the band is committed.
extern uint8_t channel_osd_preview_band;

int osd_init(void);
int osd_clear(void);
void osd_fhd(uint8_t);
void osd_signal_update();
void osd_hdzero_update(void);
void osd_rec_update(bool enable);
void osd_show(bool show);
void osd_update_element_positions();
char *channel2str(uint8_t is_hdzero, uint8_t is_lowband, uint8_t channel);
// Returns a channel label tagged with the protocol that produced it, e.g.
// "R1·HDZ" or "F4·ANA". The `protocol` parameter takes scan_protocol_t
// values (1 = PROTOCOL_HDZ, 2 = PROTOCOL_ANALOG); other values yield "----".
// Declared as `int` (not `scan_protocol_t`) so osd.h does not need to include
// scan_core.h.
char *channel2str_tagged(int protocol, uint8_t channel_index);
// Behind-the-OSD black mask that hides the green/black flash when
// HDZero_open() resets the baseband on an Auto-BW bandwidth switch. With
// detecting=true the channel OSD element shows a "Detecting..." tag and dial
// channel selection is disabled until the cover drops. Created once on the
// main thread via osd_cover_create() (called from osd_init).
void osd_cover_create(void);
void osd_cover(bool on, bool detecting);
// True while an auto-detect probe is in flight (cover up with detecting=true).
bool osd_is_detecting(void);
void load_fc_osd_font(uint8_t);
void *thread_osd(void *ptr);
void osd_resource_path(char *buf, const char *fmt, osd_resource_t osd_resource_type, ...);
void osd_toggle();
void osd_analog_rssi_update_location();
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
// Editor preview only: while the OSD element position UI is open, selects
// whether osd_show_all_elements() previews the analog-source elements (RSSI
// bar) or the HDZero-only ones. Set by ui_osd_element_pos.
extern bool osd_element_preview_analog;
#endif
#ifdef __cplusplus
}
#endif
