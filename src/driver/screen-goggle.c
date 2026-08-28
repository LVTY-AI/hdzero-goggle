#include "screen.h"

#if HDZGOGGLE

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <log/log.h>

#include "../core/common.hh"
#include "../core/defines.h"
#include "i2c.h"
#include "msp_displayport.h"
#include "uart.h"

// OLED access
/*
        V536  --m_i2c-->  AL FPGA  --I2C-->  OLED
                                  --RESX 5VEN -5VEN 1V8EN--> OLED

    SPI:
        page: 3bit, addr: 12bit, data: 32bit

    AL FPGA reg:
        reg_a0[1:0]: 1=write cmd; 2=read cmd; self-clear
        reg_a1: addr[7:0]
        reg_a2: addr[15:8]
        reg_a3: wr_data[7:0]
        reg_a4: wr_data[15:8]
        reg_a5: rd_data[7:0]         (read only)
        reg_a6: rd_data[15:8]        (read only)
        reg_a7: unused, reads 0x00   (read only)
        reg_a8: unused, reads 0x00   (read only)

    reg_a5/a6 return the panel selected by the sel field of reg_a0 -- they are
    NOT "the right panel" as this comment used to claim.  a7/a8 read back 0x00
    on this bitstream; there is no separate left-hand read path.  Verified on
    hardware: reading 0xD002 with sel=0 and sel=1 returns the two panels'
    genuinely different factory coefficients through a5/a6.

*/

static void oled_write(uint16_t addr, uint16_t wdat, uint8_t sel) {
    uint8_t val;

    val = addr & 0xFF;
    I2C_Write(ADDR_AL, 0xa1, val);
    val = (addr >> 8) & 0xFF;
    I2C_Write(ADDR_AL, 0xa2, val);

    val = wdat & 0xFF;
    I2C_Write(ADDR_AL, 0xa3, val);
    val = (wdat >> 8) & 0xFF;
    I2C_Write(ADDR_AL, 0xa4, val);

    val = (sel << 4) | 0x01;
    I2C_Write(ADDR_AL, 0xa0, val);

    usleep(250);
}

static uint16_t oled_read(uint16_t addr, uint8_t sel) {
    uint8_t val;
    uint16_t rdat;

    val = addr & 0xFF;
    I2C_Write(ADDR_AL, 0xa1, val);
    val = (addr >> 8) & 0xFF;
    I2C_Write(ADDR_AL, 0xa2, val);

    val = (sel << 4) | 0x02;
    I2C_Write(ADDR_AL, 0xa0, val);

    usleep(500);

    val = I2C_Read(ADDR_AL, 0xa6);
    rdat = val;
    rdat <<= 8;
    val = I2C_Read(ADDR_AL, 0xa5);
    rdat |= val;

    usleep(250);
    return rdat;
}

// The OLED DDIC powers up in temperature-compensation mode 0x05, which does not
// hold the grey axis as the panel warms: the image is neutral on a cold boot and
// drifts green/brown within a few seconds of the panels reaching temperature.
// Mode 0x0A with the factory coefficients biased up by 32 holds neutral across
// the range.
//
// The coefficients only reach the analog path on the next display-on, so this has
// to run before the boot display cycle (Display_UI_init() -> screen.pattern()) --
// which is exactly where screen_start_up() sits in main().  Writing them to a
// panel that is already displaying does nothing until something re-latches it.
//
// Each panel is handled independently: the two carry different factory
// coefficients (0xA1 vs 0xA8 on the bench unit), which is why the drift can show
// on one eye and not the other.  Mode 0x05 is the gate -- a panel that has already
// been adjusted, or a revision reporting some other mode, is left untouched.
static void oled_temp_compensation(void) {
    oled_write(0xF000, 0x00AA, 2);
    oled_write(0xF001, 0x0012, 2);

    for (uint8_t sel = 0; sel < 2; sel++) {
        uint16_t mode = oled_read(0xD000, sel);

        if (mode != 0x05) {
            LOGI("OLED temp comp: panel %d reports mode %#x, left alone", sel, mode);
            continue;
        }

        uint16_t c2 = oled_read(0xD002, sel);
        uint16_t c3 = oled_read(0xD003, sel);
        uint16_t c4 = oled_read(0xD004, sel);

        oled_write(0xD000, 0x000A, sel);
        oled_write(0xD001, 0x000A, sel);
        oled_write(0xD002, c2 + 32, sel);
        oled_write(0xD003, c3 + 32, sel);
        oled_write(0xD004, c4 + 32, sel);

        LOGI("OLED temp comp: panel %d 0x05 -> 0x0A, coeff %#x/%#x/%#x -> %#x/%#x/%#x",
             sel, c2, c3, c4, c2 + 32, c3 + 32, c4 + 32);
    }
}

