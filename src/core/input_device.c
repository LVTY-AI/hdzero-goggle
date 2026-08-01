#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#ifdef EMULATOR_BUILD
#include "SDLaccess.h"
#endif

#include "defines.h"
#include "input_device.h"

#include "common.hh"
#include "ht.h"
#include "osd.h"

#include "core/app_state.h"
#include "core/dvr.h"
#include "core/elrs.h"
#include "core/scan_core.h"
#include "core/settings.h"
#include "core/sleep_mode.h"
#include "driver/beep.h"
#include "driver/dm6302.h"
#include "driver/hardware.h"
#include "driver/i2c.h"
#include "driver/it66121.h"
#include "driver/rtc6715.h"
#include "driver/screen.h"
#include "driver/uart.h"
#include "ui/page_common.h"
#include "ui/page_fans.h"
#include "ui/page_headtracker.h"
#include "ui/page_imagesettings.h"
#include "ui/page_playback.h"
#include "ui/page_power.h"
#include "ui/page_scannow.h"
#include "ui/page_source.h"
#include "ui/ui_image_setting.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_osd_element_pos.h"
#include "ui/ui_porting.h"

///////////////////////////////////////////////////////////////////////////////
// Tune channel on video mode
#define TUNER_TIMER_LEN 30

static uint8_t tune_state = 0; // 0=init; 1=waiting for key; 2=tuning
static uint16_t tune_timer = 0;

#define EPOLL_FD_CNT 4

static int epfd;
static pthread_t input_device_pid;

static int btn_value = 0;

// action: 1 = tune up, 2 = tune down, 3 = confirm
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
// Set to 0 to restore the per-click bandwidth sweep in Auto BW (Both) mode:
// slower (re-inits the DM6302 on each click) but identifies the locking
// bandwidth up front. When 1, Both-mode clicks take the single-bandwidth fast
// path and let the live bw-reacquire watchdog settle the bandwidth (tagged
// "Detecting..." while it does). Revert by setting this back to 0.
#define AUTODETECT_FAST_DIAL 1
static int auto_detect_freq_idx = -1;
#endif

// Flat HDZ channel space: the band/channel split is purely a UI grouping over
// a contiguous 20-channel hardware space (DM6302 indexes lowband as ch+12).
// flat 0..11  -> Raceband ch 0..11 (R1-R8, E1, F1, F2, F4)
// flat 12..19 -> Lowband  ch 0..7  (L1-L8)
#define HDZ_FLAT_COUNT (BASE_CH_NUM + 8) // 20

static void hdz_flat_to_band_ch(int flat, uint8_t *band, uint8_t *ch1) {
    if (flat < 0) flat = 0;
    if (flat >= HDZ_FLAT_COUNT) flat = HDZ_FLAT_COUNT - 1;
    if (flat < BASE_CH_NUM) {
        *band = 0;
        *ch1 = (uint8_t)(flat + 1);
    } else {
        *band = 1;
        *ch1 = (uint8_t)(flat - BASE_CH_NUM + 1);
    }
}

static int hdz_band_ch_to_flat(uint8_t band, uint8_t ch1) {
    int ch0 = (int)ch1 - 1;
    if (ch0 < 0) ch0 = 0;
    return (band == 1) ? (BASE_CH_NUM + ch0) : ch0;
}

