#include "battery.h"

#include "core/settings.h"
#include "driver/mcp3021.h"
#include "ui/page_common.h"
#include <stdio.h>

sys_battery_t g_battery;

// A full high-voltage cell. A standard cell charges to 4.20 V.
#define CELL_FULL_MV 4350

int battery_cells_for_millivolts(int mv) {
    // The fewest cells that put the pack at or below a full cell each.
    const int cells = mv > 0 ? (mv + CELL_FULL_MV - 1) / CELL_FULL_MV : 0;
    if (cells < CELL_MIN_COUNT)
        return CELL_MIN_COUNT;
    if (cells > CELL_MAX_COUNT)
        return CELL_MAX_COUNT;
    return cells;
}

void battery_init() {
    switch (g_setting.power.cell_count_mode) {
    default:
    case SETTING_POWER_CELL_COUNT_MODE_AUTO:
        g_battery.type = battery_cells_for_millivolts(read_voltage());
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
        return BATTERY_WARN_CRITICAL;
    }

    int cell = battery_get_millivolts(true);
    int bottom = g_setting.power.voltage;
    int top = g_setting.power.warning_voltage_gradual;

    // At or below the Warning Cell Voltage is the critical tier (worst).
    if (cell <= bottom) {
        return BATTERY_WARN_CRITICAL;
    }
    // Above the gradual range (or a degenerate range) is no warning. The
    // top <= bottom guard also keeps `third` below from being <= 0.
    if (cell > top || top <= bottom) {
        return BATTERY_WARN_NONE;
    }

    // Split (bottom, top] into equal thirds: bottom third = rapid, middle =
    // slow, top third = subtle. cell == top yields SUBTLE (top is where the
    // gradual warning begins).
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
