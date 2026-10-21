#!/usr/bin/env python3
"""Compile focused production functions with stubs, without LVGL or hardware."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path, signature):
    source = (ROOT / path).read_text()
    source = re.sub(r"#if defined\(_WIN32\).*?#else\n(.*?)#endif", r"\1", source, flags=re.S)
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define LOGE(...) ((void)0)
#define le16toh(x) (x)
"""

elrs_header = (ROOT / "src/core/elrs.h").read_text()
types = elrs_header[elrs_header.index("typedef enum {"):elrs_header.index("typedef enum {\n    AWAIT_SUCCESS")]
parser = prelude + types + r"""
#define HD_VMAX 18
#define HD_HMAX 50
static mspState_e input_state;
static uint16_t offset;
static uint8_t input_buffer[MSP_PORT_INBUF_SIZE], crc;
static mspPacket_t packet;
static int processed;
static uint16_t elrs_osd[HD_VMAX][HD_HMAX];
static uint16_t elrs_osd_overlay[HD_VMAX][HD_HMAX];
static void msp_process_packet(void) { processed++; }
static void elrs_clear_osd(void) { memset(elrs_osd_overlay, 0, sizeof(elrs_osd_overlay)); }
static void osd_signal_update(void) {}
"""
parser += function("src/core/elrs.c", "uint8_t msp_crc8_dvb_s2(")
parser += function("src/core/elrs.c", "bool esp32_handler_process_byte(")
parser += function("src/core/elrs.c", "static void handle_osd(uint8_t payload[]")
parser += r"""
static void frame_header(unsigned size) {
    uint8_t header[] = {'$', 'X', '<', 0, 0, 0, size & 255, size >> 8};
    input_state = MSP_IDLE;
    for (unsigned i = 0; i < sizeof(header); i++) esp32_handler_process_byte(header[i]);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "parser")) {
        frame_header(65); assert(input_state == MSP_IDLE);
        frame_header(65535); assert(input_state == MSP_IDLE);
        for (unsigned size = 0; size <= 64; size += 64) {
            frame_header(size);
            for (unsigned i = 0; i < size; i++) esp32_handler_process_byte(1);
            assert(input_state == MSP_CHECKSUM_V2_NATIVE);
            esp32_handler_process_byte(crc);
        }
        assert(processed == 2);
    } else {
        uint8_t empty[] = {3};
        handle_osd(empty, 0);
        for (unsigned size = 1; size < 4; size++) {
            uint8_t *short_payload = malloc(size);
            memset(short_payload, 3, size);
            handle_osd(short_payload, size);
            free(short_payload);
        }
        uint8_t invalid_row[] = {3, HD_VMAX, 0, 0, 'A'};
        uint8_t invalid_col[] = {3, 0, HD_HMAX, 0, 'A'};
        uint8_t large_coords[] = {3, 255, 255, 0, 'A'};
        handle_osd(invalid_row, sizeof(invalid_row));
        handle_osd(invalid_col, sizeof(invalid_col));
        handle_osd(large_coords, sizeof(large_coords));
        for (unsigned row = 0; row < HD_VMAX; row++)
            for (unsigned col = 0; col < HD_HMAX; col++) assert(elrs_osd_overlay[row][col] == 0);
        uint8_t valid[] = {3, HD_VMAX - 1, HD_HMAX - 1, 1, 'A', 'B'};
        handle_osd(valid, sizeof(valid));
        assert(elrs_osd_overlay[HD_VMAX - 1][HD_HMAX - 1] == 'A' + 256);
    }
    return 0;
}
"""

settings = prelude + r"""
#define SETTING_INI "unused"
#define SETTING_SOURCES_HDZERO_BAND_RACEBAND 0
#define SETTING_SOURCES_HDZERO_BAND_LOWBAND 1
#define BASE_CH_NUM 12
#define ANALOG_CHANNEL_NUM 48
#define HDZERO_CHANNEL_NUM (g_setting.source.hdzero_band ? 8 : BASE_CH_NUM)
struct settings {
    struct { uint8_t channel; } scan;
    struct { uint8_t analog_module, analog_format, analog_auto, analog_ratio;
        uint8_t hdzero_band, hdzero_bw, analog_channel, auto_protocol_detect;
        uint8_t analog_scan_band, scan_mode_initial; } source;
};
static struct settings g_setting;
static const struct settings g_setting_defaults = {.scan.channel = 1, .source.analog_channel = 33};
static long band, scan, analog;
static long ini_getl(const char *section, const char *key, long fallback, const char *path) {
    (void)section; (void)path;
    if (!strcmp(key, "hdzero_band")) return band;
    if (!strcmp(key, "channel")) return scan;
    if (!strcmp(key, "analog_channel")) return analog;
    return fallback;
}
static bool settings_get_bool(const char *section, const char *key, bool fallback) {
    (void)section; (void)key; return fallback;
}
"""
settings_source = (ROOT / "src/core/settings.c").read_text()
if "static long settings_get_long_bounded(" in settings_source:
    settings += function("src/core/settings.c", "static long settings_get_long_bounded(")