void exit_tune_channel() {
    tune_state = 0;
    tune_timer = 0;
    channel_osd_mode = 0;
    channel_osd_preview_proto = 0;
    channel_osd_preview_band = 0xFF;
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    auto_detect_freq_idx = -1;
#endif
}

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
// Returns the band-order navigation index (0..SCAN_BAND_ORDER_COUNT-1) of the
// channel currently being viewed, so the dial resumes from the right position.
static int find_freq_table_index(void) {
    if (g_source_info.source == SOURCE_HDZERO) {
        uint8_t ch = (g_setting.scan.channel - 1) & 0x7F;
        int8_t  band = (int8_t)g_setting.source.hdzero_band;
        for (int i = 0; i < SCAN_BAND_ORDER_COUNT; i++) {
            const scan_freq_entry_t *e = scan_band_order_entry(i);
            if (e && e->hdz_band == band && e->hdz_channel == (int8_t)ch)
                return i;
        }
    } else if (g_source_info.source == SOURCE_AV_MODULE) {
        // The analog channel index is already in band order (A,B,E,F,R,L).
        int ch = (int)((g_setting.source.analog_channel - 1) & 0x7F);
        if (ch >= 0 && ch < SCAN_BAND_ORDER_COUNT)
            return ch;
    }
    return 0;
}
#endif

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
// analog_ch is the band-order index (== the analog channel for A..R), used in
// place of the row's stored analog index so the F8/R7 alias (both 5880 MHz,
// merged to the R7 row) tunes/labels as F8 at the F8 slot and R7 at the R7 slot.
static void apply_freq_entry(const scan_freq_entry_t *entry,
                             const scan_result_t *r,
                             bool send_msp, int analog_ch) {
    if (r->protocol == PROTOCOL_HDZ) {
        dvr_cmd(DVR_STOP);

        // Persist the new band+channel FIRST so a subsequent source switch
        // (app_switch_to_hdzero(true)) tunes directly to it instead of the
        // old value and then we re-tune. The freq table carries the band
        // (Race vs Low) per entry; committing it here is what lets the
        // unified dial cross between bands correctly.
        bool channel_changed = false;
        if (entry->hdz_band >= 0 &&
            g_setting.source.hdzero_band != (uint8_t)entry->hdz_band) {
            g_setting.source.hdzero_band = (uint8_t)entry->hdz_band;
            ini_putl("source", "hdzero_band",
                     g_setting.source.hdzero_band, SETTING_INI);
            channel_changed = true;
        }
        if (entry->hdz_channel >= 0) {
            uint8_t new_ch = (uint8_t)entry->hdz_channel + 1;
            if (g_setting.scan.channel != new_ch) {
                g_setting.scan.channel = new_ch;
                ini_putl("scan", "channel",
                         g_setting.scan.channel, SETTING_INI);
                channel_changed = true;
            }
        }

        if (g_source_info.source != SOURCE_HDZERO) {
            // Cross-protocol switch. app_switch_to_hdzero(true) reads
            // g_setting.scan.channel (just persisted above) and tunes to it.
            // We must also commit app_state + g_source_info.source + dvr
            // routing (matches the page_source.c pattern); without this,
            // periodic checks see stale source and the next dial click
            // re-runs this whole ~1.4s switch path.
            app_switch_to_hdzero(true);
            app_state_push(APP_STATE_VIDEO);
            g_source_info.source = SOURCE_HDZERO;
            dvr_select_audio_source(g_setting.record.audio_source);
            dvr_enable_line_out(true);
        } else if (channel_changed) {
            // Already on HDZ: lightweight retune. In Both mode the locked
            // bandwidth may differ from what's open, so re-open the baseband
            // first (no-op when the bandwidth is unchanged).
            HDZero_open(hdzero_effective_bw());
            hdzero_switch_channel(g_setting.scan.channel - 1);
        }
        if (send_msp) msp_channel_update();
    } else if (r->protocol == PROTOCOL_ANALOG) {
        dvr_cmd(DVR_STOP);

        bool channel_changed = false;
        if (entry->analog_channel >= 0) {
            uint8_t new_ch = (uint8_t)analog_ch + 1;
            if (g_setting.source.analog_channel != new_ch) {
                g_setting.source.analog_channel = new_ch;
                ini_putl("source", "analog_channel",
                         g_setting.source.analog_channel, SETTING_INI);
                channel_changed = true;
            }
        }

        if (g_source_info.source != SOURCE_AV_MODULE) {
            // app_switch_to_analog(0) reads g_setting.source.analog_channel
            // and tunes to it.
            app_switch_to_analog(0);
            app_state_push(APP_STATE_VIDEO);
            g_source_info.source = SOURCE_AV_MODULE;
            dvr_select_audio_source(g_setting.record.audio_source);
            dvr_enable_line_out(true);
        } else if (channel_changed) {
            rtc6715.set_ch(g_setting.source.analog_channel - 1);
        }
        if (send_msp) msp_channel_update();
    } else {
        dvr_cmd(DVR_STOP);
        // No signal: stay on current source, but tune to the entry's
        // matching protocol channel so subsequent navigation makes sense.
        if (g_source_info.source == SOURCE_HDZERO && entry->hdz_channel >= 0) {
            if (entry->hdz_band >= 0) {
                g_setting.source.hdzero_band = (uint8_t)entry->hdz_band;
                ini_putl("source", "hdzero_band",
                         g_setting.source.hdzero_band, SETTING_INI);
            }
            g_setting.scan.channel = (uint8_t)entry->hdz_channel + 1;
            hdzero_switch_channel(g_setting.scan.channel - 1);
        } else if (g_source_info.source == SOURCE_AV_MODULE && entry->analog_channel >= 0) {
            g_setting.source.analog_channel = (uint8_t)analog_ch + 1;
            rtc6715.set_ch(g_setting.source.analog_channel - 1);
        }
    }
}
#endif

void tune_channel(uint8_t action) {
    static uint8_t channel = 0;

    if (g_setting.ease.no_dial)
        return;

    // Channel selection is disabled while an auto-detect probe is in flight
    // (the "Detecting..." tag is showing).
    if (osd_is_detecting())
        return;

#if defined HDZGOGGLE
    if (g_source_info.source != SOURCE_HDZERO) {
        return;
    }

#elif defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    if (g_source_info.source != SOURCE_HDZERO && g_source_info.source != SOURCE_AV_MODULE) {
        return;
    }
