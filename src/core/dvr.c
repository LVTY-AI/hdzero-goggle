#include "dvr.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "../conf/targets.h"

#include "core/msp_displayport.h"
#include "core/settings.h"
#include "driver/hardware.h"
#include "record/record_definitions.h"
#include "ui/page_common.h"
#include "util/sdcard.h"
#include "util/system.h"

bool dvr_is_recording = false;
bool record_pending = false;

static time_t dvr_recording_start = 0;
static pthread_mutex_t dvr_mutex;
static bool live_audio_line_out_enabled = false;
static bool live_audio_muted_for_dvr = false;

// Race label for the next recording, pushed by a race timer over the ELRS
// backpack (MSP_SET_DVR_NAME). It expires so a recording made well after the
// heat (e.g. freestyle) doesn't inherit the race's name.
#define DVR_RACE_LABEL_TTL_S (5 * 60)
static char dvr_race_label[REC_labelMAXLEN] = "";
static time_t dvr_race_label_time = 0;

// Store a filename-safe copy of the label: alnum kept, spaces become '-',
// everything else dropped, capped to REC_labelMAXLEN. Empty input clears it.
void dvr_set_race_label(const uint8_t *label, uint16_t len) {
    char clean[REC_labelMAXLEN];
    size_t n = 0;

    for (uint16_t i = 0; i < len && n < sizeof(clean) - 1; i++) {
        char c = (char)label[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_') {
            clean[n++] = c;
        } else if (c == ' ' && n > 0 && clean[n - 1] != '-') {
            clean[n++] = '-';
        }
    }
    clean[n] = 0;

    pthread_mutex_lock(&dvr_mutex);
    if (g_setting.record.naming != SETTING_NAMING_ELRS) {
        dvr_race_label[0] = 0;
        dvr_race_label_time = 0;
        pthread_mutex_unlock(&dvr_mutex);
        return;
    }
    strcpy(dvr_race_label, clean);
    dvr_race_label_time = time(NULL);
    pthread_mutex_unlock(&dvr_mutex);
    LOGI("dvr race label set to \"%s\"", clean);
}

void dvr_clear_race_label(void) {
    pthread_mutex_lock(&dvr_mutex);
    dvr_race_label[0] = 0;
    dvr_race_label_time = 0;
    pthread_mutex_unlock(&dvr_mutex);
}

///////////////////////////////////////////////////////////////////
//-1=error;
// 0=idle,1=recording,2=stopped,3=No SD card,4=recorf file path error,
// 5=SD card Full,6=Encoder error,7=Open Record File Failed
void dvr_update_status() {
    pthread_mutex_lock(&dvr_mutex);
    if (dvr_is_recording) {
        int ret = -1;
        FILE *fp = fopen("/tmp/record.dat", "r");
        if (fp) {
            fscanf(fp, "%d", &ret);
            fclose(fp);
        }
        if (ret != 1) {
            dvr_is_recording = false;
            system_script(REC_STOP);
            record_pending = false;
            sleep(2);                         // wait for record process
        }
    }
    pthread_mutex_unlock(&dvr_mutex);
}

void dvr_enable_line_out(bool enable) {
    char buf[128];
    if (enable) {
        live_audio_line_out_enabled = true;
        live_audio_muted_for_dvr = false; // path below is fully reopened
        snprintf(buf, sizeof(buf), "%s out_on", AUDIO_SEL_SH);
        system_exec(buf);
        dvr_set_live_audio_volume(g_setting.record.live_audio_volume);
        snprintf(buf, sizeof(buf), "%s out_linein_on", AUDIO_SEL_SH);
        system_exec(buf);
        snprintf(buf, sizeof(buf), "%s out_dac_off", AUDIO_SEL_SH);
        system_exec(buf);
    } else {
        live_audio_line_out_enabled = false;
        live_audio_muted_for_dvr = false;
        snprintf(buf, sizeof(buf), "%s out_off", AUDIO_SEL_SH);
        system_exec(buf);
    }
}

bool dvr_live_audio_is_enabled(void) {
    return live_audio_line_out_enabled;
}

static int clamp_audio_volume(int volume) {
    if (volume < 0)
        return 0;
    if (volume > 10)
        return 10;
    return volume;
}

static int dvr_volume_to_hardware(int volume) {
    static const int volume_table[] = {0, 26, 28, 29, 29, 30, 30, 31, 31};

    if (volume > 8)
        volume = 8;
    return volume_table[volume];
}