static void screen_start_up() {
    if (!(I2C_Read(ADDR_AL, 0x00) & 0x01)) {
        LOGW("OLED_Startup failed: Auto init is not ready...");
    }

    I2C_Write(ADDR_AL, 0x10, 0x01);
    // I2C_Write(ADDR_AL, 0x15, 0x00);
    usleep(1000);

    oled_temp_compensation();
}

// OLED display on/off
static void screen_display_raw(bool on) {
    if (on) {
        I2C_Write(ADDR_AL, 0x13, 0x83);
        usleep(1000);
        oled_write(0x8000, 0x0001, 2);
        usleep(5000);
        oled_write(0x2900, 0x0000, 2); // display on
        usleep(20000);
        oled_write(0x5300, 0x0029, 2);
        oled_write(0x5100, 0x00FF, 2);
        oled_write(0x5101, 0x0001, 2);
        oled_write(0x0300, 0x0000, 2);
        usleep(1000);
        I2C_Write(ADDR_AL, 0x13, 0x03);
        LOGI("OLED: Display on");

    } else {
        I2C_Write(ADDR_AL, 0x13, 0x83);
        oled_write(0x2800, 0x0000, 2); // display off
        usleep(20000);
        I2C_Write(ADDR_AL, 0x13, 0x80);
        LOGI("OLED: Display off");
    }
}

// Set OLED to pattern mode
// enable: 0=disable; 1=enable
// mode: 0=color bar; 1=grid; 2=all black; 3=all white; 4=boot screen
// speed: color bar move speed (0~15)
static void screen_pattern(bool enable, uint8_t mode, uint8_t speed) {
    mode = (enable & 0x01) | ((mode & 0x07) << 1) | ((speed & 0x0F) << 4);

    screen.display(0);
    I2C_Write(ADDR_AL, 0x15, mode);
    screen.display(1);
}

// G1 Auto NTSC/PAL: force the next screen_vtmg() to reprogram the OLED even if
// the mode arg is unchanged. 720p50 and 720p60 are both vtmg "mode 1", so an
// analog NTSC<->PAL switch otherwise no-ops below and the OLED keeps its old
// field timing -> tearing. screen_vtmg_invalidate() lets Source_AV()'s
// screen.vtmg(1) re-sync the OLED directly, skipping the 1080p Display_UI() detour.
static int screen_vtmg_last_mode = 0;

void screen_vtmg_invalidate(void) {
    screen_vtmg_last_mode = -1;
}

static void screen_vtmg(int mode) // mode: 0=1080P; 1=720P
{
    if (screen_vtmg_last_mode != mode) {
        screen_vtmg_last_mode = mode;
        switch (mode) {
        case 0:
            I2C_Write(ADDR_AL, 0x33, 0x04);
            oled_write(0x8001, 0x00E0, 2);
            oled_write(0x6900, 0x0000, 2);
            break;
        case 1:
            I2C_Write(ADDR_AL, 0x33, 0x04);
            oled_write(0x8001, 0x0040, 2);
            oled_write(0x6900, 0x0002, 2);
            break;
        case 2:
            I2C_Write(ADDR_AL, 0x33, 0x04);
            I2C_Write(ADDR_AL, 0x16, 0x00);
            oled_write(0x8001, 0x0068, 2);
            oled_write(0x6900, 0x0001, 2);
            break;
        }
        LOGI("OLED: Set to mode %d.", mode);
    }
}