#endif

    LOGI("tune_channel:%d", action);

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
    if (g_setting.source.auto_protocol_detect &&
        (g_source_info.source == SOURCE_HDZERO ||
         g_source_info.source == SOURCE_AV_MODULE)) {

        // Auto-detect path: walk the unified frequency table on UP/DOWN,
        // probe both protocols on CLICK/PRESS.
        if (auto_detect_freq_idx < 0) auto_detect_freq_idx = find_freq_table_index();

        if (action == DIAL_KEY_UP) {
            auto_detect_freq_idx = (auto_detect_freq_idx + 1) % SCAN_BAND_ORDER_COUNT;
        } else if (action == DIAL_KEY_DOWN) {
            auto_detect_freq_idx = (auto_detect_freq_idx - 1 + SCAN_BAND_ORDER_COUNT)
                       % SCAN_BAND_ORDER_COUNT;
        } else if (action == DIAL_KEY_CLICK || action == DIAL_KEY_PRESS) {
            const scan_freq_entry_t *entry = scan_band_order_entry(auto_detect_freq_idx);
            if (!entry) return;
            // Show "Detecting..." while the (blocking) probe runs.
            osd_detecting_show(true);
            scan_result_t r;
            r.protocol  = PROTOCOL_NONE;
            r.strength  = 0;
            r.hdz_gain  = 0;
            r.analog_mv = 0;
            r.hdz_bw    = 0;
            if (!AUTODETECT_FAST_DIAL &&
                g_setting.source.hdzero_bw == SETTING_SOURCES_HDZERO_BW_BOTH) {
                // Both: sweep bandwidths to determine protocol AND which
                // bandwidth locks, so the live view opens at the right one.
                uint8_t locked_bw = g_hdz_detected_bw;
                r = scan_probe_both_sweep(entry, &locked_bw);
                if (r.protocol == PROTOCOL_NONE) {
                    // Honor single-protocol intent even if nothing locked.
                    if (entry->hdz_channel >= 0 && entry->analog_channel < 0)
                        r.protocol = PROTOCOL_HDZ;
                    else if (entry->analog_channel >= 0 && entry->hdz_channel < 0)
                        r.protocol = PROTOCOL_ANALOG;
                    else
                        r.protocol = (g_source_info.source == SOURCE_AV_MODULE)
                                         ? PROTOCOL_ANALOG : PROTOCOL_HDZ;
                }
                if (r.protocol == PROTOCOL_HDZ)
                    g_hdz_detected_bw = locked_bw;
            } else {
                // Single bandwidth — fast path, no sweep. For single-protocol
                // entries, honor the click as intent: switch without requiring
                // a probe (probes can return invalid for transient reasons).
                if (entry->hdz_channel >= 0 && entry->analog_channel < 0) {
                    r.protocol = PROTOCOL_HDZ;
                } else if (entry->analog_channel >= 0 && entry->hdz_channel < 0) {
                    r.protocol = PROTOCOL_ANALOG;
                } else {
                    r = scan_probe_both(entry);
                    if (r.protocol == PROTOCOL_NONE) {
                        r.protocol = (g_source_info.source == SOURCE_AV_MODULE)
                                         ? PROTOCOL_ANALOG : PROTOCOL_HDZ;
                    }
                }
            }
            apply_freq_entry(entry, &r, action == DIAL_KEY_PRESS,
                             auto_detect_freq_idx);
            channel_osd_mode = CHANNEL_SHOWTIME;
            channel_osd_preview_proto = 0; // back to normal "CH:" display
            channel_osd_preview_band = 0xFF;
            tune_state = 1;
            tune_timer = 0;
            osd_detecting_show(false); // back to the normal channel tag
            return;
        } else {
            return;
        }

        // For UP/DOWN: render a preview that's always meaningful, even when
        // the entry's protocol differs from the current source. Prefer the
        // current source's namespace if the entry has it; otherwise fall
        // back to whichever protocol the entry does have. channel_osd_mode
        // carries the channel index; channel_osd_preview_proto tells the OSD
        // which namespace (HDZ vs analog) to format with.
        const scan_freq_entry_t *entry = scan_band_order_entry(auto_detect_freq_idx);
        if (!entry) return;
        bool prefer_hdz = (g_source_info.source == SOURCE_HDZERO);
        // F8 and R7 are the same 5880 MHz frequency, merged into one (R7-tagged)
        // row. At the F8 band-order slot that row's HDZ channel (R7) really
        // belongs to the R7 slot, so show this slot's own analog name (F8). The
        // tell: the band-order index doesn't match the row's analog index.
        // analog_ch is the band-order index, which equals the analog channel
        // for A..R, so it names F8 as F8 (and R7 as R7 at the R7 slot).
        int analog_ch = auto_detect_freq_idx;
        bool alias_analog = (auto_detect_freq_idx < 40 &&
                             (int)entry->analog_channel != auto_detect_freq_idx);
        // Dual = this frequency carries both protocols (the F8/R7 alias slot is
        // presented as analog-only F8, so it is not "dual"). Tag it "Dual"; the
        // name is the same in either namespace, so format it as HDZ.
        bool is_dual = (entry->hdz_channel >= 0 && entry->analog_channel >= 0 &&
                        !alias_analog);
        // Default: no band override. Set it below for HDZ previews so a
        // lowband entry shows "L*" even while the committed band is Race.
        channel_osd_preview_band = 0xFF;
        if (is_dual) {
            channel_osd_mode = 0x80 | ((uint8_t)entry->hdz_channel + 1);
            channel_osd_preview_proto = 3; // Dual
            if (entry->hdz_band >= 0) channel_osd_preview_band = (uint8_t)entry->hdz_band;
        } else if (prefer_hdz && entry->hdz_channel >= 0 && !alias_analog) {
            channel_osd_mode = 0x80 | ((uint8_t)entry->hdz_channel + 1);
            channel_osd_preview_proto = 1;
            if (entry->hdz_band >= 0) channel_osd_preview_band = (uint8_t)entry->hdz_band;
        } else if (entry->analog_channel >= 0 && (!prefer_hdz || alias_analog)) {
            channel_osd_mode = 0x80 | ((uint8_t)analog_ch + 1);
            channel_osd_preview_proto = 2;
        } else if (entry->hdz_channel >= 0 && !alias_analog) {
            channel_osd_mode = 0x80 | ((uint8_t)entry->hdz_channel + 1);
            channel_osd_preview_proto = 1;
            if (entry->hdz_band >= 0) channel_osd_preview_band = (uint8_t)entry->hdz_band;
        } else if (entry->analog_channel >= 0) {
            channel_osd_mode = 0x80 | ((uint8_t)analog_ch + 1);
            channel_osd_preview_proto = 2;
        } else {
            channel_osd_mode = 0;
            channel_osd_preview_proto = 0;
        }
        tune_timer = TUNER_TIMER_LEN;
        tune_state = 2;
        return;
    }
