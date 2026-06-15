#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define CELL_MIN_COUNT 2
#define CELL_MAX_COUNT 6

typedef struct {
    int type; // cell count
    int voltage;
    int offset; // in mV
} sys_battery_t;

extern sys_battery_t g_battery;

typedef enum {
    BATTERY_WARN_NONE = 0, // no warning
    BATTERY_WARN_SUBTLE,   // amber text, no beep, no icon
    BATTERY_WARN_SLOW,     // red + icon, slow beep
    BATTERY_WARN_RAPID,    // red + icon, rapid beep
} battery_warn_level_t;

void battery_init();
void battery_update();

bool battery_is_low();
battery_warn_level_t battery_warn_level(void);
int battery_get_millivolts(bool per_cell);
void battery_get_voltage_str(char *buf);

#ifdef __cplusplus
}
#endif
