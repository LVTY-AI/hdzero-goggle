#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "defines.h"
#include <stdbool.h>
#include <stdint.h>

#define MIN_SCREEN_BRIGHTNESS 0
#define MAX_SCREEN_BRIGHTNESS 12

typedef struct {
    void (*set720p90)(uint8_t mode);
    void (*set720p60)(uint8_t mode, bool is_43);
    void (*set540p60)(void);
    void (*set1080p30)(void);
    void (*set_ratio)(bool is_43);
} mfpga_t;

typedef struct {
    void (*start_up)(void);
    void (*display)(bool on);
    // Target-specific physical panel operation.  Callers use display(), which
    // is the common request/inhibit coordinator below.
    void (*display_raw)(bool on);
    void (*brightness)(uint8_t level);
    void (*pattern)(bool enable, uint8_t mode, uint8_t speed);
    void (*vtmg)(int mode);
    mfpga_t mfpga;
} screen_t;

extern screen_t screen;

typedef enum {
    SCREEN_INHIBIT_SLEEP = 1u << 0,
    SCREEN_INHIBIT_IDLE = 1u << 1,
    SCREEN_INHIBIT_AUXILIARY = 1u << 2,
} screen_inhibit_reason_t;

// All panel on/off requests pass through this coordinator.  A request can be
// held dark by one or more independent reasons without stopping framebuffer
// rendering or the rest of the application.
void screen_display_request(bool on);
void screen_set_inhibited(uint32_t reasons, bool inhibited);
bool screen_inhibit_active(uint32_t reasons);
bool screen_effective_on(void);

// Force the next screen.vtmg() to reprogram the OLED even if the mode is
// unchanged -- e.g. G1 Auto NTSC/PAL (720p50/720p60 share mode 1), or a fresh
// process whose cache doesn't reflect the OLED's actual last-driven mode.
// Defined in screen-goggle.c (G1) / screen-google2.c (G2).
void screen_vtmg_invalidate(void);

#ifdef __cplusplus
}
#endif