static int live_volume_to_lineout(int volume) {
    return (volume * 31 + 5) / 10;
}

void dvr_set_dvr_audio_volume(int volume) {
    char buf[128];
    int hardware_volume;
    int dac_volume;

    volume = clamp_audio_volume(volume);

    hardware_volume = dvr_volume_to_hardware(volume);
    if (volume == 0)
        dac_volume = 0;
    else if (volume == 4)
        dac_volume = 153;
    else if (volume == 6)
        dac_volume = 157;
    else if (volume == 7)
        dac_volume = 158;
    else
        dac_volume = (hardware_volume * 160 + 15) / 31;

    snprintf(buf, sizeof(buf), "amixer cset name='lineout volume' %d", hardware_volume);
    system_exec(buf);
    snprintf(buf, sizeof(buf), "amixer cset name='DAC volume' %d,%d", dac_volume, dac_volume);
    system_exec(buf);
    snprintf(buf, sizeof(buf), "amixer cset name='AIF1 DAC timeslot 0 volume' %d,%d", dac_volume, dac_volume);
    system_exec(buf);
    snprintf(buf, sizeof(buf), "amixer cset name='AIF1 DAC timeslot 1 volume' %d,%d", dac_volume, dac_volume);
    system_exec(buf);
}

void dvr_set_live_audio_volume(int volume) {
    char buf[128];
    int lineout_volume;
    int linein_gain;

    volume = clamp_audio_volume(volume);
    lineout_volume = live_volume_to_lineout(volume);
    linein_gain = (volume * volume * volume * 7 + 500) / 1000;
    if (volume > 0 && linein_gain == 0)
        linein_gain = 1;

    snprintf(buf, sizeof(buf), "amixer cset name='lineout volume' %d", lineout_volume);
    system_exec(buf);
    snprintf(buf, sizeof(buf), "amixer cset name='LINEINL/R to L_R output mixer gain' %d", linein_gain);
    system_exec(buf);
}

static int clamp_record_gain(int gain) {
    if (gain < 0)
        return 0;
    if (gain > 7)
        return 7;
    return gain;
}

void dvr_set_mic_gain(int gain) {
    char buf[128];

    gain = clamp_record_gain(gain);
    snprintf(buf, sizeof(buf), "amixer cset name='MIC1 boost AMP gain control' %d", gain);
    system_exec(buf);
}

void dvr_set_linein_gain(int gain) {
    char buf[128];

    gain = clamp_record_gain(gain);
    snprintf(buf, sizeof(buf), "amixer cset name='ADC input gain control' %d", gain);
    system_exec(buf);
    snprintf(buf, sizeof(buf), "amixer cset name='MIC2 boost AMP gain control' %d", gain);
    system_exec(buf);
}

static void dvr_apply_record_audio_gain(uint8_t source) {
    if (source == SETTING_RECORD_AUDIO_SOURCE_MIC) {
        dvr_set_mic_gain(g_setting.record.mic_gain);
    } else {
        dvr_set_linein_gain(g_setting.record.linein_gain);
    }
}

void dvr_mute_live_audio(void) {
    char buf[128];

    // Unconditional on purpose: live_audio_line_out_enabled can go stale
    // (boot never sets it, and a source switch mid audio-test can desync it),
    // and a skipped mute here means live audio mixes into DVR playback.
    // Cutting LINEIN with the line out already off is harmless.
    snprintf(buf, sizeof(buf), "%s out_linein_off", AUDIO_SEL_SH);
    system_exec(buf);
    system_exec("amixer cset name='LINEINL/R to L_R output mixer gain' 0");
    live_audio_muted_for_dvr = true;
}

void dvr_restore_live_audio(void) {
    char buf[128];

    if (!live_audio_muted_for_dvr || !live_audio_line_out_enabled)
        return;

    dvr_set_live_audio_volume(g_setting.record.live_audio_volume);
    snprintf(buf, sizeof(buf), "%s out_linein_on", AUDIO_SEL_SH);
    system_exec(buf);
    live_audio_muted_for_dvr = false;
}

void dvr_select_audio_source(uint8_t source) {
    char buf[128];
    char *audio_source[3] = {
        "in_mic1",
        "in_mic2",
        "in_linein"};

    if (source > 2)
        source = 2;
    snprintf(buf, sizeof(buf), "%s %s", AUDIO_SEL_SH, audio_source[source]);
    system_exec(buf);
    dvr_apply_record_audio_gain(source);
}

