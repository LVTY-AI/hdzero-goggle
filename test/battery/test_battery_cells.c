#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/battery.h"
#include "core/settings.h"

setting_t g_setting;

static int pack_mv;
int read_voltage() { return pack_mv; }

static int failures;

static void check_cells(int mv, int want, const char *pack) {
    const int got = battery_cells_for_millivolts(mv);
    if (got != want) {
        fprintf(stderr, "FAIL: %s at %d mV reads %dS, want %dS\n", pack, mv, got, want);
        failures++;
    }
}

int main(void) {
    // Full packs read right, standard (4.20 V a cell) and high voltage (4.35 V).
    check_cells(8400, 2, "a full 2S");
    check_cells(8700, 2, "a full 2S HV");
    check_cells(12600, 3, "a full 3S");
    check_cells(13050, 3, "a full 3S HV");
    check_cells(16800, 4, "a full 4S");
    check_cells(17400, 4, "a full 4S HV");
    check_cells(21000, 5, "a full 5S");
    check_cells(21750, 5, "a full 5S HV");
    check_cells(25200, 6, "a full 6S");
    check_cells(26100, 6, "a full 6S HV");

    // Storage charge, 3.80 V a cell.
    check_cells(7600, 2, "a 2S at storage");
    check_cells(11400, 3, "a 3S at storage");
    check_cells(15200, 4, "a 4S at storage");
    check_cells(22800, 6, "a 6S at storage");

    // The flat end: a pack reads one cell short at or below 4.35 V times one
    // cell fewer, 3.63 V a cell for a 6S and 3.26 V for a 4S.
    check_cells(21750, 5, "a 6S at 3.625 V a cell");
    check_cells(21760, 6, "a 6S just above 3.625 V a cell");
    check_cells(13050, 3, "a 4S at 3.26 V a cell");
    check_cells(13060, 4, "a 4S just above 3.26 V a cell");

    // No pack, a negative reading and a reading past six cells stay in range.
    check_cells(0, CELL_MIN_COUNT, "no reading");
    check_cells(-300, CELL_MIN_COUNT, "a negative reading");
    check_cells(30000, CELL_MAX_COUNT, "a reading past six cells");

    // Auto sets both the count in use and the saved count.
    g_setting.power.cell_count_mode = SETTING_POWER_CELL_COUNT_MODE_AUTO;
    g_setting.power.cell_count = 6;
    pack_mv = 8400;
    battery_init();
    if (g_battery.type != 2 || g_setting.power.cell_count != 2) {
        fprintf(stderr, "FAIL: Auto with a full 2S counts %dS (saved %dS)\n", g_battery.type,
                g_setting.power.cell_count);
        failures++;
    }

    // Manual keeps the saved count whatever the pack reads.
    g_setting.power.cell_count_mode = SETTING_POWER_CELL_COUNT_MODE_MANUAL;
    g_setting.power.cell_count = 4;
    pack_mv = 8400;
    battery_init();
    if (g_battery.type != 4) {
        fprintf(stderr, "FAIL: Manual 4S counts %dS\n", g_battery.type);
        failures++;
    }

    if (failures)
        return 1;
    puts("test_battery_cells: all checks passed");
    return 0;
}
