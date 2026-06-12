#include "wav_test.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "wav_test"
#include <log/log.h>

#include <mm_comm_aio.h>
#include <mm_common.h>
#include <mpi_ai.h>
#include <mpi_ao.h>
#include <mpi_sys.h>
#include <plat_type.h>

// Samples per channel handed to the AO per frame; ~21ms at 48kHz.
#define WAV_TEST_POINTS_PER_FRAME 1024

#define WAV_TEST_REC_RATE     48000
#define WAV_TEST_REC_CHANNELS 2

// The AI/AO engines sit on a process-global MPP context that must be brought
// up with AW_MPI_SYS_Init before any other MPI call; without it the calls
// wedge the device hard enough to need a reboot. Bracket each test with
// init/exit the same way the media player brackets each playback
// (vdec2vo_initSys / vdec2vo_deinitSys).
static void wav_test_sys_init(void) {
    MPP_SYS_CONF_S conf;
    memset(&conf, 0, sizeof(conf));
    conf.nAlignWidth = 32;
    AW_MPI_SYS_SetConf(&conf);
    AW_MPI_SYS_Init();
}

// ---- minimal RIFF/WAVE PCM16 handling --------------------------------------

typedef struct {
    uint16_t channels;
    uint32_t sample_rate;
    long data_offset;
    uint32_t data_len;
} wav_info_t;

static uint32_t rd32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

static int wav_parse(FILE *fp, wav_info_t *out) {
    uint8_t hdr[12];

    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return -1;

    bool have_fmt = false;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, sizeof(ch), fp) != sizeof(ch))
            return -1;
        uint32_t len = rd32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (len < 16 || fread(fmt, 1, sizeof(fmt), fp) != sizeof(fmt))
                return -1;
            if (rd16(fmt) != 1 || rd16(fmt + 14) != 16)
                return -1; // PCM 16-bit only
            out->channels = rd16(fmt + 2);
            out->sample_rate = rd32(fmt + 4);
            if (out->channels < 1 || out->channels > 2)
                return -1;
            if (len > 16 && fseek(fp, len - 16, SEEK_CUR) != 0)
                return -1;
            have_fmt = true;
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!have_fmt)
                return -1;
            out->data_offset = ftell(fp);
            out->data_len = len;
            return 0;
        } else {
            // skip unknown chunk (word-aligned)
            if (fseek(fp, (long)((len + 1) & ~1u), SEEK_CUR) != 0)
                return -1;
        }
    }
}

// Canonical 44-byte PCM header for the capture format; rewritten with the
// real data length once recording finishes.
static void wav_write_header(FILE *fp, uint32_t data_len) {
    uint8_t h[44];

    memcpy(h, "RIFF", 4);
    wr32(h + 4, 36 + data_len);
    memcpy(h + 8, "WAVEfmt ", 8);
    wr32(h + 16, 16);
    wr16(h + 20, 1); // PCM
    wr16(h + 22, WAV_TEST_REC_CHANNELS);
    wr32(h + 24, WAV_TEST_REC_RATE);
    wr32(h + 28, WAV_TEST_REC_RATE * WAV_TEST_REC_CHANNELS * 2);
    wr16(h + 32, WAV_TEST_REC_CHANNELS * 2);
    wr16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    wr32(h + 40, data_len);

    fseek(fp, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), fp);
}

// ---- playback: WAV -> AO ----------------------------------------------------