load = function("src/core/settings.c", "void settings_load(")
# Exercise the channel-loading statements in their original order.
early_scan = load[load.index("// scan"):load.index("// fans")] if "// scan\n" in load else ""
settings += "static void load_channels(void) {\n" + early_scan
settings += load[load.index("// source"):load.index("// autoscan")] + "}\n"
settings += function("src/core/osd.c", "char *channel2str(")
settings += r"""
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "settings")) {
        long invalid[] = {-1, 0, 49, 257, 65537};
        for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            band = 0; scan = 2; analog = invalid[i]; load_channels();
            assert(g_setting.source.analog_channel == 33);
            assert(g_setting.scan.channel == 2);
        }
        long bad_bands[] = {-1, 2, 256, 257};
        for (unsigned i = 0; i < sizeof(bad_bands) / sizeof(bad_bands[0]); i++) {
            band = bad_bands[i]; scan = 12; analog = 48; load_channels();
            assert(g_setting.source.hdzero_band == 0);
            assert(g_setting.scan.channel == 12);
            assert(g_setting.source.analog_channel == 48);
        }
        long bad_scan[] = {-1, 0, 9, 257, 65537};
        for (unsigned i = 0; i < sizeof(bad_scan) / sizeof(bad_scan[0]); i++) {
            band = 1; scan = bad_scan[i]; analog = 1; load_channels();
            assert(g_setting.scan.channel == 1);
        }
        band = 1; scan = 8; analog = 1; load_channels();
        assert(g_setting.scan.channel == 8 && g_setting.source.analog_channel == 1);
    } else {
        assert(!strcmp(channel2str(0, 0, 0), "--"));
        assert(!strcmp(channel2str(0, 0, 49), "--"));
        assert(!strcmp(channel2str(0, 0, 255), "--"));
        assert(!strcmp(channel2str(0, 0, 1), "A1"));
        assert(!strcmp(channel2str(0, 0, 48), "L8"));
        assert(!strcmp(channel2str(1, 0, 12), "F4"));
        assert(!strcmp(channel2str(1, 1, 8), "L8"));
        assert(!strcmp(channel2str(1, 255, 1), "--"));
    }
    return 0;
}
"""

firmware = prelude + r"""
static int str_compare_versions(const char *a, const char *b) { return strcmp(a, b); }
"""
firmware += function("src/ui/page_version.c", "static const char *page_version_find_latest_fw(" if
                     "static const char *page_version_find_latest_fw(" in (ROOT / "src/ui/page_version.c").read_text()
                     else "static bool page_version_find_latest_fw(")
firmware += r"""
int main(void) {
    assert(mkdir("fwdir", 0700) == 0);
    assert(mkdir("fwdir/1.0.0", 0700) == 0);
    assert(mkdir("fwdir/2.0.0", 0700) == 0);
    char latest[256] = {0};
"""
if "static const char *page_version_find_latest_fw(" in firmware:
    firmware += 'const char *name = page_version_find_latest_fw("fwdir");\nassert(name);\nassert(!strcmp(name, "2.0.0"));\n'
else:
    firmware += r"""
    assert(page_version_find_latest_fw("fwdir", latest, sizeof(latest)));
    assert(!strcmp(latest, "2.0.0"));
    assert(!page_version_find_latest_fw("missing", latest, sizeof(latest)));
"""
firmware += "return 0;\n}\n"

failed = []
with tempfile.TemporaryDirectory(prefix="memory-bounds-") as directory:
    path = Path(directory)
    for name, source, cases in [("elrs", parser, ["parser", "osd"]),
                                ("settings", settings, ["settings", "names"]),
                                ("firmware", firmware, ["firmware"])]:
        cases = [case for case in cases if not sys.argv[1:] or case in sys.argv[1:]]
        if not cases:
            continue
        (path / f"{name}.c").write_text(source)
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-g",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(path / f"{name}.c"), "-o", str(path / name)], check=True)
        for case in cases:
            result = subprocess.run([str(path / name), case], cwd=path, capture_output=True, text=True)
            print(f"{case}: {'PASS' if result.returncode == 0 else 'FAIL'}")
            if result.returncode:
                failed.append(case)
                print(result.stderr)
if failed:
    raise SystemExit("Failed: " + ", ".join(failed))
