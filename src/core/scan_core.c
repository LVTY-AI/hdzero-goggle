#include "core/scan_core.h"

#if defined(HDZBOXPRO)

#include <time.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "core/app_state.h"
#include "core/dvr.h"
#include "core/settings.h"
#include "driver/dm5680.h"
#include "driver/dm6302.h"
#include "driver/rtc6715.h"
#include "ui/page_common.h"

// Idle-timeout power management state.
// Written from probe calls (UI thread); read+written from thread_peripheral.
// Tearing on struct timespec is benign — at worst the idle check fires one
// tick early/late.  No mutex needed for this level of precision.
static struct timespec last_probe_ts = {0};
// Set by scan_core_idle_tick when analog is powered down; cleared by
// scan_probe_analog so the next probe re-inits the chip before tuning.
static volatile bool analog_powered_down = false;

static void mark_probe_activity(void) {
    clock_gettime(CLOCK_MONOTONIC, &last_probe_ts);
}

// Called when a caller has just powered RTC6715 on themselves (e.g. the
// manual band scanner in page_scannow.c) so that scan_probe_analog does
// not redundantly re-init when its first call sees a stale "powered down"
// flag.
void scan_core_notify_analog_powered_on(void) {
    analog_powered_down = false;
    mark_probe_activity();
}

static bool probe_idle_expired(int idle_secs) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - last_probe_ts.tv_sec) >= idle_secs;
}

#define IDLE_TIMEOUT_SECS 5

// Unified table: one row per distinct 5.8GHz frequency, deduplicated across
// HDZero and analog protocols. Sorted strictly ascending by freq_mhz.
// HDZ band 0=Race (R1..R8, E1, F1, F2, F4), 1=Low (L1..L8).
// Analog channel indices match RTC6715 tab order: 0..7=A, 8..15=B, 16..23=E,
// 24..31=F, 32..39=R, 40..47=L.
//
// Note: analog F8 (idx 31) and analog R7 (idx 38) are both 5880 MHz (identical
// RTC6715 PLL register). The merged row uses idx 38 so the user-facing label
// matches HDZ R7 nomenclature. idx 31 produces identical RF output.
const scan_freq_entry_t scan_freq_table[] = {
    // L band analog (40..47) interleaved with HDZ Low band (band=1, ch 0..7).
    // Analog L: 5333, 5373, 5413, 5453, 5493, 5533, 5573, 5613
    // HDZ Low:  5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621
    { 5333,  -1,  -1, 40 }, // analog L1
    { 5362,   1,   0, -1 }, // HDZ L1
    { 5373,  -1,  -1, 41 }, // analog L2
    { 5399,   1,   1, -1 }, // HDZ L2
    { 5413,  -1,  -1, 42 }, // analog L3
    { 5436,   1,   2, -1 }, // HDZ L3
    { 5453,  -1,  -1, 43 }, // analog L4
    { 5473,   1,   3, -1 }, // HDZ L4
    { 5493,  -1,  -1, 44 }, // analog L5
    { 5510,   1,   4, -1 }, // HDZ L5
    { 5533,  -1,  -1, 45 }, // analog L6
    { 5547,   1,   5, -1 }, // HDZ L6
    { 5573,  -1,  -1, 46 }, // analog L7
    { 5584,   1,   6, -1 }, // HDZ L7
    { 5613,  -1,  -1, 47 }, // analog L8
    { 5621,   1,   7, -1 }, // HDZ L8

    // Upper 5.8 GHz band: E4..E1 analog (descending freq), then R/HDZ overlaps,
    // then A/B/F scattered. Sorted strictly ascending below.
    { 5645,  -1,  -1, 19 }, // analog E4
    { 5658,   0,   0, 32 }, // HDZ R1 + analog R1
    { 5665,  -1,  -1, 18 }, // analog E3
    { 5685,  -1,  -1, 17 }, // analog E2
    { 5695,   0,   1, 33 }, // HDZ R2 + analog R2
    { 5705,   0,   8, 16 }, // HDZ E1 + analog E1
    { 5725,  -1,  -1,  7 }, // analog A8
    { 5732,   0,   2, 34 }, // HDZ R3 + analog R3
    { 5733,  -1,  -1,  8 }, // analog B1
    { 5740,   0,   9, 24 }, // HDZ F1 + analog F1
    { 5745,  -1,  -1,  6 }, // analog A7
    { 5752,  -1,  -1,  9 }, // analog B2
    { 5760,   0,  10, 25 }, // HDZ F2 + analog F2
    { 5765,  -1,  -1,  5 }, // analog A6
    { 5769,   0,   3, 35 }, // HDZ R4 + analog R4
    { 5771,  -1,  -1, 10 }, // analog B3
    { 5780,  -1,  -1, 26 }, // analog F3
    { 5785,  -1,  -1,  4 }, // analog A5
    { 5790,  -1,  -1, 11 }, // analog B4
    { 5800,   0,  11, 27 }, // HDZ F4 + analog F4
    { 5805,  -1,  -1,  3 }, // analog A4
    { 5806,   0,   4, 36 }, // HDZ R5 + analog R5
    { 5809,  -1,  -1, 12 }, // analog B5
    { 5820,  -1,  -1, 28 }, // analog F5
    { 5825,  -1,  -1,  2 }, // analog A3
    { 5828,  -1,  -1, 13 }, // analog B6
    { 5840,  -1,  -1, 29 }, // analog F6
    { 5843,   0,   5, 37 }, // HDZ R6 + analog R6
    { 5845,  -1,  -1,  1 }, // analog A2
    { 5847,  -1,  -1, 14 }, // analog B7
    { 5860,  -1,  -1, 30 }, // analog F7
    { 5865,  -1,  -1,  0 }, // analog A1
    { 5866,  -1,  -1, 15 }, // analog B8
    { 5880,   0,   6, 38 }, // HDZ R7 + analog R7; analog F8 (idx 31) same freq, see note above
    { 5885,  -1,  -1, 20 }, // analog E5
    { 5905,  -1,  -1, 21 }, // analog E6
    { 5917,   0,   7, 39 }, // HDZ R8 + analog R8
    { 5925,  -1,  -1, 22 }, // analog E7
    { 5945,  -1,  -1, 23 }, // analog E8
};

