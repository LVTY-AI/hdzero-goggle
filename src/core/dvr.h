#pragma once

#ifdef __cplusplus
#include <atomic>
typedef std::atomic<bool> dvr_atomic_bool;
extern "C" {
#else
#include <stdatomic.h>
typedef atomic_bool dvr_atomic_bool;
#endif

#include <stdbool.h>
#include <stdint.h>

#include "msp_displayport.h"

typedef enum {
    DVR_TOGGLE,
    DVR_STOP,
    DVR_START,
} osd_dvr_cmd_t;

extern dvr_atomic_bool dvr_is_recording;
extern dvr_atomic_bool record_pending;

void dvr_update_status();
void dvr_init(void);
void dvr_poll(void);
void dvr_stop_live(void);
// True defers this whole transition until a matched recorder STOP acknowledgment.
// Continuations run on the main UI thread with lvgl_mutex held; NULL means retry by caller.
bool dvr_defer_transition(void (*resume)(uintptr_t), uintptr_t argument);
const char *dvr_transition_status(void);
bool dvr_input_blocked(void);
void dvr_select_audio_source(uint8_t audio_source);
void dvr_set_dvr_audio_volume(int volume);
void dvr_set_live_audio_volume(int volume);
void dvr_set_mic_gain(int gain);
void dvr_set_linein_gain(int gain);
void dvr_mute_live_audio(void);
void dvr_restore_live_audio(void);
void dvr_enable_line_out(bool enable);
bool dvr_live_audio_is_enabled(void);
// True when there is enough room on the card to begin a recording. With rolling
// recording enabled the recorder frees space itself, so a nearly full card is no
// longer a reason to refuse the start.
bool dvr_space_allows_start(void);
void dvr_cmd(osd_dvr_cmd_t cmd);
void dvr_update_vi_conf(video_resolution_t fmt);
void dvr_toggle();
void dvr_star();
void dvr_set_race_label(const uint8_t *label, uint16_t len);
void dvr_clear_race_label(void);

#ifdef __cplusplus
}
#endif
