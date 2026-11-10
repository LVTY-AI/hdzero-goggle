#include "dvr.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/vfs.h>
#endif

#include <log/log.h>
#include <log/dvr_trace.h>
#include <log/record_control.h>
#include <minIni.h>

#include "../conf/targets.h"

#include "core/msp_displayport.h"
#include "core/app_state.h"
#include "core/settings.h"
#include "driver/hardware.h"
#include "driver/rtc.h"
#include "record/record_definitions.h"
#include "ui/page_common.h"
#include "util/sdcard.h"
#include "util/system.h"

dvr_atomic_bool dvr_is_recording = false;
dvr_atomic_bool record_pending = false;

static time_t dvr_recording_start = 0;
static pthread_mutex_t dvr_mutex = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    setting_record_t record;
    video_resolution_t camera;
    int source, pal, hdmi;
    char label[REC_labelMAXLEN];
} dvr_snapshot_t;
static struct {
    bool desired, live_stop, pause, paused, resuming, star, initialized, confirmed_idle;
    unsigned stop, stopped, vi_generation;
    video_resolution_t vi;
    dvr_snapshot_t snapshot;
    void (*resume)(uintptr_t);
    uintptr_t argument;
} control;
static atomic_uint control_phase, control_error;
static char runtime_conf[RECORD_CONTROL_PATH];
static char pending_conf[RECORD_CONTROL_PATH];
static const char *config_file = REC_CONF;
enum { DVR_OFFLINE, DVR_IDLE, DVR_CONFIGURING, DVR_WAITING };

static bool live_audio_line_out_enabled = false;
static bool live_audio_muted_for_dvr = false;

// Race label for the next recording, pushed by a race timer over the ELRS
// backpack (MSP_SET_DVR_NAME). It expires so a recording made well after the
// heat (e.g. freestyle) doesn't inherit the race's name.
#define DVR_RACE_LABEL_TTL_S (5 * 60)
static char dvr_race_label[REC_labelMAXLEN] = "";
static time_t dvr_race_label_time = 0;

static dvr_snapshot_t dvr_snapshot(void) {
    dvr_snapshot_t snapshot = {.record = g_setting.record, .camera = CAM_MODE,
        .source = g_source_info.source, .pal = g_hw_stat.av_pal[g_hw_stat.is_av_in],
        .hdmi = g_hw_stat.hdmiin_vtmg};
    if (snapshot.record.naming == SETTING_NAMING_DATE) {
        struct rtc_date rd;
        rtc_get_clock(&rd);
        if (rd.year <= 1970 || rtc_has_valid_date(&rd) != 0)
            snapshot.record.naming = SETTING_NAMING_CONTIGUOUS;
    }
    pthread_mutex_lock(&dvr_mutex);
    if (snapshot.record.naming == SETTING_NAMING_ELRS &&
        time(NULL) - dvr_race_label_time <= DVR_RACE_LABEL_TTL_S)
        snprintf(snapshot.label, sizeof(snapshot.label), "%s", dvr_race_label);
    pthread_mutex_unlock(&dvr_mutex);
    return snapshot;
}

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
    // Recorder health is polled by the control worker, never the fan loop.
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

static void dvr_select_audio_values(uint8_t source, int mic_gain, int linein_gain) {
    char buf[128];
    char *audio_source[3] = {
        "in_mic1",
        "in_mic2",
        "in_linein"};

    if (source > 2)
        source = 2;
    snprintf(buf, sizeof(buf), "%s %s", AUDIO_SEL_SH, audio_source[source]);
    system_exec(buf);
    if (source == SETTING_RECORD_AUDIO_SOURCE_MIC) dvr_set_mic_gain(mic_gain);
    else dvr_set_linein_gain(linein_gain);
}

void dvr_select_audio_source(uint8_t source) {
    dvr_select_audio_values(source, g_setting.record.mic_gain, g_setting.record.linein_gain);
}