#endif

    if (tune_state == 0) {
        channel_osd_mode = 0;
        tune_state = 1;
    }

    if (tune_state == 1) {
        if ((action == DIAL_KEY_UP) || (action == DIAL_KEY_DOWN)) {
            tune_timer = TUNER_TIMER_LEN;
            tune_state = 2;

            if (g_source_info.source == SOURCE_HDZERO) {
                // Walk a flat 20-channel space across both bands (no toggle).
                channel = (uint8_t)(hdz_band_ch_to_flat(
                              g_setting.source.hdzero_band,
                              g_setting.scan.channel) + 1);
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
            } else if (g_source_info.source == SOURCE_AV_MODULE) {
                channel = g_setting.source.analog_channel;
#endif
            } else {
                return;
            }
        }
    }

    if (tune_state != 2)
        return;

    uint8_t channel_num;
    if (g_source_info.source == SOURCE_HDZERO)
        channel_num = HDZ_FLAT_COUNT;
    else if (g_source_info.source == SOURCE_AV_MODULE)
        channel_num = ANALOG_CHANNEL_NUM;
    else
        return;

    switch (action) {
    case DIAL_KEY_UP: // Tune up
        if (channel == channel_num)
            channel = 1;
        else
            channel++;
        break;

    case DIAL_KEY_DOWN: // Tune down
        if (channel == 1)
            channel = channel_num;
        else
            channel--;
        break;

    case DIAL_KEY_PRESS: // confirm to tune with VTX freq send
    case DIAL_KEY_CLICK: // confirm to tune
        if (g_source_info.source == SOURCE_HDZERO) {
            // channel is a flat 0..19 index (+1); map back to band+channel.
            uint8_t nband, nch1;
            hdz_flat_to_band_ch((int)channel - 1, &nband, &nch1);
            if (g_setting.scan.channel != nch1 ||
                g_setting.source.hdzero_band != nband) {
                g_setting.source.hdzero_band = nband;
                ini_putl("source", "hdzero_band", nband, SETTING_INI);
                g_setting.scan.channel = nch1;
                ini_putl("scan", "channel", nch1, SETTING_INI);
                dvr_cmd(DVR_STOP);
                // Auto/Both: switch at the currently open bandwidth. If the new
                // channel's VTX is on the other one, the bw-reacquire watchdog
                // (scan_core_hdz_bw_tick) settles it within ~2s behind the
                // "Detecting..." mask. The old per-click Wide+Narrow sweep
                // re-inited the DM6302 up to three times (~5s frozen, unmasked
                // green flashes) on every confirm.
                hdzero_switch_channel(nch1 - 1);
                if (action == DIAL_KEY_PRESS) {
                    msp_channel_update();
                }
            }
        } else if (g_source_info.source == SOURCE_AV_MODULE) {
            if (g_setting.source.analog_channel != channel) {
                g_setting.source.analog_channel = channel;
                ini_putl("source", "analog_channel", g_setting.source.analog_channel, SETTING_INI);
                dvr_cmd(DVR_STOP);
                rtc6715.set_ch(g_setting.source.analog_channel - 1);
                if (action == DIAL_KEY_PRESS) {
                    msp_channel_update();
                }
            }
        }
        tune_timer = 0;
        tune_state = 1;
        channel_osd_mode = CHANNEL_SHOWTIME;
        channel_osd_preview_proto = 0;
        channel_osd_preview_band = 0xFF;
        return;

    default:
        perror("TuneChannel: bad command");
        break;
    }

    if (g_source_info.source == SOURCE_HDZERO) {
        // channel is a flat index (+1); preview the matching band's R*/L* name.
        uint8_t pband, pch1;
        hdz_flat_to_band_ch((int)channel - 1, &pband, &pch1);
        channel_osd_mode = 0x80 | pch1;
        channel_osd_preview_proto = 0; // untagged "To L4?" like the classic dial
        channel_osd_preview_band = pband;
    } else {
        channel_osd_mode = 0x80 | channel;
        channel_osd_preview_proto = 0;
        channel_osd_preview_band = 0xFF;
    }
    tune_timer = TUNER_TIMER_LEN;
}

