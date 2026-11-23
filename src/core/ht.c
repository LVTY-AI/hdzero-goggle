#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if !defined(__linux__)
#include "platform/timer_compat.h" // POSIX interval-timer shim (macOS/Windows)
#endif
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "MadgwickAHRS.h"
#include "common.hh"
#include "elrs.h"
#include "ht.h"
#include "ht_alarm.h"
#include "osd.h"

#include "bmi270/accel_gyro.h"
#include "core/settings.h"
#include "core/sleep_mode.h"
#include "driver/beep.h"
#include "driver/dm5680.h"
#include "driver/hardware.h"
#include "driver/screen.h"
#include "ui/page_common.h"
#include "util/math.h"

// #define FAST_SIM
typedef enum {
    OLED_MD_DETECTING, // regular operation, oled on
    OLED_MD_PRE_OFF,   // pre-sleep, reduced brightness
    OLED_MD_OFF        // sleep, oled off
} oled_motion_detect_state_t;

///////////////////////////////////////////////////////////////////////////////
// local
static ht_data_t ht_data;
static const uint8_t frame_period = 10;
static const uint8_t sync_len = 200;

static bool has_motion_data = false;
static bool is_moving = true;

static volatile bool calibrating = false;
static int calibration_count = 0;

// Runtime gyro-bias tracking. The one-shot calibration stores an integer-LSB
// offset (gyr_offset); at +-2000 dps that floor leaves up to ~1.8 deg/min of
// residual yaw bias, which the 6-axis fusion cannot correct (no heading
// reference) and which integrates into pan drift. gyr_bias_dyn is a float,
// sensor-frame correction that is continuously re-estimated while the goggles
// are held still, nulling that residual. Frozen while moving so real head
// motion is never absorbed. Operates upstream of rotate()/imu_orientation, so
// it is platform-agnostic (helps Goggle, Goggle2 and BoxPro alike).
static float gyr_bias_dyn[3] = {0.0f, 0.0f, 0.0f}; // float LSB correction, sensor frame
static int gyr_still_cnt = 0;                       // consecutive stationary ticks

#define GYR_STILL_DPS    1.5f                          // per-axis "stationary" threshold (deg/s)
#define GYR_STILL_TICKS  (AHRS_UPDATE_FREQUENCY / 2)   // must be still this long before tracking (0.5s)
#define GYR_BIAS_TC_SEC  20                            // bias IIR time constant (seconds)
#define GYR_BIAS_K       (1.0f / (GYR_BIAS_TC_SEC * AHRS_UPDATE_FREQUENCY))

// Intrinsic Tait-Bryan body-frame rotation (X -> Y -> Z, see util/math.c::rotate)
// applied identically to gyro and accel before Madgwick fusion. Aligns IMU
// axes with the device frame so pan/tilt/roll are independent at Madgwick output.
#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
// 23 deg accounts for the goggle visor cant.
static const float imu_orientation[3] = {0.0 * DEG_TO_RAD, -90.0 * DEG_TO_RAD, (-90.0 + 23.0) * DEG_TO_RAD};
#elif defined(HDZBOXPRO)
// BoxPro IMU is mounted at ~30 deg around device-X (chip not horizontal).
// Determined empirically; do not change without re-tuning on hardware.
static const float imu_orientation[3] = {-30.0 * DEG_TO_RAD, -90.0 * DEG_TO_RAD, -67.0 * DEG_TO_RAD};
#endif

static const int ppmMaxPulse = 500;
static const int ppmMinPulse = -500;
static const int ppmCenter = 1500;

static pthread_t head_alarm_handle;

static void calculate_orientation();
static void *head_alarm_thread(void *arg);

///////////////////////////////////////////////////////////////////////////////
// Use the input routed to the panel, including in live settings previews.
// Other inputs may still have cached lock flags while the menu is showing.
static bool panel_has_live_video(void) {
    switch (g_hw_stat.source_mode) {
    case SOURCE_MODE_HDZERO:
        return rx_status[0].rx_valid || rx_status[1].rx_valid;
    case SOURCE_MODE_AV:
        return g_hw_stat.av_valid[g_hw_stat.is_av_in] != 0;
    case SOURCE_MODE_HDMIIN:
        return g_hw_stat.hdmiin_valid != 0;
    case SOURCE_MODE_UI:
        return false;
    }
    return true; // An unrecognized video path is not safe to blank.
}