static void MFPGA_SetRatio(bool is_43) {
    if (is_43)
        I2C_Write(ADDR_FPGA, 0x8f, 0x80);
    else
        I2C_Write(ADDR_FPGA, 0x8f, 0x00);
    // LOGI("MFPGA_SetRatio %d",is_43);
}

static void MFPGA_Set720P90(uint8_t mode) {
    I2C_Write(ADDR_FPGA, 0x40, 0xc0);
    I2C_Write(ADDR_FPGA, 0x41, 0x23);
    I2C_Write(ADDR_FPGA, 0x42, 0x1c);
    I2C_Write(ADDR_FPGA, 0x43, 0xaa);
    I2C_Write(ADDR_FPGA, 0x44, 0x45);
    I2C_Write(ADDR_FPGA, 0x45, 0x39);
    I2C_Write(ADDR_FPGA, 0x46, 0x00);
    I2C_Write(ADDR_FPGA, 0x47, 0x00);
    I2C_Write(ADDR_FPGA, 0x48, 0x28);
    I2C_Write(ADDR_FPGA, 0x49, 0xdd);
    I2C_Write(ADDR_FPGA, 0x4a, 0x01);
    I2C_Write(ADDR_FPGA, 0x4b, 0x05);
    I2C_Write(ADDR_FPGA, 0x4c, 0x11);

    if (mode == VR_540P90_CROP) {
        I2C_Write(ADDR_FPGA, 0x4d, 0xE2);
        I2C_Write(ADDR_FPGA, 0x4e, 0x04);
    } else {
        I2C_Write(ADDR_FPGA, 0x4d, 0x30);
        I2C_Write(ADDR_FPGA, 0x4e, 0x05);
    }

    I2C_Write(ADDR_FPGA, 0x4f, 0x00);
    I2C_Write(ADDR_FPGA, 0x52, 0x48);
    I2C_Write(ADDR_FPGA, 0x53, 0x48);
    I2C_Write(ADDR_FPGA, 0x54, 0x66);
    I2C_Write(ADDR_FPGA, 0x61, 0x71);
    I2C_Write(ADDR_FPGA, 0x63, 0x5a);
    I2C_Write(ADDR_FPGA, 0x65, 0x96);
    I2C_Write(ADDR_FPGA, 0x66, 0x00);

    MFPGA_SetRatio(1);
    I2C_Write(ADDR_FPGA, 0x06, 0x0F);
}

static void MFPGA_Set540P60() {
    I2C_Write(ADDR_FPGA, 0x40, 0xc0);
    I2C_Write(ADDR_FPGA, 0x41, 0x23);
    I2C_Write(ADDR_FPGA, 0x42, 0x1c);
    I2C_Write(ADDR_FPGA, 0x43, 0x4B);
    I2C_Write(ADDR_FPGA, 0x44, 0x44);
    I2C_Write(ADDR_FPGA, 0x45, 0x33);
    I2C_Write(ADDR_FPGA, 0x46, 0x00);
    I2C_Write(ADDR_FPGA, 0x47, 0x00);
    I2C_Write(ADDR_FPGA, 0x48, 0x28);
    I2C_Write(ADDR_FPGA, 0x49, 0x84);
    I2C_Write(ADDR_FPGA, 0x4a, 0x00);
    I2C_Write(ADDR_FPGA, 0x4b, 0x05);
    I2C_Write(ADDR_FPGA, 0x4c, 0x11);

    I2C_Write(ADDR_FPGA, 0x4d, 0xd0);
    I2C_Write(ADDR_FPGA, 0x4e, 0x07);

    I2C_Write(ADDR_FPGA, 0x4f, 0x00);
    I2C_Write(ADDR_FPGA, 0x52, 0x48);
    I2C_Write(ADDR_FPGA, 0x53, 0x48);
    I2C_Write(ADDR_FPGA, 0x54, 0x66);
    I2C_Write(ADDR_FPGA, 0x61, 0x71);
    I2C_Write(ADDR_FPGA, 0x63, 0x5a);
    I2C_Write(ADDR_FPGA, 0x65, 0x96);
    I2C_Write(ADDR_FPGA, 0x66, 0x00);

    MFPGA_SetRatio(1);
    I2C_Write(ADDR_FPGA, 0x06, 0x0F);
}