void tune_channel_confirm() {
#if defined HDZGOGGLE
    if (g_source_info.source == SOURCE_HDZERO) {
        tune_channel(DIAL_KEY_CLICK);
    }
#elif defined HDZBOXPRO
    if (g_source_info.source == SOURCE_HDZERO) {
        tune_channel(DIAL_KEY_CLICK);
    } else if (g_source_info.source == SOURCE_AV_MODULE) {
        tune_channel(DIAL_KEY_CLICK);
    }
#elif defined HDZGOGGLE2
    if (g_source_info.source == SOURCE_HDZERO) {
        tune_channel(DIAL_KEY_CLICK);
    } else if (g_source_info.source == SOURCE_AV_MODULE && g_setting.source.analog_module == SETTING_SOURCES_ANALOG_MODULE_INTERNAL) {
        tune_channel(DIAL_KEY_CLICK);
    }
#endif
}

void tune_channel_timer() {
    if (tune_state == 2) {
        if (!tune_timer)
            return;

        if (tune_timer == 1) {
            tune_state = 1;
            channel_osd_mode = CHANNEL_SHOWTIME;
        }
        tune_timer--;
        // LOGI("tune_channel_timer:%d",tune_timer);
    } else {
        if (channel_osd_mode)
            channel_osd_mode--;
    }
}
///////////////////////////////////////////////////////////////////////////////

static int roller_up_acc = 0;
static int roller_down_acc = 0;

static bool scroll_sim_mode = false;
static bool scroll_sim_mode_pending = false;

#define SCROLL_REPEAT_NONE 0
#define SCROLL_REPEAT_UP   1
#define SCROLL_REPEAT_DOWN 2
static int scroll_sim_mode_repeat = SCROLL_REPEAT_NONE;

void (*btn_click_callback)() = &osd_toggle;
void (*btn_press_callback)() = &app_switch_to_menu;

void (*rbtn_click_callback)() = &dvr_toggle;
void (*rbtn_press_callback)() = &step_topfan;
void (*rbtn_double_click_callback)() = &ht_set_center_position;

void (*roller_callback)(uint8_t key) = &tune_channel;

static void roller_up(void);
static void roller_down(void);

static void btn_press(void) // long press left key
{
    LOGI("btn_press (%d)", g_app_state);
    if (g_scanning || (g_init_done != 1)) // no long pree Enter before done with init
        return;

    if (g_app_state == APP_STATE_USER_INPUT_DISABLED)
        return;

    pthread_mutex_lock(&lvgl_mutex);

    g_autoscan_exit = true;
    if (g_app_state == APP_STATE_MAINMENU) // Main menu -> Video
    {
        app_exit_menu();
        app_state_push(APP_STATE_VIDEO);
    } else if ((g_app_state == APP_STATE_VIDEO) || (g_app_state == APP_STATE_IMS)) { // video -> Main menu
        if (tune_timer) {
#if defined HDZGOGGLE
            if (g_source_info.source == SOURCE_HDZERO) {
                tune_channel(DIAL_KEY_PRESS);
            } else {
                (*btn_press_callback)();
            }
#elif defined HDZBOXPRO
            if (g_source_info.source == SOURCE_HDZERO) {
                tune_channel(DIAL_KEY_PRESS);
            } else if (g_source_info.source == SOURCE_AV_MODULE) {
                tune_channel(DIAL_KEY_PRESS);
            } else {
                (*btn_press_callback)();
            }

#elif defined HDZGOGGLE2
            if (g_source_info.source == SOURCE_HDZERO) {
                tune_channel(DIAL_KEY_PRESS);
            } else if (g_source_info.source == SOURCE_AV_MODULE) {
                tune_channel(DIAL_KEY_PRESS);
            } else if (g_source_info.source == SOURCE_AV_MODULE && g_setting.source.analog_module == SETTING_SOURCES_ANALOG_MODULE_INTERNAL) {
                tune_channel(DIAL_KEY_PRESS);
            } else {
                (*btn_press_callback)();
            }
#endif
        } else {
            (*btn_press_callback)();
        }
    } else if (g_app_state == APP_STATE_OSD_ELEMENT_PREV) {
        ui_osd_element_pos_cancel_and_hide();
        app_switch_to_menu();
    } else if (g_app_state == APP_STATE_PLAYBACK) {
        pb_key(DIAL_KEY_PRESS);
    } else if (g_app_state == APP_STATE_SLEEP) {
        wake_up();
    } else { // Sub-menu -> back. Page may absorb (e.g. scan_now RESULTS->IDLE);
             // otherwise fall through to Main menu.
        if (!submenu_back()) {
            app_state_push(APP_STATE_MAINMENU);
            main_menu_show(true);
        }
    }
    pthread_mutex_unlock(&lvgl_mutex);
}