const size_t scan_freq_table_len =
    sizeof(scan_freq_table) / sizeof(scan_freq_table[0]);

// Analog channel idx (0..47) → frequency in MHz. Matches the rtc6715 tab[]
// ordering (A, B, E, F, R, L). Used to fall back when an analog channel idx
// isn't directly present in scan_freq_table (because multiple analog indices
// alias the same frequency and are merged — see the 5880 MHz note above).
const uint16_t scan_analog_idx_to_mhz[48] = {
    5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725, // A1..A8
    5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866, // B1..B8
    5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945, // E1..E8
    5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880, // F1..F8
    5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917, // R1..R8
    5333, 5373, 5413, 5453, 5493, 5533, 5573, 5613, // L1..L8
};

int scan_freq_table_find_by_mhz(uint16_t mhz) {
    for (size_t i = 0; i < scan_freq_table_len; i++) {
        if (scan_freq_table[i].freq_mhz == mhz) {
            return (int)i;
        }
    }
    return -1;
}

// Tunable: fraction of (calib_max - calib_min) above calib_min that counts as
// "signal present". Lowered from 20% to 10% so the auto-detect crossover
// triggers on mid-strength analog signals (typical setups produce RSSI well
// short of the calibrated max).
#define ANALOG_SIGNAL_THRESH_FRAC_NUM 10
#define ANALOG_SIGNAL_THRESH_FRAC_DEN 100

// Fallback threshold in mV when calibration is missing/corrupt.
#define ANALOG_SIGNAL_THRESH_FALLBACK_MV 1500

