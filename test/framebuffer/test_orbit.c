#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

// Exercise the real static flush callback without an fb device or LVGL loop.
#include "../../src/ui/ui_porting.c"

setting_t g_setting;
static size_t expected_length;
void fb_sync(PFBDEV fb) {
    assert(fb->fb_mem_offset == 64);
    assert(fb->fb_fix.smem_len == expected_length);
}
void lv_disp_flush_ready(lv_disp_drv_t *driver) {}

static void check_size(int width, int height) {
    const int sw = width + DISP_OVERSCAN, sh = height + DISP_OVERSCAN;
    lv_color_t *source = malloc((size_t)sw * sh * sizeof(*source));
    const size_t visible_bytes = (size_t)width * height * sizeof(*source);
    expected_length = visible_bytes + 128;
    uint8_t *storage = malloc(expected_length);
    assert(source && storage);
    fbdev.fb_mem = storage;
    fbdev.fb_mem_offset = 64;
    fbdev.fb_fix.smem_len = expected_length;
    disp_drv.hor_res = sw;
    disp_drv.ver_res = sh;
    const lv_area_t area = {0, 0, sw - 1, sh - 1};

    for (int level = 0; level <= 2; level++) {
        const int max_offset = level ? 1 << level : 0;
        g_setting.osd.orbit = level;
        for (int oy = 0; oy <= max_offset; oy++) {
            for (int ox = 0; ox <= max_offset; ox++) {
                memset(storage, 0xa5, expected_length);
                for (int y = 0; y < sh; y++)
                    for (int x = 0; x < sw; x++)
                        source[y * sw + x].full = 1 + x + y * sw + (ox + oy) * sw * sh;
                disp_orbit_x = ox;
                disp_orbit_y = oy;
                disp_orbit_state = ORBIT_NONE;
                hdz_disp_flush(&disp_drv, &area, source);
                const lv_color_t *actual = (const lv_color_t *)(storage + 64);
                for (int y = 0; y < height; y++)
                    for (int x = 0; x < width; x++)
                        assert(actual[y * width + x].full == source[(y + oy) * sw + x + ox].full);
                for (int p = 0; p < 64; p++) {
                    assert(storage[p] == 0xa5);
                    assert(storage[64 + visible_bytes + p] == 0xa5);
                }
            }
        }
    }
    free(source);
    free(storage);
}

int main(void) {
    _Static_assert(sizeof(lv_color_t) == 4, "hardware framebuffer uses 32-bit pixels");
    check_size(1280, 720);
    check_size(1920, 1080);
    puts("orbit: full replacement, viewport pixels, bounds and mapping metadata passed");
}