static void btn_click(void) // short press enter key
{
    LOGI("btn_click (%d)", g_app_state);
    if (g_init_done != 1) // no short pree Enter before done with init
        return;

    if (g_app_state == APP_STATE_USER_INPUT_DISABLED)
        return;

    if (g_app_state == APP_STATE_VIDEO) {
        pthread_mutex_lock(&lvgl_mutex);
        if (tune_state == 2) {
            tune_channel_confirm();
        } else {
            (*btn_click_callback)();
        }
        pthread_mutex_unlock(&lvgl_mutex);
        return;
    } else if (g_app_state == APP_STATE_IMS) {
        pthread_mutex_lock(&lvgl_mutex);
        if (ims_key(DIAL_KEY_CLICK))
            app_switch_to_menu();
        pthread_mutex_unlock(&lvgl_mutex);
        return;
    } else if (g_app_state == APP_STATE_OSD_ELEMENT_PREV) {
        pthread_mutex_lock(&lvgl_mutex);
        if (ui_osd_element_pos_handle_input(DIAL_KEY_CLICK))
            app_switch_to_menu();
        pthread_mutex_unlock(&lvgl_mutex);
        return;
    }

    if (!main_menu_is_shown())
        return;

    if (g_scanning)
        return;

    pthread_mutex_lock(&lvgl_mutex);

    autoscan_exit();
    if (g_app_state == APP_STATE_MAINMENU) {
        LOGI("level = 1");
        app_state_push(APP_STATE_SUBMENU);
        submenu_enter();
    } else if (g_app_state == APP_STATE_SUBMENU ||
               g_app_state == APP_STATE_PLAYBACK ||
               g_app_state == APP_STATE_SUBMENU_ITEM_FOCUSED ||
               g_app_state == APP_STATE_WIFI) {
        submenu_click();
    } else if (g_app_state == APP_STATE_SLEEP) {
        wake_up();
    }
    pthread_mutex_unlock(&lvgl_mutex);
}

void rbtn_click(right_button_t click_type) {
    if (g_init_done != 1)
        return;

    if (g_app_state == APP_STATE_USER_INPUT_DISABLED)
        return;

    if (scroll_sim_mode) {
        switch (click_type) {
        case RIGHT_LONG_PRESS:
            if (btn_value) {
                scroll_sim_mode = false;
                scroll_sim_mode_pending = true;
                beep();
            }
            break;
        case RIGHT_CLICK:
            if (scroll_sim_mode_repeat == SCROLL_REPEAT_NONE) {
                roller_up();
            }
            if (btn_value)
                scroll_sim_mode_repeat = SCROLL_REPEAT_UP;
            else
                scroll_sim_mode_repeat = SCROLL_REPEAT_NONE;
            break;
        case RIGHT_DOUBLE_CLICK:
            if (scroll_sim_mode_repeat == SCROLL_REPEAT_NONE) {
                roller_down();
            }
            if (btn_value)
                scroll_sim_mode_repeat = SCROLL_REPEAT_DOWN;
            else
                scroll_sim_mode_repeat = SCROLL_REPEAT_NONE;
            break;
        default:
            break;
        }
    } else if (g_setting.ease.no_dial && btn_value) {
        scroll_sim_mode = true;
        scroll_sim_mode_pending = true;
        beep();
    } else {

        pthread_mutex_lock(&lvgl_mutex);

        switch (g_app_state) {
        case APP_STATE_SUBMENU:
        case APP_STATE_WIFI:
            if (click_type == RIGHT_CLICK)
                submenu_right_button(true);
            else if (click_type == RIGHT_LONG_PRESS)
                submenu_right_button(false);
            break;
        case APP_STATE_PLAYBACK:
            if (click_type == RIGHT_CLICK)
                pb_key(RIGHT_KEY_CLICK);
            else if (click_type == RIGHT_LONG_PRESS)
                pb_key(RIGHT_KEY_PRESS);
            break;
        case APP_STATE_VIDEO:
            if (click_type == RIGHT_CLICK) {
                (*rbtn_click_callback)();
            } else if (click_type == RIGHT_LONG_PRESS) {
                (*rbtn_press_callback)();
            } else if (click_type == RIGHT_DOUBLE_CLICK) {
                (*rbtn_double_click_callback)();
            }
            break;
        case APP_STATE_SLEEP:
            wake_up();
            break;
        }

        pthread_mutex_unlock(&lvgl_mutex);
    }
}

static void roller_up(void) {
    LOGI("roller up (%d)", g_app_state);

    if (g_scanning)
        return;

    if (g_init_done == 0) // disable roller before init done
        return;

    if (g_app_state == APP_STATE_USER_INPUT_DISABLED)
        return;

    pthread_mutex_lock(&lvgl_mutex);
    autoscan_exit();
    if (g_app_state == APP_STATE_MAINMENU) // main menu
    {
        menu_nav(DIAL_KEY_UP);
    } else if (g_app_state == APP_STATE_SUBMENU ||
               g_app_state == APP_STATE_PLAYBACK ||
               g_app_state == APP_STATE_WIFI) {
        submenu_roller(DIAL_KEY_UP);
    } else if ((g_app_state == APP_STATE_SUBMENU_ITEM_FOCUSED)) {
        submenu_roller_no_selection_change(DIAL_KEY_UP);
    } else if (g_app_state == APP_STATE_VIDEO) {
        (*roller_callback)(DIAL_KEY_UP);
    } else if (g_app_state == APP_STATE_IMS) {
        ims_key(DIAL_KEY_UP);
    } else if (g_app_state == APP_STATE_OSD_ELEMENT_PREV) {
        ui_osd_element_pos_handle_input(DIAL_KEY_UP);
    } else if (g_app_state == APP_STATE_SLEEP) {
        wake_up();
    }
    pthread_mutex_unlock(&lvgl_mutex);
}

