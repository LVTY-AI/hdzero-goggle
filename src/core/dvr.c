#include "dvr.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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

static void dvr_update_record_conf() {
    int bitrate_scale;
    switch (g_setting.record.bitrate_scale) {
    case SETTING_RECORD_BITRATE_SCALE_NORMAL:
        bitrate_scale = 1;
        break;
    case SETTING_RECORD_BITRATE_SCALE_HALF:
        bitrate_scale = 2;
        break;
    case SETTING_RECORD_BITRATE_SCALE_QUARTER:
        bitrate_scale = 4;
        break;
    default:
        bitrate_scale = 1;
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
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
        } else if (CAM_MODE == VR_1080P24) { // 1080p24
            ini_putl("venc", "fps", 50, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
        } else if (CAM_MODE == VR_540P90 || CAM_MODE == VR_540P90_CROP) { // 90fps
            ini_putl("venc", "fps", 90, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
        } else {
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 24000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 1, REC_CONF);
        }
    } else if (g_source_info.source == SOURCE_AV_IN || g_source_info.source == SOURCE_AV_MODULE) { // Analog
        ini_putl("venc", "width", 1280, REC_CONF);
        ini_putl("venc", "height", 720, REC_CONF);

        ini_putl("venc", "kbps", 24000 / bitrate_scale, REC_CONF);
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
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_1080P50:
            ini_putl("venc", "width", 1920, REC_CONF);
            ini_putl("venc", "height", 1080, REC_CONF);
            ini_putl("venc", "fps", 50, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_1080Pother:
            ini_putl("venc", "width", 1920, REC_CONF);
            ini_putl("venc", "height", 1080, REC_CONF);
            ini_putl("venc", "fps", 50, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_720P50:
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
            ini_putl("venc", "fps", 50, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_720P60:
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        case HDMIIN_VTMG_720P100:
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
            ini_putl("venc", "fps", 90, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        default:
            ini_putl("venc", "width", 1280, REC_CONF);
            ini_putl("venc", "height", 720, REC_CONF);
            ini_putl("venc", "fps", 60, REC_CONF);
            ini_putl("venc", "kbps", 34000 / bitrate_scale, REC_CONF);
            ini_putl("venc", "h265", 0, REC_CONF);
            break;
        }
    }

    ini_putl("record", "audio", g_setting.record.audio, REC_CONF);
    dvr_select_audio_source(g_setting.record.audio_source);
    ini_putl("record", "naming", g_setting.record.naming, REC_CONF);

    sync();
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
            system_script(REC_STOP);
            sleep(2); // wait for record process
        }
    }

    pthread_mutex_unlock(&dvr_mutex);
}
