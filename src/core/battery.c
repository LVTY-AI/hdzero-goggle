#include "battery.h"

#include "core/settings.h"
#include "driver/mcp3021.h"
#include "ui/page_common.h"
#include <stdio.h>

sys_battery_t g_battery;

static int battery_detect_type() {
    int v = read_voltage();
    return (v * 10 / 1000 / 42 + 1);
}

void battery_init() {
    switch (g_setting.power.cell_count_mode) {
    default:
    case SETTING_POWER_CELL_COUNT_MODE_AUTO:
        g_battery.type = battery_detect_type();
        if (g_battery.type < CELL_MIN_COUNT)
            g_battery.type = CELL_MIN_COUNT;
        g_setting.power.cell_count = g_battery.type;
        break;
    case SETTING_POWER_CELL_COUNT_MODE_MANUAL:
        g_battery.type = g_setting.power.cell_count;
        break;
    }
}

void battery_update() {
    g_battery.voltage = read_voltage();
}

bool battery_is_low() {
    if (g_battery.type == 0) {
        return true;
    }
    int cell_volt = battery_get_millivolts(true);
    return cell_volt <= g_setting.power.voltage;
}

battery_warn_level_t battery_warn_level(void) {
    if (g_battery.type == 0) {
        return BATTERY_WARN_RAPID;
    }

    int cell = battery_get_millivolts(true);
    int bottom = g_setting.power.voltage;
    int top = g_setting.power.warning_voltage_gradual;

    if (cell > top) {
        return BATTERY_WARN_NONE;
    }
    if (top <= bottom || cell <= bottom) {
        return BATTERY_WARN_RAPID;
    }

    // Split (bottom, top] into equal thirds. cell == top yields SUBTLE (top is
    // where the gradual warning begins). With the 10 mV-stepped UI the range is
    // always >= 10 mV, so third >= 3; if a hand-edited range made third == 0 the
    // SLOW/RAPID tiers collapse to SUBTLE, which is acceptable (no crash/UB).
    int third = (top - bottom) / 3;
    if (cell <= bottom + third) {
        return BATTERY_WARN_RAPID;
    }
    if (cell <= bottom + 2 * third) {
        return BATTERY_WARN_SLOW;
    }
    return BATTERY_WARN_SUBTLE;
}

int battery_get_millivolts(bool per_cell) {
    if (per_cell && g_battery.type > 0) {
        return (g_battery.voltage + g_battery.offset) / g_battery.type;
    }
    return g_battery.voltage + g_battery.offset;
}

void battery_get_voltage_str(char *buf) {
    switch (g_setting.power.osd_display_mode) {

    default:
    case SETTING_POWER_OSD_DISPLAY_MODE_TOTAL: {
        int bat_mv = battery_get_millivolts(false);
        sprintf(buf, "%dS %d.%02dV",
                g_battery.type,
                bat_mv / 1000,
                bat_mv % 1000 / 10);
        break;
    }

    case SETTING_POWER_OSD_DISPLAY_MODE_CELL: {
        int bat_mv = battery_get_millivolts(true);
        sprintf(buf, "%dS %d.%02dV/C",
                g_battery.type,
                bat_mv / 1000,
                bat_mv % 1000 / 10);
        break;
    }
    }
}