// video input config
static bool config_ok;
static int dvr_ini_putl(const char *section, const char *key, long value, const char *file) {
    int result = ini_putl(section, key, value, file);
    config_ok &= result != 0;
    return result;
}
static int dvr_ini_puts(const char *section, const char *key, const char *value, const char *file) {
    int result = ini_puts(section, key, value, file);
    config_ok &= result != 0;
    return result;
}

static void dvr_write_vi_conf(video_resolution_t fmt) {
    int width = 1280, height = 720, fps = 60;
    switch (fmt) {
    case VR_720P50:
        fps = 50; break;
    case VR_720P30:
        fps = 30; break;
    case VR_540P90:
    case VR_540P90_CROP:
        fps = 90; break;
    case VR_1080P30:
        fps = 30;
#if defined(HDZBOXPRO)
        fps = 60;
#endif
        break;
    case VR_1080P24:
    case VR_1080P50:
        fps = 50; break;
    case VR_1080P60:
        fps = 59; break; // Preserve the hardware's existing 1080p60 workaround.
    default: break;
    }
#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
    if (fmt == VR_1080P30 || fmt == VR_1080P24 || fmt == VR_1080P50 || fmt == VR_1080P60) {
        width = 1920; height = 1080;
    }
#endif
    dvr_ini_putl("vi", "width", width, config_file);
    dvr_ini_putl("vi", "height", height, config_file);
    dvr_ini_putl("vi", "fps", fps, config_file);
}

void dvr_toggle() {
    dvr_cmd(DVR_TOGGLE);
}

static void dvr_write_star(void) {
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
}

// Never perform debug storage I/O while a DVR caller may hold UI/control locks.
static void dvr_dbg_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    dvr_trace_vlog("app", fmt, ap);
    va_end(ap);
}