// video input config
void dvr_update_vi_conf(video_resolution_t fmt) {
    pthread_mutex_lock(&dvr_mutex);
    switch (fmt) {
    case VR_720P50:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 50, REC_CONF);
        break;
    case VR_720P60:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 60, REC_CONF);
        break;
    case VR_720P30:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 30, REC_CONF);
        break;
    case VR_540P90:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 90, REC_CONF);
        break;
    case VR_540P60:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 60, REC_CONF);
        break;
    case VR_960x720P60:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 60, REC_CONF);
        break;
    case VR_540P90_CROP:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 90, REC_CONF);
        break;
#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    case VR_1080P30:
        ini_putl("vi", "width", 1920, REC_CONF);
        ini_putl("vi", "height", 1080, REC_CONF);
        ini_putl("vi", "fps", 30, REC_CONF);
        break;
    case VR_1080P24:
        ini_putl("vi", "width", 1920, REC_CONF);
        ini_putl("vi", "height", 1080, REC_CONF);
        ini_putl("vi", "fps", 50, REC_CONF);
        break;
    case VR_1080P50:
        ini_putl("vi", "width", 1920, REC_CONF);
        ini_putl("vi", "height", 1080, REC_CONF);
        ini_putl("vi", "fps", 50, REC_CONF);
        break;
    case VR_1080P60:
        ini_putl("vi", "width", 1920, REC_CONF);
        ini_putl("vi", "height", 1080, REC_CONF);
        ini_putl("vi", "fps", 59, REC_CONF); // If set fps to 60, DVR is wrong. I don't know why. 59 or 61 is ok.
        break;
#elif defined HDZBOXPRO
    case VR_1080P30:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 60, REC_CONF);
        break;
    case VR_1080P24:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 50, REC_CONF);
        break;
    case VR_1080P50:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 50, REC_CONF);
        break;
    case VR_1080P60:
        ini_putl("vi", "width", 1280, REC_CONF);
        ini_putl("vi", "height", 720, REC_CONF);
        ini_putl("vi", "fps", 59, REC_CONF); // If set fps to 60, DVR is wrong. I don't know why. 59 or 61 is ok.
        break;
#endif
    }
    pthread_mutex_unlock(&dvr_mutex);
    sync();

    LOGI("update_record_vi_conf: fmt=%d", fmt);
}

void dvr_toggle() {
    dvr_cmd(DVR_TOGGLE);
}

void dvr_star() {
    pthread_mutex_lock(&dvr_mutex);
    if (dvr_is_recording) {
        char current_dvr_file[256] = "";
        FILE *now_recording_file = fopen(NOW_RECORDING_FILE, "r");
        if (now_recording_file) {
            const size_t read_count = fread(current_dvr_file, 1, sizeof(current_dvr_file) - 1, now_recording_file);
            if (ferror(now_recording_file) == 0) {
                current_dvr_file[read_count] = '\0';
                strcat(current_dvr_file, REC_starSUFFIX);
                FILE *like_file = fopen(current_dvr_file, "a");
                if (like_file) {
                    unsigned recording_duration_s = time(NULL) - dvr_recording_start;
                    unsigned minutes = recording_duration_s / 60;
                    unsigned seconds = recording_duration_s % 60;
                    fprintf(like_file, REC_starFORMAT, minutes, seconds);
                    fclose(like_file);
                }
            }
            fclose(now_recording_file);
        }
    }
    pthread_mutex_unlock(&dvr_mutex);
}

// Append one line to a persistent DVR-format trace on the SD card so the rare
// "MP4 setting recorded as .ts" handoff failure can be diagnosed from logs
// after the fact. Best-effort: silently skipped when no card is mounted.
static void dvr_dbg_log(const char *fmt, ...) {
    FILE *fp = fopen("/mnt/extsd/dvr_dbg.log", "a");
    if (fp == NULL)
        return;

    time_t now = time(NULL);
    char ts[20];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(fp, "%s [app] ", ts);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
}