static void roller_down(void) {
    LOGI("roller down (%d)", g_app_state);

    if (g_scanning)
        return;

    if (g_init_done == 0) // disable roller before init done
        return;

    if (g_app_state == APP_STATE_USER_INPUT_DISABLED)
        return;

    pthread_mutex_lock(&lvgl_mutex);
    autoscan_exit();
    if (g_app_state == APP_STATE_MAINMENU) {
        menu_nav(DIAL_KEY_DOWN);
    } else if (g_app_state == APP_STATE_SUBMENU ||
               g_app_state == APP_STATE_PLAYBACK ||
               g_app_state == APP_STATE_WIFI) {
        submenu_roller(DIAL_KEY_DOWN);
    } else if ((g_app_state == APP_STATE_SUBMENU_ITEM_FOCUSED)) {
        submenu_roller_no_selection_change(DIAL_KEY_DOWN);
    } else if (g_app_state == APP_STATE_VIDEO) {
        (*roller_callback)(DIAL_KEY_DOWN);
    } else if (g_app_state == APP_STATE_IMS) {
        ims_key(DIAL_KEY_DOWN);
    } else if (g_app_state == APP_STATE_OSD_ELEMENT_PREV) {
        ui_osd_element_pos_handle_input(DIAL_KEY_DOWN);
    } else if (g_app_state == APP_STATE_SLEEP) {
        wake_up();
    }
    pthread_mutex_unlock(&lvgl_mutex);
}

static void get_event(int fd) {
    struct input_event event;
    static int event_type_last = 0;
    static int btn_press_time = 0;

    static int roller_value = 0;

    // time (sec, usec) difference above which will the next scroll wheel event be accepted
    // 10000 usec = 10msec is more than short enough (100Hz)
    // scroll events
    const struct timeval scroll_time_diff = {0, 10000};
    // direction change events
    const struct timeval rel_time_diff = {0, 20000};
    // expected timestamp in the future beyond that will the events be accepted
    static struct timeval next_scroll = {0, 0};
    static struct timeval next_rel = {0, 0};
    static struct timeval last_wall_time = {0, 0};
    static bool discard_scroll = false;

    read(fd, &event, sizeof(event));

    // Setting the RTC backwards can also move evdev timestamps backwards on
    // kernels that do not support EVIOCSCLOCKID. Reset the absolute debounce
    // deadlines before classifying this event so wheel input is not discarded
    // until wall time catches up.
    struct timeval wall_time;
    gettimeofday(&wall_time, NULL);
    if (timerisset(&last_wall_time) && timercmp(&wall_time, &last_wall_time, <)) {
        timerclear(&next_scroll);
        timerclear(&next_rel);
        discard_scroll = false;
        roller_up_acc = 0;
        roller_down_acc = 0;
    }
    last_wall_time = wall_time;

    switch (event.type) {
    case EV_SYN:
        if (event.code == SYN_REPORT) {
            if (event_type_last == EV_REL) {
                if (g_setting.ease.no_dial)
                    break;

                if (!discard_scroll && timercmp(&event.time, &next_scroll, >)) {
                    timeradd(&event.time, &scroll_time_diff, &next_scroll);
                    if (roller_value == 1) {
                        roller_up_acc++;
                        roller_down_acc = 0;
                    } else if (roller_value == -1) {
                        roller_down_acc++;
                        roller_up_acc = 0;
                    }

                    if (roller_up_acc == DIAL_SENSITIVITY) {
                        roller_up();
                        g_key = DIAL_KEY_UP;
                        roller_up_acc = 0;
                    } else if (roller_down_acc == DIAL_SENSITIVITY) {
                        roller_down();
                        g_key = DIAL_KEY_DOWN;
                        roller_down_acc = 0;
                    }
                } else {
                    // LOGI("discard EV_SYN");
                }
            } else if (event_type_last == EV_KEY) {
                if (btn_value) {
                    if (!g_setting.ease.no_dial) {
                        if (btn_press_time == 10) {
                            btn_press();
                            g_key = DIAL_KEY_PRESS;
                        }
                    } else {
                        if (scroll_sim_mode_repeat == SCROLL_REPEAT_DOWN) {
                            roller_down();
                        } else if (scroll_sim_mode_repeat == SCROLL_REPEAT_UP) {
                            roller_up();
                        }
                    }
                    if (scroll_sim_mode_pending)
                        btn_press_time = 0;
                    else
                        btn_press_time++;
                    // LOGI("btn down");
                } else {
                    if (scroll_sim_mode_pending) {
                        scroll_sim_mode_pending = false;
                    } else {
                        if (scroll_sim_mode_repeat == SCROLL_REPEAT_NONE) {
                            if (btn_press_time < 10) {
                                btn_click();
                                g_key = DIAL_KEY_CLICK;
                            } else if (g_setting.ease.no_dial) {
                                if (btn_press_time < 50) {
                                    btn_press();
                                    g_key = DIAL_KEY_PRESS;
                                }
                                // else if(btn_press_time > 200){
                                //	btn_super_press();
                                // }
                            }
                        }
                    }
                    btn_press_time = 0;
                }
            }
            // LOGI("------------ syn report ----------");
        } else if (event.code == SYN_MT_REPORT) {
            // LOGI("----------- syn mt report ------------");
        }
        break;
    case EV_KEY:
        // LOGI("key code%d is %s!", event.code, event.value?"down":"up");
        btn_value = event.value;
        event_type_last = EV_KEY;
        break;
    case EV_ABS:
        if ((event.code == ABS_X) ||
            (event.code == ABS_MT_POSITION_X)) {
            // LOGI("abs,x = %d", event.value);
        } else if ((event.code == ABS_Y) ||
                   (event.code == ABS_MT_POSITION_Y)) {
            // LOGI("abs,y = %d", event.value);
        } else if ((event.code == ABS_PRESSURE) ||
                   (event.code == ABS_MT_PRESSURE)) {
            // LOGI("pressure value: %d", event.value);
        }
        break;
    case EV_REL:
        if (timercmp(&event.time, &next_rel, >)) {
            discard_scroll = false;
            if (event.code == REL_X) {
                // LOGI("x = %d", event.value);
            } else if (event.code == REL_Y) {
                if (roller_value != event.value) {
                    timeradd(&event.time, &rel_time_diff, &next_rel);
                    roller_value = event.value;
                    // LOGI("y = %d", event.value);
                }
                event_type_last = EV_REL;
            }
        } else {
            discard_scroll = true;
            LOGI("discard EV_REL");
        }
        break;
    default:
        // LOGI("unknown [type=%d, code=%d value=%d]", event.type, event.code, event.value);
        break;
    }
}