int wav_test_play(const char *path) {
    wav_info_t wav;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOGE("open %s failed", path);
        return -1;
    }
    if (wav_parse(fp, &wav) != 0) {
        LOGE("%s is not a 16-bit PCM WAV", path);
        fclose(fp);
        return -1;
    }
    LOGD("play %s: %u Hz, %u ch, %u bytes", path, wav.sample_rate,
         wav.channels, wav.data_len);

    wav_test_sys_init();

    AUDIO_DEV aoDev = 0;
    AIO_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.u32ChnCnt = wav.channels;
    attr.enBitwidth = AUDIO_BIT_WIDTH_16;
    attr.enSamplerate = (AUDIO_SAMPLE_RATE_E)wav.sample_rate;
    AW_MPI_AO_SetPubAttr(aoDev, &attr);
    AW_MPI_AO_Enable(aoDev);

    AO_CHN aoChn = 0;
    bool chn_ok = false;
    while (aoChn < AIO_MAX_CHN_NUM) {
        ERRORTYPE ret = AW_MPI_AO_EnableChn(aoDev, aoChn);
        if (ret == SUCCESS) {
            chn_ok = true;
            break;
        } else if (ret == ERR_AO_EXIST) {
            aoChn++;
        } else {
            LOGE("enable ao chn[%d] fail: 0x%x", aoChn, ret);
            break;
        }
    }
    if (!chn_ok) {
        AW_MPI_AO_Disable(aoDev);
        AW_MPI_SYS_Exit();
        fclose(fp);
        return -1;
    }
    AW_MPI_AO_StartChn(aoDev, aoChn);

    // SendFrame is zero-copy: the AO queues the buffer POINTER and only lets
    // go of it after the DMA has played it (MPP_EVENT_RELEASE_AUDIO_BUFFER --
    // see ai2ao's using-list). Feeding every chunk from one reused buffer
    // plays garbage for everything still queued, so load the whole file once
    // and send pointers into it; it is freed after the drain below.
    size_t chunk = (size_t)WAV_TEST_POINTS_PER_FRAME * wav.channels * 2;
    uint8_t *data = malloc(wav.data_len);
    size_t total = 0;
    if (data) {
        fseek(fp, wav.data_offset, SEEK_SET);
        total = fread(data, 1, wav.data_len, fp);
    } else {
        LOGE("alloc %u bytes for %s failed", wav.data_len, path);
    }
    fclose(fp);

    size_t sent = 0;
    unsigned int seq = 0;
    uint64_t pts_us = 0;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (sent < total) {
        size_t n = total - sent < chunk ? total - sent : chunk;

        AUDIO_FRAME_S frm;
        memset(&frm, 0, sizeof(frm));
        frm.mSamplerate = (AUDIO_SAMPLE_RATE_E)wav.sample_rate;
        frm.mBitwidth = AUDIO_BIT_WIDTH_16;
        frm.mSoundmode = (wav.channels == 2) ? AUDIO_SOUND_MODE_STEREO
                                             : AUDIO_SOUND_MODE_MONO;
        frm.mpAddr = data + sent;
        frm.mLen = (unsigned int)n;
        frm.mSeq = seq++;
        frm.mTimeStamp = pts_us;
        pts_us += (uint64_t)(n / (wav.channels * 2)) * 1000000u / wav.sample_rate;

        AW_MPI_AO_SendFrame(aoDev, aoChn, &frm, -1);
        sent += n;
    }

    // SendFrame queues frames much faster than the DMA plays them, so by here
    // almost the whole file can still be waiting in the AO. Tearing the
    // channel down now cuts playback off after a fraction of a second. Signal
    // EOF with the drain flag, then also wait out the file's real duration
    // (pts_us counts every sample queued) in case the drain flag returns
    // early; only then take the channel apart.
    AW_MPI_AO_SetStreamEof(aoDev, aoChn, TRUE, TRUE);

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t elapsed_us = (int64_t)(t1.tv_sec - t0.tv_sec) * 1000000 +
                         (t1.tv_nsec - t0.tv_nsec) / 1000;
    int64_t remain_us = (int64_t)pts_us + 200000 - elapsed_us;
    if (remain_us > 0)
        usleep((useconds_t)remain_us);

    AW_MPI_AO_StopChn(aoDev, aoChn);
    AW_MPI_AO_DisableChn(aoDev, aoChn);
    AW_MPI_AO_Disable(aoDev);
    AW_MPI_SYS_Exit();
    free(data); // not before: queued frames point into it until played
    return total > 0 ? 0 : -1;
}

// ---- capture: AI -> WAV -----------------------------------------------------

int wav_test_record(const char *path, int seconds) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        LOGE("open %s failed", path);
        return -1;
    }
    wav_write_header(fp, 0); // placeholder, patched below

    wav_test_sys_init();

    AUDIO_DEV aiDev = 0;
    AIO_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.u32ChnCnt = WAV_TEST_REC_CHANNELS;
    attr.enBitwidth = AUDIO_BIT_WIDTH_16;
    attr.enSamplerate = (AUDIO_SAMPLE_RATE_E)WAV_TEST_REC_RATE;
    AW_MPI_AI_SetPubAttr(aiDev, &attr);
    AW_MPI_AI_Enable(aiDev);

    AI_CHN aiChn = 0;
    bool chn_ok = false;
    while (aiChn < AIO_MAX_CHN_NUM) {
        ERRORTYPE ret = AW_MPI_AI_CreateChn(aiDev, aiChn);
        if (ret == SUCCESS) {
            chn_ok = true;
            break;
        } else if (ret == ERR_AI_EXIST) {
            aiChn++;
        } else {
            LOGE("create ai chn[%d] fail: 0x%x", aiChn, ret);
            break;
        }
    }
    if (!chn_ok) {
        AW_MPI_AI_Disable(aiDev);
        AW_MPI_SYS_Exit();
        fclose(fp);
        return -1;
    }
    AW_MPI_AI_EnableChn(aiDev, aiChn);

    uint32_t target = (uint32_t)WAV_TEST_REC_RATE * (uint32_t)seconds *
                      WAV_TEST_REC_CHANNELS * 2;
    uint32_t written = 0;
    while (written < target) {
        AUDIO_FRAME_S frm;
        memset(&frm, 0, sizeof(frm));
        ERRORTYPE ret = AW_MPI_AI_GetFrame(aiDev, aiChn, &frm, NULL, -1);
        if (ret != SUCCESS) {
            LOGE("ai get frame fail: 0x%x", ret);
            break;
        }
        uint32_t n = frm.mLen;
        if (n > target - written)
            n = target - written;
        if (frm.mpAddr && n)
            fwrite(frm.mpAddr, 1, n, fp);
        written += n;
        AW_MPI_AI_ReleaseFrame(aiDev, aiChn, &frm, NULL);
    }

    AW_MPI_AI_DisableChn(aiDev, aiChn);
    AW_MPI_AI_ResetChn(aiDev, aiChn);
    AW_MPI_AI_DestroyChn(aiDev, aiChn);
    AW_MPI_AI_Disable(aiDev);
    AW_MPI_SYS_Exit();

    wav_write_header(fp, written);
    fclose(fp);
    LOGD("recorded %u bytes to %s", written, path);
    return written > 0 ? 0 : -1;
}