static void dvr_update_record_conf() {
    int bitrate_num = 1;
    int bitrate_den = 1;
    switch (g_setting.record.bitrate_scale) {
    case SETTING_RECORD_BITRATE_SCALE_NORMAL:
        break;
    case SETTING_RECORD_BITRATE_SCALE_HALF:
        bitrate_den = 2;
        break;
    case SETTING_RECORD_BITRATE_SCALE_QUARTER:
        bitrate_den = 4;
        break;
    case SETTING_RECORD_BITRATE_SCALE_HIGH:
        bitrate_num = 3;
        bitrate_den = 2;
        break;
    case SETTING_RECORD_BITRATE_SCALE_MAX:
        bitrate_num = 2;
        break;
    default:
        break;
    }
    if (g_setting.record.format_ts)
        ini_puts("record", "type", "ts", REC_CONF);
    else
        ini_puts("record", "type", "mp4", REC_CONF);

    if (g_source_info.source == SOURCE_HDZERO) {
        LOGI("CAM_MODE=%d", CAM_MODE);
        if (CAM_MODE == VR_1080P30 || CAM_MODE == VR_1080P24) {
            ini_putl("venc", "width", 1920, REC_CONF);
            ini_putl("venc", "height", 1080, REC_CONF);
        } else {
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
        }

        if (CAM_MODE == VR_1080P30) { // 1080p30
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 34000 * bitrate_num / bitrate_den, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
        } else if (CAM_MODE == VR_1080P24) { // 1080p24
            ini_putl("venc", "fps", 50, REC_CONF);
            ini_putl("venc", "kbps", 34000 * bitrate_num / bitrate_den, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
        } else if (CAM_MODE == VR_540P90 || CAM_MODE == VR_540P90_CROP) { // 90fps
            ini_putl("venc", "fps", 90, REC_CONF);
            ini_putl("venc", "kbps", 34000 * bitrate_num / bitrate_den, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
        } else {
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 24000 * bitrate_num / bitrate_den, REC_CONF);
            ini_putl("venc", "h265", 1, REC_CONF);
        }
    } else if (g_source_info.source == SOURCE_AV_IN || g_source_info.source == SOURCE_AV_MODULE) { // Analog
        ini_putl("venc", "width", 1280, REC_CONF);
        ini_putl("venc", "height", 720, REC_CONF);

        ini_putl("venc", "kbps", 24000 * bitrate_num / bitrate_den, REC_CONF);
        ini_putl("venc", "h265", 1, REC_CONF);
        if (g_hw_stat.av_pal[g_hw_stat.is_av_in])
            ini_putl("venc", "fps", 50, REC_CONF);
        else
            ini_putl("venc", "fps", 60, REC_CONF);
    } else if (g_source_info.source == SOURCE_HDMI_IN) {
        LOGI("g_hw_stat.hdmiin_vtmg=%d", g_hw_stat.hdmiin_vtmg);
        switch (g_hw_stat.hdmiin_vtmg) {
        case HDMIIN_VTMG_1080P60:
            ini_putl("venc", "width", 1920, REC_CONF);
            ini_putl("venc", "height", 1080, REC_CONF);
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 34000 * bitrate_num / bitrate_den, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_1080P50:
            ini_putl("venc", "width", 1920, REC_CONF);
            ini_putl("venc", "height", 1080, REC_CONF);
            ini_putl("venc", "fps", 50, REC_CONF);
            ini_putl("venc", "kbps", 34000 * bitrate_num / bitrate_den, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_1080Pother:
            ini_putl("venc", "width", 1920, REC_CONF);
            ini_putl("venc", "height", 1080, REC_CONF);
            ini_putl("venc", "fps", 50, REC_CONF);
            ini_putl("venc", "kbps", 34000 * bitrate_num / bitrate_den, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_720P50:
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
            ini_putl("venc", "fps", 50, REC_CONF);
            ini_putl("venc", "kbps", 34000 * bitrate_num / bitrate_den, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_720P60:
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 34000 * bitrate_num / bitrate_den, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_720P100:
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
            ini_putl("venc", "fps", 90, REC_CONF);
            ini_putl("venc", "kbps", 34000 * bitrate_num / bitrate_den, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        default:
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 34000 * bitrate_num / bitrate_den, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        }
    }

    // The recorder treats kbps as a target under CBR and as a ceiling under
    // VBR. Keep CBR as the default and apply the user-controlled VBR quality
    // and QP floor to both codecs so changing source or codec cannot leave
    // stale encoder parameters behind.
    bool const use_vbr = (g_setting.record.rc_mode == SETTING_RECORD_RC_VBR);
    ini_putl("venc", "rc", use_vbr ? 1 : 0, REC_CONF);
    for (int i = 0; i < 2; i++) {
        const char *sec = i ? "h265" : "h264";
        if (use_vbr) {
            ini_putl(sec, "quality", g_setting.record.vbr_quality, REC_CONF);
            ini_putl(sec, "maxQP", g_setting.record.vbr_max_qp, REC_CONF);
        } else {
            ini_putl(sec, "maxQP", 52, REC_CONF);
        }
    }

    ini_putl("record", "audio", g_setting.record.audio, REC_CONF);
    dvr_select_audio_source(g_setting.record.audio_source);
    ini_putl("record", "naming", g_setting.record.naming, REC_CONF);

    // Only the ELRS naming scheme consumes race labels. Clear the pending
    // label when another scheme is selected so it cannot leak into a later
    // non-ELRS recording.
    if (g_setting.record.naming != SETTING_NAMING_ELRS) {
        dvr_race_label[0] = 0;
    } else if (dvr_race_label[0] && time(NULL) - dvr_race_label_time > DVR_RACE_LABEL_TTL_S) {
        dvr_race_label[0] = 0;
    }
    ini_puts("record", "label", g_setting.record.naming == SETTING_NAMING_ELRS ? dvr_race_label : "", REC_CONF);

    sync();

    // The record daemon picks the clip container purely from this "type" key
    // and silently falls back to "ts" if the value is missing or unrecognised.
    // A lost or raced write therefore turns an MP4 recording into a .ts file,
    // so confirm the intended value is actually on disk before we hand off to
    // the daemon, and re-write it if it is not.
    const char *want_type = g_setting.record.format_ts ? "ts" : "mp4";
    char got_type[8] = "";
    int type_retries = 0;
    while (true) {
        ini_gets("record", "type", "", got_type, sizeof(got_type), REC_CONF);
        if (strcmp(got_type, want_type) == 0)
            break;
        if (type_retries == 3)
            break;
        ini_puts("record", "type", want_type, REC_CONF);
        sync();
        type_retries++;
    }
    dvr_dbg_log("update_conf format_ts=%d want=%s got=%s retries=%d",
                g_setting.record.format_ts, want_type, got_type, type_retries);
}

void dvr_cmd(osd_dvr_cmd_t cmd) {
    LOGI("dvr_cmd: sdcard=%d, recording=%d, cmd=%d", g_sdcard_enable, dvr_is_recording, cmd);

    bool start_rec = dvr_is_recording;

    switch (cmd) {
    case DVR_TOGGLE:
        start_rec = !dvr_is_recording && !record_pending;
        break;
    case DVR_STOP:
        start_rec = false;
        break;
    case DVR_START:
        start_rec = true;
        break;
    }

    if (!g_sdcard_enable) {
        record_pending = start_rec;
        return;
    }

    pthread_mutex_lock(&dvr_mutex);

    if (start_rec) {
        if (!dvr_is_recording && !sdcard_is_full()) {
            dvr_update_record_conf();
            // Re-assert the record-OSD bit at the exact point recording starts.
            // Display/UI transitions can reinitialize this FPGA register after
            // source entry, so the source-entry write alone is not sufficient.
            //
            // Not on HDMI In on the goggles: Source_HDMI_in() runs that session
            // with the overlay plane switched off, and re-asserting the bit here
            // turned it back on part-way through -- an overlay the rest of the
            // session is not set up for.
            bool reassert_osd = true;
#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
            reassert_osd = (g_source_info.source != SOURCE_HDMI_IN);
#endif
            if (reassert_osd) {
                Display_Osd(g_setting.record.osd);
                uint8_t osd_reg = Display_Osd_Readback();
                LOGI("dvr record OSD: requested=%d readback=0x%02x",
                     g_setting.record.osd, osd_reg);
            }
            if (g_sdcard_ready) {
                dvr_is_recording = true;
                record_pending = false;
                usleep(100 * 1000);
                system_script(REC_START);
                dvr_recording_start = time(NULL);
                sleep(2); // wait for record process
            } else {
                record_pending = true;
            }
        }
    } else {
        if (dvr_is_recording) {
            dvr_is_recording = false;
            dvr_dbg_log("stop_request cmd=%d", cmd);
            system_script(REC_STOP);
            sleep(2); // wait for record process
        }
    }

    pthread_mutex_unlock(&dvr_mutex);
}