static void add_to_epfd(int epfd, int fd) {
    struct epoll_event event = {
        .events = EPOLLIN,
        .data = {
            .fd = fd,
        },
    };

    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event);
    assert(ret == 0);
}

static void *thread_input_device(void *ptr) {
#ifndef EMULATOR_BUILD
    for (;;) {
        struct epoll_event events[EPOLL_FD_CNT];

        int ret = epoll_wait(epfd, events, EPOLL_FD_CNT, DIAL_SENSITIVTY_TIMEOUT_MS);
        if (ret < 0) {
            perror("epoll_wait");
            continue;
        }

        if (ret > 0) {
            for (int i = 0; i < ret; i++) {
                if (events[i].events & EPOLLIN) {
                    get_event(events[i].data.fd);
                }
            }
        } else {
            roller_up_acc = 0;
            roller_down_acc = 0;
            if (scroll_sim_mode_repeat != SCROLL_REPEAT_NONE)
                beep();
        }
    }
    return NULL;
#else
    static uint32_t btn_d_start = 0;
    static uint32_t btn_a_start = 0;

    while (true) {
        SDL_Event event;
        SDL_LockMutex(global_sdl_mutex);
        while (SDL_PollEvent(&event)) {
            SDL_UnlockMutex(global_sdl_mutex);
            switch (event.type) {
            case SDL_QUIT:
                exit(0);

            case SDL_KEYDOWN:
                switch (event.key.keysym.sym) {
                case SDLK_d:
                    if (!btn_d_start) {
                        btn_d_start = event.key.timestamp;
                    }
                    break;

                case SDLK_a:
                    if (!btn_a_start) {
                        btn_a_start = event.key.timestamp;
                    }
                    break;
                }
                break;

            case SDL_KEYUP:
                switch (event.key.keysym.sym) {
                case SDLK_s:
                    roller_up();
                    g_key = DIAL_KEY_UP;
                    break;

                case SDLK_w:
                    roller_down();
                    g_key = DIAL_KEY_DOWN;
                    break;

                case SDLK_d:
                    if (event.key.timestamp - btn_d_start > 500) {
                        btn_press();
                        g_key = DIAL_KEY_PRESS;
                    } else {
                        btn_click();
                        g_key = DIAL_KEY_CLICK;
                    }
                    btn_d_start = 0;
                    break;

                case SDLK_a:
                    if (event.key.timestamp - btn_a_start > 500) {
                        rbtn_click(RIGHT_LONG_PRESS);
                        g_key = RIGHT_KEY_PRESS;
                    } else {
                        rbtn_click(RIGHT_CLICK);
                        g_key = RIGHT_KEY_CLICK;
                    }
                    btn_a_start = 0;
                    break;
                }
                break;
            }
            SDL_LockMutex(global_sdl_mutex);
        }
        SDL_UnlockMutex(global_sdl_mutex);
        usleep(50000); // Sorry, this will break windows, but it's not like it is working now anyway :-(
    }
#endif
}

void input_device_init() {
#ifndef EMULATOR_BUILD
    epfd = epoll_create(EPOLL_FD_CNT);
    assert(epfd > 0);

    char buf[64];
    for (int i = 0; i < EPOLL_FD_CNT; i++) {
        snprintf(buf, 64, "/dev/input/event%d", i);

        int fd = open(buf, O_RDONLY);
        if (fd >= 0) {
            // Keep input timestamps independent of RTC changes made by the
            // clock page or an external time source.
            int clkid = CLOCK_MONOTONIC;
            if (ioctl(fd, EVIOCSCLOCKID, &clkid) != 0) {
                LOGI("EVIOCSCLOCKID unsupported on %s", buf);
            }
            add_to_epfd(epfd, fd);
            LOGI("opened %s", buf);
        }
    }
    app_state_push(APP_STATE_MAINMENU);
#else
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("Error initializing SDL: %s\n", SDL_GetError());
    }
#endif
    pthread_create(&input_device_pid, NULL, thread_input_device, NULL);
}