static void MFPGA_Set720P60(uint8_t mode, bool is_43) {
    I2C_Write(ADDR_FPGA, 0x40, 0x00);
    I2C_Write(ADDR_FPGA, 0x41, 0x25);
    I2C_Write(ADDR_FPGA, 0x42, 0xd0);
    I2C_Write(ADDR_FPGA, 0x43, 0x72);
    I2C_Write(ADDR_FPGA, 0x44, 0x46);
    I2C_Write(ADDR_FPGA, 0x45, 0xee);
    I2C_Write(ADDR_FPGA, 0x46, 0x00);
    I2C_Write(ADDR_FPGA, 0x47, 0x00);
    I2C_Write(ADDR_FPGA, 0x48, 0x28);
    I2C_Write(ADDR_FPGA, 0x49, 0xf7);
    I2C_Write(ADDR_FPGA, 0x4a, 0x00);
    I2C_Write(ADDR_FPGA, 0x4b, 0x05);
    I2C_Write(ADDR_FPGA, 0x4c, 0x19);

    if (mode == VR_960x720P60) {
        I2C_Write(ADDR_FPGA, 0x4d, 0xDC);
        I2C_Write(ADDR_FPGA, 0x4e, 0x05);
    } else if (mode == VR_720P50) {
        I2C_Write(ADDR_FPGA, 0x4d, 0xBC);
        I2C_Write(ADDR_FPGA, 0x4e, 0x07);
    } else {
        I2C_Write(ADDR_FPGA, 0x4d, 0x72);
        I2C_Write(ADDR_FPGA, 0x4e, 0x06);
    }

    I2C_Write(ADDR_FPGA, 0x4f, 0x00);
    I2C_Write(ADDR_FPGA, 0x52, 0x5f);
    I2C_Write(ADDR_FPGA, 0x53, 0x5f);
    I2C_Write(ADDR_FPGA, 0x54, 0x88);
    I2C_Write(ADDR_FPGA, 0x61, 0x96);
    I2C_Write(ADDR_FPGA, 0x63, 0x78);
    I2C_Write(ADDR_FPGA, 0x65, 0xc8);
    I2C_Write(ADDR_FPGA, 0x66, 0x00);

    MFPGA_SetRatio(is_43);
    I2C_Write(ADDR_FPGA, 0x06, 0x0F);
}