static uint16_t analog_signal_threshold_mv(void) {
    uint16_t cmin = g_setting.analog_rssi.calib_min;
    uint16_t cmax = g_setting.analog_rssi.calib_max;
    if (cmax <= cmin) {
        static bool warned = false;
        if (!warned) {
            LOGW("analog_rssi calibration invalid (min=%u max=%u), using %u mV fallback",
                 cmin, cmax, ANALOG_SIGNAL_THRESH_FALLBACK_MV);
            warned = true;
        }
        return ANALOG_SIGNAL_THRESH_FALLBACK_MV;
    }
    return cmin + (uint16_t)(((uint32_t)(cmax - cmin)
                              * ANALOG_SIGNAL_THRESH_FRAC_NUM)
                             / ANALOG_SIGNAL_THRESH_FRAC_DEN);
}

bool scan_probe_hdzero(uint8_t band, uint8_t channel,
                       uint8_t *gain_out, bool *valid_out) {
    mark_probe_activity();
    uint8_t gain[4];

    DM6302_SetChannel(band, channel);

    usleep(100000);
    DM5680_clear_vldflg();
    DM5680_req_vldflg();

    DM6302_get_gain(gain);
    uint8_t b1 = (gain[0] > gain[1]) ? gain[0] : gain[1];
    uint8_t b2 = (gain[2] > gain[3]) ? gain[2] : gain[3];
    uint8_t max_gain = (b1 > b2) ? b1 : b2;

    bool valid = (rx_status[0].rx_valid | rx_status[1].rx_valid) != 0;

    if (gain_out)  *gain_out  = max_gain;
    if (valid_out) *valid_out = valid;

    LOGI("scan_probe_hdzero band:%u ch:%u valid:%d gain:%u",
         band, channel, valid, max_gain);
    return valid;
}

bool scan_probe_analog(uint8_t channel_idx,
                       uint16_t *rssi_mv_out, bool *valid_out) {
    mark_probe_activity();
    if (analog_powered_down) {
        // rtc6715.init(1, 0) includes a 200ms power-on stabilization delay.
        // This is additive to the 50ms PLL-lock usleep below; first probe
        // after an idle power-down therefore stalls ~250ms instead of 50ms.
        rtc6715.init(1, 0);
        analog_powered_down = false;
    }
    rtc6715.set_ch(channel_idx);
    usleep(50000); // RTC6715 PLL lock time

    int mv = rtc6715_get_rssi();
    if (mv < 0) mv = 0;
    uint16_t rssi = (uint16_t)mv;

    uint16_t thresh = analog_signal_threshold_mv();
    bool valid = rssi > thresh;

    if (rssi_mv_out) *rssi_mv_out = rssi;
    if (valid_out)   *valid_out   = valid;

    LOGI("scan_probe_analog ch:%u rssi:%u mv (thresh=%u) valid:%d",
         channel_idx, rssi, thresh, valid);
    return valid;
}

// Map raw signal strength to 0..100 for sorting.
static uint8_t hdz_strength_norm(uint8_t gain) {
    // DM6302 gain table is 0..60 (see driver/dm6302.c DM6302_gain_tab).
    // Map 0..60 → 0..100; clamp at 100 in case the table returns 61 (no-match
    // sentinel) or a future driver widens the range.
    uint32_t s = (uint32_t)gain * 100u / 60u;
    if (s > 100) s = 100;
    return (uint8_t)s;
}

static uint8_t analog_strength_norm(uint16_t rssi_mv) {
    uint16_t cmin = g_setting.analog_rssi.calib_min;
    uint16_t cmax = g_setting.analog_rssi.calib_max;
    if (cmax <= cmin || rssi_mv <= cmin) return 0;
    if (rssi_mv >= cmax) return 100;
    return (uint8_t)(((uint32_t)(rssi_mv - cmin) * 100u) / (cmax - cmin));
}

