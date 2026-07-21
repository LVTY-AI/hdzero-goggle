#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "msp_displayport.h"

typedef enum {
    DVR_TOGGLE,
    DVR_STOP,
    DVR_START,
} osd_dvr_cmd_t;

extern bool dvr_is_recording;
extern bool record_pending;

void dvr_update_status();
void dvr_select_audio_source(uint8_t audio_source);
void dvr_set_dvr_audio_volume(int volume);
void dvr_set_live_audio_volume(int volume);
void dvr_set_mic_gain(int gain);
void dvr_set_linein_gain(int gain);
void dvr_mute_live_audio(void);
void dvr_restore_live_audio(void);
void dvr_enable_line_out(bool enable);
bool dvr_live_audio_is_enabled(void);
void dvr_cmd(osd_dvr_cmd_t cmd);
void dvr_update_vi_conf(video_resolution_t fmt);
void dvr_toggle();
void dvr_star();
void dvr_set_race_label(const uint8_t *label, uint16_t len);
void dvr_clear_race_label(void);

#ifdef __cplusplus
}
#endif