static void MFPGA_Set1080P30() {
    I2C_Write(ADDR_FPGA, 0x40, 0x80);
    I2C_Write(ADDR_FPGA, 0x41, 0x47);
    I2C_Write(ADDR_FPGA, 0x42, 0x38);
    I2C_Write(ADDR_FPGA, 0x43, 0x9a);
    I2C_Write(ADDR_FPGA, 0x44, 0x88);
    I2C_Write(ADDR_FPGA, 0x45, 0x64);
    I2C_Write(ADDR_FPGA, 0x46, 0x00);
    I2C_Write(ADDR_FPGA, 0x47, 0x00);
    I2C_Write(ADDR_FPGA, 0x48, 0x2c);
    I2C_Write(ADDR_FPGA, 0x49, 0xa9);
    I2C_Write(ADDR_FPGA, 0x4a, 0x00);
    I2C_Write(ADDR_FPGA, 0x4b, 0x05);
    I2C_Write(ADDR_FPGA, 0x4c, 0x28);

    I2C_Write(ADDR_FPGA, 0x4d, 0x98);
    I2C_Write(ADDR_FPGA, 0x4e, 0x08);

    I2C_Write(ADDR_FPGA, 0x4f, 0x00);
    I2C_Write(ADDR_FPGA, 0x52, 0x8f);
    I2C_Write(ADDR_FPGA, 0x53, 0x8f);
    I2C_Write(ADDR_FPGA, 0x54, 0xcc);
    I2C_Write(ADDR_FPGA, 0x61, 0xe1);
    I2C_Write(ADDR_FPGA, 0x63, 0xb4);
    I2C_Write(ADDR_FPGA, 0x65, 0x2c);
    I2C_Write(ADDR_FPGA, 0x66, 0x01);

    MFPGA_SetRatio(0);
    I2C_Write(ADDR_FPGA, 0x06, 0x0F);
}

// OLED power off
static void screen_power_down() {
    I2C_Write(ADDR_AL, 0x13, 0x01);
    usleep(1000);
    oled_write(0x2800, 0x0000, 2); // display off
    usleep(1000);
    oled_write(0x1000, 0x0000, 2); // sleep-in
    usleep(1000);

    I2C_Write(ADDR_AL, 0x11, 0x00); // RESX
    usleep(1000);

    I2C_Write(ADDR_AL, 0x12, 0x03); // AVEE disable
    usleep(2000);
    I2C_Write(ADDR_AL, 0x12, 0x01); // AVDD disable
    usleep(1000);
    I2C_Write(ADDR_AL, 0x12, 0x00); // VDDI disable
}

// OLED brightness setting
static void screen_brightness(uint8_t level) {
    uint16_t dh = 0, dl = 0;

    switch (level) {
    case 12:
        dh = 0x0002;
        dl = 0x00F0;
        break;
    case 11:
        dh = 0x0002;
        dl = 0x00C0;
        break;
    case 10:
        dh = 0x0002;
        dl = 0x0090;
        break;
    case 9:
        dh = 0x0002;
        dl = 0x0060;
        break;
    case 8:
        dh = 0x0002;
        dl = 0x0030;
        break;
    case 7:
        dh = 0x0002;
        dl = 0x0000;
        break;
    case 6:
        dh = 0x0001;
        dl = 0x00D0;
        break;
    case 5:
        dh = 0x0001;
        dl = 0x00A0;
        break;
    case 4:
        dh = 0x0001;
        dl = 0x0070;
        break;
    case 3:
        dh = 0x0001;
        dl = 0x0040;
        break;
    case 2:
        dh = 0x0001;
        dl = 0x0010;
        break;
    case 1:
        dh = 0x0000;
        dl = 0x00E0;
        break;
    case 0:
        dh = 0x0000;
        dl = 0x0020;
        break;
    }

    oled_write(0xF000, 0x00AA, 2);
    oled_write(0xF001, 0x0011, 2);

    oled_write(0xC200, dh, 2);
    oled_write(0xC201, dl, 2);
    oled_write(0xC202, dh, 2);
    oled_write(0xC203, dl, 2);
    oled_write(0xC204, dh, 2);
    oled_write(0xC205, dl, 2);
    oled_write(0xC206, dh, 2);
    oled_write(0xC207, dl, 2);
}

screen_t screen = {
    .start_up = screen_start_up,
    .display = screen_display_request,
    .display_raw = screen_display_raw,
    .brightness = screen_brightness,
    .pattern = screen_pattern,
    .vtmg = screen_vtmg,
    .mfpga = {
        .set720p90 = MFPGA_Set720P90,
        .set720p60 = MFPGA_Set720P60,
        .set540p60 = MFPGA_Set540P60,
        .set1080p30 = MFPGA_Set1080P30,
        .set_ratio = MFPGA_SetRatio,
    },
};

#endif