scan_result_t scan_probe_both(const scan_freq_entry_t *entry) {
    mark_probe_activity();
    scan_result_t r = { PROTOCOL_NONE, 0, 0, 0 };

    // Probe analog first (slower PLL).
    bool analog_valid = false;
    uint16_t analog_mv = 0;
    if (entry->analog_channel >= 0) {
        scan_probe_analog((uint8_t)entry->analog_channel,
                          &analog_mv, &analog_valid);
    }

    // Probe HDZero.
    bool hdz_valid = false;
    uint8_t hdz_gain = 0;
    if (entry->hdz_band >= 0 && entry->hdz_channel >= 0) {
        scan_probe_hdzero((uint8_t)entry->hdz_band,
                          (uint8_t)entry->hdz_channel,
                          &hdz_gain, &hdz_valid);
    }

    // Tie-break: HDZ wins if both valid.
    if (hdz_valid) {
        r.protocol  = PROTOCOL_HDZ;
        r.hdz_gain  = hdz_gain;
        r.strength  = hdz_strength_norm(hdz_gain);
    } else if (analog_valid) {
        r.protocol  = PROTOCOL_ANALOG;
        r.analog_mv = analog_mv;
        r.strength  = analog_strength_norm(analog_mv);
    }

    LOGI("scan_probe_both freq=%u protocol=%d strength=%u",
         entry->freq_mhz, r.protocol, r.strength);
    return r;
}

// Watchdog state. Counts in 100ms scan_core_idle_tick ticks. Resets to 0
// whenever the current source is reporting a valid signal, so a brief signal
// hiccup never triggers a crossover.
//   CROSSOVER_NO_SIGNAL_THRESH ticks of no signal arms the watchdog.
//   CROSSOVER_PROBE_PERIOD ticks between successive cross-protocol probes.
#define CROSSOVER_NO_SIGNAL_THRESH 30  // ~3s before crossover probes start
#define CROSSOVER_PROBE_PERIOD     20  // ~2s between probes once armed
static int no_signal_ticks = 0;
static int crossover_period_ticks = 0;

static int find_freq_idx_for_current_source(void) {
    if (g_source_info.source == SOURCE_HDZERO) {
        uint8_t ch  = (uint8_t)((g_setting.scan.channel - 1) & 0x7F);
        int8_t band = (int8_t)g_setting.source.hdzero_band;
        for (size_t i = 0; i < scan_freq_table_len; i++) {
            if (scan_freq_table[i].hdz_band == band &&
                scan_freq_table[i].hdz_channel == (int8_t)ch) {
                return (int)i;
            }
        }
    } else if (g_source_info.source == SOURCE_AV_MODULE) {
        int8_t ch = (int8_t)((g_setting.source.analog_channel - 1) & 0x7F);
        for (size_t i = 0; i < scan_freq_table_len; i++) {
            if (scan_freq_table[i].analog_channel == ch) {
                return (int)i;
            }
        }
    }
    return -1;
}