static bool dvr_update_record_conf(const dvr_snapshot_t *snapshot) {
    dvr_ini_puts("record", "type", snapshot->record.format_ts ? "ts" : "mp4", config_file);
    int bitrate_num = 1;
    int bitrate_den = 1;
    switch (snapshot->record.bitrate_scale) {
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
    int width = 1280, height = 720, fps = 60, kbps = 34000, h265 = 0;
    if (snapshot->source == SOURCE_HDZERO) {
        if (snapshot->camera == VR_1080P30 || snapshot->camera == VR_1080P24) {
            width = 1920; height = 1080;
            if (snapshot->camera == VR_1080P24) fps = 50;
        } else if (snapshot->camera == VR_540P90 || snapshot->camera == VR_540P90_CROP) fps = 90;
        else { kbps = 24000; h265 = 1; }
    } else if (snapshot->source == SOURCE_AV_IN || snapshot->source == SOURCE_AV_MODULE) {
        kbps = 24000; h265 = 1; fps = snapshot->pal ? 50 : 60;
    } else if (snapshot->source == SOURCE_HDMI_IN) {
        if (snapshot->hdmi == HDMIIN_VTMG_1080P60 || snapshot->hdmi == HDMIIN_VTMG_1080P50 ||
            snapshot->hdmi == HDMIIN_VTMG_1080Pother) { width = 1920; height = 1080; }
        if (snapshot->hdmi == HDMIIN_VTMG_1080P50 || snapshot->hdmi == HDMIIN_VTMG_1080Pother ||
            snapshot->hdmi == HDMIIN_VTMG_720P50) fps = 50;
        if (snapshot->hdmi == HDMIIN_VTMG_720P100) fps = 90;
    }
    dvr_ini_putl("venc", "width", width, config_file);
    dvr_ini_putl("venc", "height", height, config_file);
    dvr_ini_putl("venc", "fps", fps, config_file);
    dvr_ini_putl("venc", "kbps", kbps * bitrate_num / bitrate_den, config_file);
    dvr_ini_putl("venc", "h265", h265, config_file);

    // The recorder treats kbps as a target under CBR and as a ceiling under
    // VBR. Keep CBR as the default and apply the user-controlled VBR quality
    // and QP floor to both codecs so changing source or codec cannot leave
    // stale encoder parameters behind.
    bool const use_vbr = (snapshot->record.rc_mode == SETTING_RECORD_RC_VBR);
    dvr_ini_putl("venc", "rc", use_vbr ? 1 : 0, config_file);
    for (int i = 0; i < 2; i++) {
        const char *sec = i ? "h265" : "h264";
        if (use_vbr) {
            dvr_ini_putl(sec, "quality", snapshot->record.vbr_quality, config_file);
            dvr_ini_putl(sec, "maxQP", snapshot->record.vbr_max_qp, config_file);
        } else {
            dvr_ini_putl(sec, "maxQP", 52, config_file);
        }
    }

    dvr_ini_putl("record", "audio", snapshot->record.audio, config_file);
    dvr_select_audio_values(snapshot->record.audio_source, snapshot->record.mic_gain, snapshot->record.linein_gain);
    // An unset clock only changes this recording, not the saved preference.
    // Recheck at each start so a later clock sync enables Date naming again.
    setting_record_naming_t naming = snapshot->record.naming;
    dvr_ini_putl("record", "naming", naming, config_file);
    dvr_ini_putl("record", "rolling", snapshot->record.rolling ? 1 : 0, config_file);

    // Only the ELRS naming scheme consumes race labels. Clear the pending
    // label when another scheme is selected so it cannot leak into a later
    // non-ELRS recording.
    dvr_ini_puts("record", "label", snapshot->label, config_file);

    // The record daemon picks the clip container purely from this "type" key
    // and silently falls back to "ts" if the value is missing or unrecognised.
    // A lost or raced write therefore turns an MP4 recording into a .ts file,
    // so confirm the intended value is actually on disk before we hand off to
    // the daemon, and re-write it if it is not.
    const char *want_type = snapshot->record.format_ts ? "ts" : "mp4";
    char got_type[8] = "";
    int type_retries = 0;
    while (true) {
        ini_gets("record", "type", "", got_type, sizeof(got_type), config_file);
        if (strcmp(got_type, want_type) == 0)
            break;
        if (type_retries == 3)
            break;
        dvr_ini_puts("record", "type", want_type, config_file);
        type_retries++;
    }
    dvr_dbg_log("update_conf format_ts=%d want=%s got=%s retries=%d",
                snapshot->record.format_ts, want_type, got_type, type_retries);
    return config_ok && strcmp(got_type, want_type) == 0;
}

static unsigned monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)ts.tv_sec;
}

static int control_transaction(int queue, unsigned command, unsigned epoch, record_control_reply_t *reply) {
    static unsigned sequence;
    static unsigned recorder;
    record_control_request_t request = {.type = RECORD_CONTROL_TYPE, .magic = RECORD_CONTROL_MAGIC,
        .owner = (unsigned)getpid(), .cookie = (unsigned)getpid() ^ (unsigned)time(NULL),
        .sequence = ++sequence, .command = command};
    if (!request.cookie) request.cookie = 1;
    snprintf(request.config, sizeof(request.config), "%s", runtime_conf);
    atomic_store(&control_phase, DVR_WAITING);
    unsigned began = monotonic_seconds();
    for (;;) {
        if (command == RECORD_START) {
            pthread_mutex_lock(&dvr_mutex);
            bool cancelled = control.stop != epoch || !control.desired || control.resume;
            pthread_mutex_unlock(&dvr_mutex);
            if (cancelled) return ECANCELED;
        }
        if (record_control_send(queue, &request) == 0) break;
        if (errno != EINTR && errno != EAGAIN) return errno;
        if (monotonic_seconds() - began >= 5) return EAGAIN;
        usleep(10000);
    }
    for (;;) {
        int result = record_control_ack(queue, &request, reply);
        if (result < 0) return errno;
        if (result > 0) {
            if (recorder && recorder != reply->recorder) return ESRCH;
            recorder = reply->recorder;
            return reply->error;
        }
        // A late ACK is still required: a timeout never authorizes source teardown.
        if (monotonic_seconds() - began >= 15) atomic_store(&control_error, ETIMEDOUT);
        usleep(10000);
    }
}