static void detect_motion(bool is_moving) {
    static oled_motion_detect_state_t state = OLED_MD_DETECTING;
    static int cnt = 0;

    const bool keep_awake = g_setting.image.auto_off == 4 || panel_has_live_video();
    if (keep_awake || is_moving) {
        // Debounce motion-only wake; a returning picture must wake immediately.
        if (state == OLED_MD_OFF && !keep_awake && ++cnt < 2)
            return;
        if (state != OLED_MD_DETECTING)
            screen.brightness(g_setting.image.oled);
        if (state == OLED_MD_OFF)
            screen_set_inhibited(SCREEN_INHIBIT_IDLE, false);
        state = OLED_MD_DETECTING;
        cnt = 0;
        return;
    }

    switch (state) {
    case OLED_MD_DETECTING:
        cnt++;

#ifdef FAST_SIM
        if (cnt > (MOTION_DUR_1MINUTE * (g_setting.image.auto_off + 1))) {
#else
        if (cnt > (MOTION_DUR_1MINUTE * (g_setting.image.auto_off * 2 + 1))) {
#endif
            LOGI("OLED pre-OFF for protection.");
            screen.brightness(0);
            state = OLED_MD_PRE_OFF;
            cnt = 0;
        }
#ifdef FAST_SIM
        LOGI("IDLE %d", cnt);
#endif
        break;

    case OLED_MD_PRE_OFF:
#ifdef FAST_SIM
        LOGI("PRE OFF %d", cnt);
#endif
        cnt++;

        if (cnt == MOTION_DUR_1MINUTE) { // 1-min
            LOGI("OLED OFF for protection.");
            beep();
            // Blank only the panel. The tuned receiver must keep listening
            // so a returning signal can wake it without head movement.
            screen_set_inhibited(SCREEN_INHIBIT_IDLE, true);
            state = OLED_MD_OFF;
            cnt = 0;
        }
        break;

    case OLED_MD_OFF:
#ifdef FAST_SIM
        LOGI("OFF %d", cnt);
#endif
        cnt = 0;
        break;

    default:
        // wat?
        state = OLED_MD_DETECTING;
        break;
    }
}

void ht_detect_motion() {
    if (!isSleeping && has_motion_data) {
        detect_motion(is_moving);
        has_motion_data = false;
    }
}

static void get_imu_data() {
    static int dec_cnt = 0;

#ifndef EMULATOR_BUILD
    get_bmi270(&ht_data.sensor_data);
#endif

    dec_cnt++;
    if (dec_cnt != AHRS_UPDATE_FREQUENCY)
        return; // calibrate dec_cnt to make sure the following code runs at 1Hz
    dec_cnt = 0;

    static struct bmi2_sens_axes_data gyr_last;

    const int16_t dx = ht_data.sensor_data.gyr.x - gyr_last.x;
    const int16_t dy = ht_data.sensor_data.gyr.y - gyr_last.y;
    const int16_t dz = ht_data.sensor_data.gyr.z - gyr_last.z;
    const uint32_t diff = dx * dx + dy * dy + dz * dz;

    is_moving = (diff > MOTION_GYRO_THR) || g_key > 0;
    // LOGD("diff: %d g_key: %d is_moving: %d", diff, g_key, is_moving);

    g_key = 0;
    gyr_last = ht_data.sensor_data.gyr;

    has_motion_data = true;
}

static void timer_callback_imu(union sigval timer_data) {
    get_imu_data();
    calculate_orientation();
}

/////////////////////////////////////////////////////////////////////////////////
// HT function
void ht_init() {
    ht_data.tiltAngle = 0;
    ht_data.rollAngle = 0;
    ht_data.panAngle = 0;

#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    ht_data.tiltInverse = 1;
    ht_data.rollInverse = -1;
    ht_data.panInverse = -1;
#elif defined HDZBOXPRO
    ht_data.tiltInverse = -1;
    ht_data.rollInverse = -1;
    ht_data.panInverse = -1;
#endif

    ht_data.htChannels[0] = 0;
    ht_data.htChannels[1] = 0;
    ht_data.htChannels[2] = 0;

    ht_data.enable = 0;
    ht_set_maxangle(g_setting.ht.max_angle);
    ht_data.acc_offset[0] = g_setting.ht.acc_x;
    ht_data.acc_offset[1] = g_setting.ht.acc_y;
    ht_data.acc_offset[2] = g_setting.ht.acc_z;
    ht_data.gyr_offset[0] = g_setting.ht.gyr_x;
    ht_data.gyr_offset[1] = g_setting.ht.gyr_y;
    ht_data.gyr_offset[2] = g_setting.ht.gyr_z;

    // start timer
    timer_t timerId = 0;
    struct sigevent sev = {0};
    struct itimerspec its = {.it_value.tv_sec = 1,
                             .it_value.tv_nsec = 0,
                             .it_interval.tv_sec = 0,
                             .it_interval.tv_nsec = 1000000000 / AHRS_UPDATE_FREQUENCY};

    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = &timer_callback_imu;

    int res = timer_create(CLOCK_REALTIME, &sev, &timerId);
    if (res != 0) {
        LOGE("Error timer_create: %s\n", strerror(errno));
        return;
    }

    res = timer_settime(timerId, 0, &its, NULL);
    if (res != 0) {
        LOGE("Error timer_settime: %s\n", strerror(errno));
    }
}

void ht_set_maxangle(int angle) {
    ht_data.tiltFactor = 1000.0 / angle;
    ht_data.rollFactor = 1000.0 / angle;
    ht_data.panFactor = 1000.0 / angle;
}

void ht_set_alarm_angle() {
    g_setting.ht.alarm_angle = ht_get_tilt_angle();
    ini_putl("ht", "alarm_angle", g_setting.ht.alarm_angle, SETTING_INI);
}

static void calc_gyr(float *gyrAngle) // in degree
{
    // convert gyro readings to degrees/sec (calibration offset + runtime bias)
    gyrAngle[0] = gyr_to_dps_f((float)ht_data.sensor_data.gyr.x - ht_data.gyr_offset[0] - gyr_bias_dyn[0]);
    gyrAngle[1] = gyr_to_dps_f((float)ht_data.sensor_data.gyr.y - ht_data.gyr_offset[1] - gyr_bias_dyn[1]);
    gyrAngle[2] = gyr_to_dps_f((float)ht_data.sensor_data.gyr.z - ht_data.gyr_offset[2] - gyr_bias_dyn[2]);
    rotate(gyrAngle, imu_orientation);
}

static void calc_acc(float *accAngle) // in G
{
    // convert accelerometer readings to G forces
    accAngle[0] = acc_to_g(ht_data.sensor_data.acc.x);
    accAngle[1] = acc_to_g(ht_data.sensor_data.acc.y);
    accAngle[2] = acc_to_g(ht_data.sensor_data.acc.z);
    rotate(accAngle, imu_orientation);
}

void ht_calibrate() {
    LOGI("HT calibration...");
    ht_data.acc_offset[0] = ht_data.acc_offset[1] = ht_data.acc_offset[2] = 0;
    ht_data.gyr_offset[0] = ht_data.gyr_offset[1] = ht_data.gyr_offset[2] = 0;

    // Fresh baseline: drop the runtime bias correction so it re-converges from 0.
    gyr_bias_dyn[0] = gyr_bias_dyn[1] = gyr_bias_dyn[2] = 0.0f;
    gyr_still_cnt = 0;

    calibration_count = 0;
    calibrating = true;
    // Check if finished calibrating
    while (calibrating) {
        usleep(100000);
    }
    ht_data.acc_offset[0] >>= CALIBRATION_BCNT;
    ht_data.acc_offset[1] >>= CALIBRATION_BCNT;
    ht_data.acc_offset[2] >>= CALIBRATION_BCNT;
    ht_data.gyr_offset[0] >>= CALIBRATION_BCNT;
    ht_data.gyr_offset[1] >>= CALIBRATION_BCNT;
    ht_data.gyr_offset[2] >>= CALIBRATION_BCNT;

    ini_putl("ht", "acc_x", ht_data.acc_offset[0], SETTING_INI);
    ini_putl("ht", "acc_y", ht_data.acc_offset[1], SETTING_INI);
    ini_putl("ht", "acc_z", ht_data.acc_offset[2], SETTING_INI);
    ini_putl("ht", "gyr_x", ht_data.gyr_offset[0], SETTING_INI);
    ini_putl("ht", "gyr_y", ht_data.gyr_offset[1], SETTING_INI);
    ini_putl("ht", "gyr_z", ht_data.gyr_offset[2], SETTING_INI);

    LOGI("done!");
}

// Re-estimate the gyro bias while the goggles are held still. corr[] is the
// current bias-corrected gyro reading (LSB, sensor frame); when the device is
// stationary that residual is the leftover bias, so a slow IIR pulls
// gyr_bias_dyn toward it and the corrected rate trends to zero. Frozen while
// moving so genuine head motion is never absorbed.
static void update_gyro_bias() {
    float corr[3], dps[3];
    corr[0] = (float)ht_data.sensor_data.gyr.x - ht_data.gyr_offset[0] - gyr_bias_dyn[0];
    corr[1] = (float)ht_data.sensor_data.gyr.y - ht_data.gyr_offset[1] - gyr_bias_dyn[1];
    corr[2] = (float)ht_data.sensor_data.gyr.z - ht_data.gyr_offset[2] - gyr_bias_dyn[2];
    dps[0] = gyr_to_dps_f(corr[0]);
    dps[1] = gyr_to_dps_f(corr[1]);
    dps[2] = gyr_to_dps_f(corr[2]);

    bool still = fabsf(dps[0]) < GYR_STILL_DPS &&
                 fabsf(dps[1]) < GYR_STILL_DPS &&
                 fabsf(dps[2]) < GYR_STILL_DPS;

    if (!still) {
        gyr_still_cnt = 0;
        return;
    }

    if (gyr_still_cnt < GYR_STILL_TICKS) {
        gyr_still_cnt++;
        return;
    }

    gyr_bias_dyn[0] += GYR_BIAS_K * corr[0];
    gyr_bias_dyn[1] += GYR_BIAS_K * corr[1];
    gyr_bias_dyn[2] += GYR_BIAS_K * corr[2];
}

static void calculate_orientation() {
    float gyrAngle[3], accAngle[3];
    float tmp;

    if (!calibrating && !ht_data.enable)
        return;

    if (calibrating) {
        ht_data.acc_offset[0] += ht_data.sensor_data.acc.x;
        ht_data.acc_offset[1] += ht_data.sensor_data.acc.y;
        ht_data.acc_offset[2] += ht_data.sensor_data.acc.z;
        ht_data.gyr_offset[0] += ht_data.sensor_data.gyr.x;
        ht_data.gyr_offset[1] += ht_data.sensor_data.gyr.y;
        ht_data.gyr_offset[2] += ht_data.sensor_data.gyr.z;
        calibration_count++;
        if (calibration_count == 1 << CALIBRATION_BCNT)
            calibrating = false;
    } else {
        // Not while calibrating: gyr_offset holds a running sum mid-calibration.
        update_gyro_bias();
    }

    calc_gyr(gyrAngle);
    calc_acc(accAngle);

    MadgwickAHRSupdateIMU(gyrAngle[0] * DEG_TO_RAD, gyrAngle[1] * DEG_TO_RAD, gyrAngle[2] * DEG_TO_RAD,
                          accAngle[0], accAngle[1], accAngle[2]);

    // Adjust PTR relatice to user specified home position
    ht_data.panAngle = getYaw() - ht_data.panAngleHome;
    ht_data.tiltAngle = getPitch() - ht_data.tiltAngleHome;
    ht_data.rollAngle = getRoll() - ht_data.rollAngleHome;

    // One-time auto-center after startup. The home offsets boot as 0, so
    // until the user re-centers, the angles are absolute fusion output; on
    // mountings whose worn pose sits far from the fusion's zero (BoxPro
    // rests at roll ~-80 deg) every axis starts pegged. Wait out the
    // Madgwick convergence from the boot quaternion (beta 0.1 closes ~80
    // deg in ~15 s), then adopt the current pose as center once.
    static int settle_cnt = 0;
    if (settle_cnt >= 0 && ++settle_cnt == AHRS_UPDATE_FREQUENCY * 20) {
        ht_set_center_position();
        settle_cnt = -1;
    }

#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    tmp = normalize(ht_data.panAngle, -180.0, 180.0) * ht_data.panInverse * ht_data.panFactor + 0.5;
    ht_data.htChannels[0] = constrain(tmp, ppmMinPulse, ppmMaxPulse) + ppmCenter;
    tmp = normalize(ht_data.tiltAngle, -180.0, 180.0) * ht_data.tiltInverse * ht_data.tiltFactor + 0.5;
    ht_data.htChannels[1] = constrain(tmp, ppmMinPulse, ppmMaxPulse) + ppmCenter;
    tmp = normalize(ht_data.rollAngle, -180.0, 180.0) * ht_data.rollInverse * ht_data.rollFactor + 0.5;
    ht_data.htChannels[2] = constrain(tmp, ppmMinPulse, ppmMaxPulse) + ppmCenter;
#elif defined HDZBOXPRO
    tmp = normalize(ht_data.panAngle, -180.0, 180.0) * ht_data.panInverse * ht_data.panFactor + 0.5;
    ht_data.htChannels[0] = constrain(tmp, ppmMinPulse, ppmMaxPulse) + ppmCenter;
    tmp = normalize(ht_data.tiltAngle, -180.0, 180.0) * ht_data.tiltInverse * ht_data.tiltFactor + 0.5;
    ht_data.htChannels[2] = constrain(tmp, ppmMinPulse, ppmMaxPulse) + ppmCenter;
    tmp = normalize(ht_data.rollAngle, -180.0, 180.0) * ht_data.rollInverse * ht_data.rollFactor + 0.5;
    ht_data.htChannels[1] = constrain(tmp, ppmMinPulse, ppmMaxPulse) + ppmCenter;
#endif
    Set_HT_dat(ht_data.htChannels[0], ht_data.htChannels[1], ht_data.htChannels[2]);

    if (elrs_headtracking_enabled()) {
        uint16_t ptrCRSF[3];
        ptrCRSF[0] = fmap(ht_data.htChannels[0], ppmMinPulse + ppmCenter, ppmMaxPulse + ppmCenter, 191.0, 1792.0) + 0.5;
        ptrCRSF[1] = fmap(ht_data.htChannels[1], ppmMinPulse + ppmCenter, ppmMaxPulse + ppmCenter, 191.0, 1792.0) + 0.5;
        ptrCRSF[2] = fmap(ht_data.htChannels[2], ppmMinPulse + ppmCenter, ppmMaxPulse + ppmCenter, 191.0, 1792.0) + 0.5;
        msp_ht_update(ptrCRSF[0], ptrCRSF[1], ptrCRSF[2]);
    }
}

void ht_set_center_position() {
    ht_data.panAngleHome += ht_data.panAngle;
    ht_data.tiltAngleHome += ht_data.tiltAngle;
    ht_data.rollAngleHome += ht_data.rollAngle;
}

void ht_enable() {
    ht_data.enable = 1;
    Set_HT_status(ht_data.enable, frame_period, sync_len);
}

void ht_disable() {
    ht_data.enable = 0;
    Set_HT_status(ht_data.enable, frame_period, sync_len);
}

int16_t *ht_get_channels() {
    return ht_data.htChannels;
}

void head_alarm_init() {
    pthread_create(&head_alarm_handle, NULL, head_alarm_thread, NULL);
}

float ht_get_pan_angle(void) {
    return ht_data.panAngle;
}

float ht_get_tilt_angle(void) {
#if defined(HDZBOXPRO)
    return normalize(-ht_data.rollAngle, -180.0, 180.0);
#else
    return ht_data.tiltAngle;
#endif
}

void *head_alarm_thread(void *arg) {
    while (1) {
        bool sounding_alarm = false;
        if (ht_data.enable && (g_setting.ht.alarm_state != SETTING_HT_ALARM_STATE_OFF) && ht_alarm_angle_is_set(g_setting.ht.alarm_angle)) {                                                          // user settings
            if ((g_setting.ht.alarm_on_arm && g_setting.ht.alarm_state == SETTING_HT_ALARM_STATE_ARM) || (g_setting.ht.alarm_on_video && g_setting.ht.alarm_state == SETTING_HT_ALARM_STATE_VIDEO)) { // system enabling alarm (when armed or has video signal)

                if (ht_get_tilt_angle() < g_setting.ht.alarm_angle) {
                    beep();
                    usleep(100000);
                    beep();
                    for (int i = 0; i < 30; i++) { // delay of 3 seconds split into 30x100ms to allow for a quicker response to the alarm
                        if (ht_get_tilt_angle() >= g_setting.ht.alarm_angle) {
                            break;
                        }
                        usleep(100000);
                    }
                }

                usleep(150000); // prevent resource occupation (when armed or video)
            } else {
                usleep(250000); // prevent resource occupation (when !armed and/or !video)
            }
        } else {
            sleep(3); // prevent resource occupation (when ht and/or alarm is disabled)
        }
    }
    pthread_exit(NULL);
}
