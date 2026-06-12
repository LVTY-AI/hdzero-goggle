#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl/lvgl.h>

#include "ui/ui_main_menu.h"

extern page_pack_t pp_source;

void source_status_timer();
void source_toggle();
void source_cycle();
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
// Probes both protocols at the current channel and enters video on whichever
// has signal (defaults to HDZero). Used by the Source page pick and by the
// boot-time Auto Scan when its source is set to Auto Detect.
void page_source_select_auto_detect();
#endif

#ifdef __cplusplus
}
#endif