static bool prepare_runtime_conf(void) {
#if defined(__linux__)
    struct statfs fs;
    if (statfs("/tmp", &fs) || fs.f_type != 0x01021994) return false;
#endif
    char dir[128];
    if (mkdir("/tmp/hdz-dvr-control", 0700) < 0 && errno != EEXIST) return false;
    struct stat st;
    if (lstat("/tmp/hdz-dvr-control", &st) || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 077)) return false;
    snprintf(dir, sizeof(dir), "/tmp/hdz-dvr-control/%u", (unsigned)getpid());
    if (mkdir(dir, 0700) != 0) return false;
    snprintf(runtime_conf, sizeof(runtime_conf), "%s/record.conf", dir);
    int in = open(REC_CONF, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int out = open(runtime_conf, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    bool ok = in >= 0 && out >= 0;
    char buffer[4096];
    ssize_t n;
    size_t total = 0;
    while (ok && (n = read(in, buffer, sizeof(buffer))) > 0) {
        total += n;
        if (total > 65536 || write(out, buffer, n) != n) ok = false;
    }
    if (ok && n < 0) ok = false;
    if (in >= 0) close(in);
    if (out >= 0 && close(out) != 0) ok = false;
    snprintf(pending_conf, sizeof(pending_conf), "%s/pending.conf", dir);
    if (ok) config_file = runtime_conf;
    return ok;
}

static void *dvr_worker(void *unused) {
    (void)unused;
    int queue, error;
    record_control_reply_t reply;
    for (;;) {
        while ((queue = record_control_open(false)) < 0) {
            if (errno == EACCES) { atomic_store(&control_error, EACCES); return NULL; }
            usleep(100000);
        }
        error = control_transaction(queue, RECORD_HELLO, 0, &reply);
        // A recorder may replace its predecessor's queue during startup. Only
        // this unbound, read-only handshake is safe to retry after queue loss.
        if (error != EIDRM && error != EINVAL) break;
        usleep(100000);
    }
    if (!error && !prepare_runtime_conf()) error = EIO;
    if (error) { atomic_store(&control_error, error); return NULL; }
    bool recording = reply.recording;
    unsigned written_vi = 0;
    unsigned last_health = monotonic_seconds();
    pthread_mutex_lock(&dvr_mutex);
    control.confirmed_idle = !recording;
    pthread_mutex_unlock(&dvr_mutex);
    for (;;) {
        pthread_mutex_lock(&dvr_mutex);
        bool stop = control.stop != control.stopped || (recording && !control.desired);
        bool start = control.desired && !recording && !control.resume && !control.pause && !control.paused &&
                     g_sdcard_enable && g_sdcard_ready && dvr_space_allows_start();
        bool live_stop = control.live_stop && !control.resume, pause = control.pause, star = control.star;
        unsigned epoch = control.stop, vi_generation = control.vi_generation;
        video_resolution_t vi = control.vi;
        dvr_snapshot_t snapshot = control.snapshot;
        control.star = false;
        pthread_mutex_unlock(&dvr_mutex);
        error = 0;
        if (stop) {
            error = control_transaction(queue, RECORD_STOP, epoch, &reply);
            if (!error && reply.recording) error = EBUSY;
            if (!error) {
                recording = false;
                pthread_mutex_lock(&dvr_mutex);
                control.stopped = epoch;
                control.confirmed_idle = true;
                pthread_mutex_unlock(&dvr_mutex);
            }
        } else if (pause) {
            error = control_transaction(queue, RECORD_PAUSE, epoch, &reply);
            if (!error && reply.recording) error = EBUSY;
            if (!error) {
                recording = false;
                pthread_mutex_lock(&dvr_mutex);
                control.pause = false;
                control.paused = true;
                control.confirmed_idle = true;
                pthread_mutex_unlock(&dvr_mutex);
            }
        } else if (start || vi_generation != written_vi || live_stop) {
            atomic_store(&control_phase, DVR_CONFIGURING);
            // Edit a private copy, then publish a new immutable RAM snapshot.
            // The recorder can still be using the preceding path for live capture.
            char old_conf[RECORD_CONTROL_PATH];
            snprintf(old_conf, sizeof(old_conf), "%s", runtime_conf);
            FILE *in = fopen(old_conf, "r"), *out = fopen(pending_conf, "wx");
            config_ok = in && out;
            char bytes[4096];
            size_t n;
            while (config_ok && (n = fread(bytes, 1, sizeof(bytes), in)) != 0)
                config_ok &= fwrite(bytes, 1, n, out) == n;
            if (in) { config_ok &= !ferror(in); fclose(in); }
            if (out && fclose(out)) config_ok = false;
            config_file = pending_conf;
            if (config_ok) dvr_write_vi_conf(vi);
            if (start && config_ok) config_ok &= dvr_update_record_conf(&snapshot);
            static unsigned config_sequence;
            char *slash = strrchr(runtime_conf, '/');
            snprintf(slash + 1, sizeof(runtime_conf) - (slash + 1 - runtime_conf),
                     "record-%u.conf", ++config_sequence);
            if (config_ok && rename(pending_conf, runtime_conf)) config_ok = false;
            if (!config_ok) error = EIO;
            else {
                written_vi = vi_generation;
                if (start && snapshot.source != SOURCE_HDMI_IN) {
                    Display_Osd(snapshot.record.osd);
                    (void)Display_Osd_Readback();
                }
                {
                    error = control_transaction(queue, start ? RECORD_START : live_stop ? RECORD_LIVE_STOP : RECORD_CONFIG, epoch, &reply);
                    if (error == ECANCELED)
                        error = control_transaction(queue, RECORD_CONFIG, epoch, &reply);
                    if (!error) {
                        unlink(old_conf);
                        recording = reply.recording;
                        pthread_mutex_lock(&dvr_mutex);
                        bool still_requested = control.desired && control.stop == epoch;
                        pthread_mutex_unlock(&dvr_mutex);
                        if (start && still_requested && !recording) error = EIO;
                        if (start && recording) dvr_recording_start = time(NULL);
                        if (live_stop) {
                            pthread_mutex_lock(&dvr_mutex);
                            control.live_stop = false;
                            control.paused = false;
                            pthread_mutex_unlock(&dvr_mutex);
                        }
                    }
                }
            }
        } else if (star) dvr_write_star();
        else if (recording && monotonic_seconds() - last_health >= 1) {
            error = control_transaction(queue, RECORD_HELLO, epoch, &reply);
            last_health = monotonic_seconds();
            if (!error && !reply.recording) {
                recording = false;
                pthread_mutex_lock(&dvr_mutex);
                control.desired = false;
                control.confirmed_idle = true;
                pthread_mutex_unlock(&dvr_mutex);
            }
        }
        if (error == ECANCELED) error = 0;
        pthread_mutex_lock(&dvr_mutex);
        dvr_is_recording = recording;
        record_pending = control.desired && !recording;
        if (recording) control.confirmed_idle = false;
        pthread_mutex_unlock(&dvr_mutex);
        atomic_store(&control_error, error);
        atomic_store(&control_phase, DVR_IDLE);
        if (error) {
            // Stop/config errors do not permit dependent hardware changes or retries of START.
            pthread_mutex_lock(&dvr_mutex);
            control.desired = false;
            control.confirmed_idle = false;
            pthread_mutex_unlock(&dvr_mutex);
            while (true) usleep(100000);
        }
        usleep(10000);
    }
    return NULL;
}

void dvr_init(void) {
#ifdef EMULATOR_BUILD
    control.initialized = true;
    return; // No native recorder/MPP process exists in the SDL emulator.
#endif
    pthread_mutex_lock(&dvr_mutex);
    if (!control.initialized) {
        control.initialized = true;
        control.vi = VR_720P60;
        control.vi_generation = 1;
        pthread_t worker;
        if (pthread_create(&worker, NULL, dvr_worker, NULL) == 0) pthread_detach(worker);
        else atomic_store(&control_error, EAGAIN);
    }
    pthread_mutex_unlock(&dvr_mutex);
}

void dvr_update_vi_conf(video_resolution_t fmt) {
    pthread_mutex_lock(&dvr_mutex);
    if (fmt != control.vi) { control.vi = fmt; control.vi_generation++; }
    pthread_mutex_unlock(&dvr_mutex);
}

void dvr_star(void) {
    pthread_mutex_lock(&dvr_mutex);
    control.star = true;
    pthread_mutex_unlock(&dvr_mutex);
}

void dvr_stop_live(void) {
    dvr_cmd(DVR_STOP);
    pthread_mutex_lock(&dvr_mutex);
    control.live_stop = true;
    pthread_mutex_unlock(&dvr_mutex);
}

bool dvr_defer_transition(void (*resume)(uintptr_t), uintptr_t argument) {
#ifdef EMULATOR_BUILD
    return false;
#endif
    pthread_mutex_lock(&dvr_mutex);
    bool ready = control.resuming || (control.paused && control.confirmed_idle && !control.desired && control.stop == control.stopped &&
                 !atomic_load(&control_error) && atomic_load(&control_phase) == DVR_IDLE);
    if (!ready) {
        if (!control.resume && resume) { control.resume = resume; control.argument = argument; }
        control.desired = false;
        control.live_stop = false; // Supersede any old source's queued unpause.
        control.pause = true;
        if (control.stop == control.stopped) control.stop++;
    }
    pthread_mutex_unlock(&dvr_mutex);
    return !ready;
}

void dvr_poll(void) {
    void (*resume)(uintptr_t) = NULL;
    uintptr_t argument = 0;
    pthread_mutex_lock(&dvr_mutex);
    if (control.resume && control.paused && control.confirmed_idle && control.stop == control.stopped &&
        atomic_load(&control_phase) == DVR_IDLE && !atomic_load(&control_error)) {
        resume = control.resume; argument = control.argument; control.resume = NULL;
        control.resuming = true;
    }
    pthread_mutex_unlock(&dvr_mutex);
    if (resume) {
        resume(argument); // Main UI thread, with lvgl_mutex held.
        if (g_app_state != APP_STATE_SLEEP) dvr_stop_live();
        pthread_mutex_lock(&dvr_mutex);
        control.resuming = false;
        pthread_mutex_unlock(&dvr_mutex);
    }
}

const char *dvr_transition_status(void) {
    if (atomic_load(&control_error)) return "DVR: recorder delayed/error - keep card inserted";
    pthread_mutex_lock(&dvr_mutex);
    bool pending = control.resume != NULL;
    pthread_mutex_unlock(&dvr_mutex);
    return pending ? "DVR: waiting for recorder to finish" : NULL;
}

bool dvr_space_allows_start(void) {
    return g_setting.record.rolling || !sdcard_is_full();
}

bool dvr_input_blocked(void) {
    pthread_mutex_lock(&dvr_mutex);
    bool blocked = control.resume && !control.resuming;
    pthread_mutex_unlock(&dvr_mutex);
    return blocked;
}

void dvr_cmd(osd_dvr_cmd_t cmd) {
    dvr_snapshot_t snapshot = {0};
    if (cmd != DVR_STOP) snapshot = dvr_snapshot();
    pthread_mutex_lock(&dvr_mutex);
    bool start = cmd == DVR_START || (cmd == DVR_TOGGLE && !control.desired);
    if (start && !control.resume && !control.resuming && !control.pause && !control.paused) {
        control.snapshot = snapshot;
        control.desired = true;
        control.confirmed_idle = false;
    } else if (!start) {
        control.desired = false;
        control.stop++;
    }
    record_pending = control.desired && !dvr_is_recording;
    pthread_mutex_unlock(&dvr_mutex);
}