// Probe the OTHER protocol at the same frequency the user is currently
// watching, and switch the source if it has signal. Heavy (probe ~250ms,
// switch up to ~1.4s with internal sleeps), but only runs once every
// CROSSOVER_PROBE_PERIOD ticks while the current source is dark.
static void try_crossover_probe(void) {
    int idx = find_freq_idx_for_current_source();
    if (idx < 0) return;
    const scan_freq_entry_t *entry = &scan_freq_table[idx];

    if (g_source_info.source == SOURCE_HDZERO) {
        if (entry->analog_channel < 0) return;
        uint16_t rssi_mv = 0;
        bool valid = false;
        scan_probe_analog((uint8_t)entry->analog_channel, &rssi_mv, &valid);
        if (!valid) return;

        LOGI("auto-detect crossover: HDZ->analog (analog_ch=%u rssi=%u mv)",
             (uint8_t)entry->analog_channel + 1, rssi_mv);
        g_setting.source.analog_channel = (uint8_t)entry->analog_channel + 1;
        ini_putl("source", "analog_channel",
                 g_setting.source.analog_channel, SETTING_INI);
        dvr_cmd(DVR_STOP);
        app_switch_to_analog(0);
        app_state_push(APP_STATE_VIDEO);
        g_source_info.source = SOURCE_AV_MODULE;
        dvr_select_audio_source(g_setting.record.audio_source);
        dvr_enable_line_out(true);
    } else if (g_source_info.source == SOURCE_AV_MODULE) {
        if (entry->hdz_channel < 0 || entry->hdz_band < 0) return;
        uint8_t gain = 0;
        bool valid = false;
        scan_probe_hdzero((uint8_t)entry->hdz_band,
                          (uint8_t)entry->hdz_channel, &gain, &valid);
        if (!valid) return;

        LOGI("auto-detect crossover: analog->HDZ (hdz_ch=%u gain=%u)",
             (uint8_t)entry->hdz_channel + 1, gain);
        g_setting.scan.channel = (uint8_t)entry->hdz_channel + 1;
        ini_putl("scan", "channel", g_setting.scan.channel, SETTING_INI);
        dvr_cmd(DVR_STOP);
        app_switch_to_hdzero(true);
        app_state_push(APP_STATE_VIDEO);
        g_source_info.source = SOURCE_HDZERO;
        dvr_select_audio_source(g_setting.record.audio_source);
        dvr_enable_line_out(true);
    }
}

void scan_core_idle_tick(void) {
    if (!g_setting.source.auto_protocol_detect) {
        no_signal_ticks = 0;
        crossover_period_ticks = 0;
        return;
    }

    // Original idle-power-management: after IDLE_TIMEOUT_SECS without a
    // manual probe, power down the radio not in use.
    if (last_probe_ts.tv_sec != 0 && probe_idle_expired(IDLE_TIMEOUT_SECS)) {
        if (g_source_info.source == SOURCE_HDZERO) {
            rtc6715.init(0, 0);
            analog_powered_down = true;
        }
        last_probe_ts.tv_sec = 0;
    }

    // Crossover watchdog runs only while a video source is active. In menus,
    // playback, sleep, etc. there's nothing to fall over from.
    if (g_app_state != APP_STATE_VIDEO) {
        no_signal_ticks = 0;
        crossover_period_ticks = 0;
        return;
    }

    bool have_signal;
    if (g_source_info.source == SOURCE_HDZERO) {
        have_signal = (rx_status[0].rx_valid | rx_status[1].rx_valid) != 0;
    } else if (g_source_info.source == SOURCE_AV_MODULE) {
        have_signal = g_source_info.av_bay_status;
    } else {
        // HDMI / AV In / other sources: no auto-detect crossover.
        no_signal_ticks = 0;
        crossover_period_ticks = 0;
        return;
    }

    if (have_signal) {
        no_signal_ticks = 0;
        crossover_period_ticks = 0;
        return;
    }

    no_signal_ticks++;
    if (no_signal_ticks < CROSSOVER_NO_SIGNAL_THRESH) return;

    crossover_period_ticks++;
    if (crossover_period_ticks < CROSSOVER_PROBE_PERIOD) return;
    crossover_period_ticks = 0;

    try_crossover_probe();
}

void scan_core_self_check(void) {
    for (size_t i = 1; i < scan_freq_table_len; i++) {
        if (scan_freq_table[i].freq_mhz <= scan_freq_table[i-1].freq_mhz) {
            LOGE("scan_freq_table not strictly ascending at row %zu (%u vs %u)",
                 i, scan_freq_table[i-1].freq_mhz, scan_freq_table[i].freq_mhz);
        }
        if (scan_freq_table[i].hdz_channel == -1 &&
            scan_freq_table[i].analog_channel == -1) {
            LOGE("scan_freq_table row %zu has neither HDZ nor analog channel",
                 i);
        }
    }
}

_Static_assert(sizeof(scan_freq_table) / sizeof(scan_freq_table[0]) > 0,
               "scan_freq_table must be non-empty");

#endif // HDZBOXPRO
