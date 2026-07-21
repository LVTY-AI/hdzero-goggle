#include "awdmx.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// #define LOG_NDEBUG 0
#define LOG_TAG "awdmx"
#include <log/log.h>

#include <ClockCompPortIndex.h>
#include <mpi_clock.h>
#include <mpi_demux.h>
#include <mpi_sys.h>
#include <mpi_vdec.h>

// The mpegts demuxer leaves mFrameRate at 0 (mp4 fills it), so the panel-retime
// never engages for .ts recordings. Recover the rate container-side: walk the
// first few MB of transport packets, collect the video PES PTS stamps (90kHz),
// and take the median delta. Codec-agnostic (H.264 and H.265 alike), no
// bitstream parsing. Returns whole fps, or 0 if the file isn't readable TS.
#define TSPROBE_READ_MAX     (4 * 1024 * 1024)
#define TSPROBE_PTS_MAX      40
#define TSPROBE_CHUNK_PKTS   512
#define TSPROBE_CHUNK_BYTES  (TSPROBE_CHUNK_PKTS * 188)

static int tsprobe_cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint16_t awdmx_probeTsFps(const char *sFile) {
    uint64_t pts[TSPROBE_PTS_MAX];
    uint64_t deltas[TSPROBE_PTS_MAX];
    int nPts = 0, nDeltas = 0;

    int fd = open(sFile, O_RDONLY);
    if (fd < 0)
        return 0;

    // Resync in case the file doesn't start on a packet boundary.
    off_t base = 0;
    {
        uint8_t head[188 * 3];
        ssize_t n = pread(fd, head, sizeof(head), 0);
        int i;
        for (i = 0; i + 188 * 2 < n; i++) {
            if (head[i] == 0x47 && head[i + 188] == 0x47 && head[i + 188 * 2] == 0x47)
                break;
        }
        if (i + 188 * 2 >= n) {
            close(fd);
            return 0; // not a transport stream
        }
        base = i;
    }

    // Read in large, packet-aligned chunks instead of one pread() per 188-byte
    // packet -- the per-packet version could cost thousands of syscalls on a
    // slow-PTS-density file, which is measurable overhead on flash storage.
    uint8_t *buf = malloc(TSPROBE_CHUNK_BYTES);
    if (!buf) {
        close(fd);
        return 0;
    }

    off_t off = base;
    while (off < base + TSPROBE_READ_MAX && nPts < TSPROBE_PTS_MAX) {
        ssize_t n = pread(fd, buf, TSPROBE_CHUNK_BYTES, off);
        if (n < 188)
            break;

        int nPkts = n / 188;
        bool desync = false;
        for (int k = 0; k < nPkts && nPts < TSPROBE_PTS_MAX; k++) {
            const uint8_t *pkt = &buf[k * 188];
            if (pkt[0] != 0x47) {
                desync = true;
                break;
            }
            if (!(pkt[1] & 0x40)) // payload_unit_start_indicator
                continue;

            int p = 4;
            if (pkt[3] & 0x20) // adaptation field present
                p += 1 + pkt[4];
            if (p + 14 > 188)
                continue;

            // PES start for a video stream_id (0xE0-0xEF), with a PTS present.
            if (pkt[p] != 0x00 || pkt[p + 1] != 0x00 || pkt[p + 2] != 0x01)
                continue;
            if ((pkt[p + 3] & 0xF0) != 0xE0)
                continue;
            if (!(pkt[p + 7] & 0x80)) // PTS_DTS_flags: PTS absent
                continue;

            const uint8_t *q = &pkt[p + 9];
            pts[nPts++] = ((uint64_t)((q[0] >> 1) & 0x07) << 30) |
                          ((uint64_t)q[1] << 22) |
                          ((uint64_t)((q[2] >> 1) & 0x7F) << 15) |
                          ((uint64_t)q[3] << 7) |
                          ((uint64_t)(q[4] >> 1) & 0x7F);
        }

        if (desync)
            break;
        off += (off_t)nPkts * 188;
        if (n < TSPROBE_CHUNK_BYTES)
            break; // short read: EOF
    }

    free(buf);
    close(fd);

    // PTS arrive in decode order; sort so B-frame reordering can't produce
    // bogus deltas, then take the median frame interval.
    if (nPts < 8)
        return 0;
    qsort(pts, nPts, sizeof(pts[0]), tsprobe_cmp_u64);
    for (int i = 1; i < nPts; i++) {
        if (pts[i] > pts[i - 1])
            deltas[nDeltas++] = pts[i] - pts[i - 1];
    }
    if (nDeltas < 4)
        return 0;
    qsort(deltas, nDeltas, sizeof(deltas[0]), tsprobe_cmp_u64);
    uint64_t med = deltas[nDeltas / 2];
    if (med == 0)
        return 0;

    int fps = (int)((90000 + med / 2) / med);
    return (fps >= 20 && fps <= 130) ? (uint16_t)fps : 0;
}

static ERRORTYPE MPPCallbackWrapper(void *cookie, MPP_CHN_S *pChn, MPP_EVENT_TYPE event, void *pEventData) {
    AwdmxContext_t *dmxCtx = (AwdmxContext_t *)cookie;

    if (pChn->mModId == MOD_ID_DEMUX) {
        switch (event) {
        case MPP_EVENT_NOTIFY_EOF:
            LOGD("demux to end of file");
            dmxCtx->bEof = true;
            if (dmxCtx->cbOnEof != NULL) {
                dmxCtx->cbOnEof(dmxCtx->cbOnEofContext);
            }
            break;

        default:
            break;
        }
    }

    return SUCCESS;
}

static void awdmx_configDmxChnAttr(AwdmxContext_t *dmxCtx, DEMUX_CHN_ATTR_S *dmxChnAttr) {
    dmxChnAttr->mStreamType = STREAMTYPE_LOCALFILE;
    dmxChnAttr->mSourceType = SOURCETYPE_FD;
    dmxChnAttr->mSourceUrl = NULL;
    dmxChnAttr->mFd = dmxCtx->srcFd;
    dmxChnAttr->mDemuxDisableTrack = DEMUX_DISABLE_SUBTITLE_TRACK;
}

static ERRORTYPE awdmx_createClockChn(AwdmxContext_t *dmxCtx) {
    ERRORTYPE ret;
    BOOL bSuccessFlag = FALSE;

    CLOCK_CHN_ATTR_S clkChnAttr;
    CLOCK_CHN clkChn = 0;

    dmxCtx->clkChn = 0;
    clkChnAttr.nWaitMask = 0;

    if (dmxCtx->videoNum > 0) {
        clkChnAttr.nWaitMask |= 1 << CLOCK_PORT_INDEX_VIDEO;
    }

    if (dmxCtx->audioNum > 0) {
        clkChnAttr.nWaitMask |= 1 << CLOCK_PORT_INDEX_AUDIO;
    }

    while (clkChn < CLOCK_MAX_CHN_NUM) {
        ret = AW_MPI_CLOCK_CreateChn(clkChn, &clkChnAttr);
        if (SUCCESS == ret) {
            bSuccessFlag = TRUE;
            LOGD("create clock channel[%d] success!", clkChn);
            break;
        } else if (ERR_CLOCK_EXIST == ret) {
            LOGD("clock channel[%d] is exist, find next!", clkChn);
            clkChn++;
        } else {
            LOGD("create clock channel[%d] ret[0x%x]!", clkChn, ret);
            break;
        }
    }

    if (FALSE == bSuccessFlag) {
        dmxCtx->clkChn = MM_INVALID_CHN;
        LOGE("fatal error! create clock channel fail!");
        return FAILURE;
    }
    dmxCtx->clkChn = clkChn;

    return SUCCESS;
}

static ERRORTYPE awdmx_createDemuxChn(AwdmxContext_t *dmxCtx) {
    ERRORTYPE ret;
    BOOL nSuccessFlag = FALSE;
    DEMUX_CHN_ATTR_S dmxChnAttr;

    memset(&dmxChnAttr, 0, sizeof(dmxChnAttr));
    awdmx_configDmxChnAttr(dmxCtx, &dmxChnAttr);

    dmxCtx->dmxChn = 0;
    while (dmxCtx->dmxChn < DEMUX_MAX_CHN_NUM) {
        ret = AW_MPI_DEMUX_CreateChn(dmxCtx->dmxChn, &dmxChnAttr);
        if (SUCCESS == ret) {
            nSuccessFlag = TRUE;
            LOGD("create demux channel[%d] success!", dmxCtx->dmxChn);
            break;
        } else if (ERR_DEMUX_EXIST == ret) {
            LOGD("demux channel[%d] is exist, find next!", dmxCtx->dmxChn);
            dmxCtx->dmxChn++;
        } else {
            LOGD("create demux channel[%d] ret[0x%x]!", dmxCtx->dmxChn, ret);
            break;
        }
    }

    if (FALSE == nSuccessFlag) {
        dmxCtx->dmxChn = MM_INVALID_CHN;
        LOGE("fatal error! create demux channel fail!");
        return ret;
    } else {
        MPPCallbackInfo cbInfo;
        cbInfo.cookie = (void *)dmxCtx;
        cbInfo.callback = (MPPCallbackFuncType)&MPPCallbackWrapper;
        AW_MPI_DEMUX_RegisterCallback(dmxCtx->dmxChn, &cbInfo);
        return SUCCESS;
    }
}

AwdmxContext_t *awdmx_open(char *sFile, CB_onDmxEof cbOnEof, void *context) {
    AwdmxContext_t *dmxCtx = (AwdmxContext_t *)malloc(sizeof(AwdmxContext_t));
    ERRORTYPE ret;
    int nIndex = 0;
    DEMUX_MEDIA_INFO_S DemuxMediaInfo = {0};

    if (dmxCtx == NULL) {
        LOGE("out of memory");
        return NULL;
    }

    memset(dmxCtx, 0, sizeof(AwdmxContext_t));
    dmxCtx->dmxChn = MM_INVALID_CHN;
    dmxCtx->clkChn = MM_INVALID_CHN;
    dmxCtx->bEof = false;
    dmxCtx->videoNum = 0;
    dmxCtx->audioNum = 0;
    dmxCtx->cbOnEof = cbOnEof;
    dmxCtx->cbOnEofContext = context;

    LOGD("open file");
    strcpy(dmxCtx->srcFile, sFile);
    dmxCtx->srcFd = open(sFile, O_RDONLY);
    if (dmxCtx->srcFd < 0) {
        LOGE("open file failed: %s", strerror(errno));
        goto failed;
    }

    ret = awdmx_createDemuxChn(dmxCtx);
    if (ret != SUCCESS) {
        LOGE("create demux chn fail");
        goto failed;
    }

    ret = AW_MPI_DEMUX_GetMediaInfo(dmxCtx->dmxChn, &DemuxMediaInfo);
    if (ret != SUCCESS) {
        LOGE("fatal error! get media info fail!");
        goto failed;
    }

    if ((DemuxMediaInfo.mVideoNum > 0 && DemuxMediaInfo.mVideoIndex >= DemuxMediaInfo.mVideoNum) ||
        (DemuxMediaInfo.mAudioNum > 0 && DemuxMediaInfo.mAudioIndex >= DemuxMediaInfo.mAudioNum) ||
        (DemuxMediaInfo.mSubtitleNum > 0 && DemuxMediaInfo.mSubtitleIndex >= DemuxMediaInfo.mSubtitleNum)) {
        LOGD("fatal error, trackIndex wrong! [%d][%d],[%d][%d],[%d][%d]",
             DemuxMediaInfo.mVideoNum, DemuxMediaInfo.mVideoIndex, DemuxMediaInfo.mAudioNum, DemuxMediaInfo.mAudioIndex, DemuxMediaInfo.mSubtitleNum, DemuxMediaInfo.mSubtitleIndex);
        goto failed;
    }

    nIndex = DemuxMediaInfo.mVideoIndex;
    dmxCtx->videoNum = DemuxMediaInfo.mVideoNum;
    dmxCtx->width = DemuxMediaInfo.mVideoStreamInfo[nIndex].mWidth;
    dmxCtx->height = DemuxMediaInfo.mVideoStreamInfo[nIndex].mHeight;
    dmxCtx->fps = (DemuxMediaInfo.mVideoStreamInfo[nIndex].mFrameRate + 500) / 1000; // x1000 -> whole fps
    dmxCtx->codecType = DemuxMediaInfo.mVideoStreamInfo[nIndex].mCodecType;
    dmxCtx->msDuration = DemuxMediaInfo.mDuration;
    LOGD("stream info %dx%d @ %dfps", DemuxMediaInfo.mVideoStreamInfo[nIndex].mWidth, DemuxMediaInfo.mVideoStreamInfo[nIndex].mHeight, dmxCtx->fps);

    if (dmxCtx->fps == 0) {
        dmxCtx->fps = awdmx_probeTsFps(sFile);
        LOGD("stream info fps probed from PES PTS: %dfps", dmxCtx->fps);
    }

    if (DemuxMediaInfo.mAudioNum > 0) {
        nIndex = DemuxMediaInfo.mAudioIndex;
        dmxCtx->audioNum = DemuxMediaInfo.mAudioNum;
        dmxCtx->aCodecType = DemuxMediaInfo.mAudioStreamInfo[nIndex].mCodecType;
        dmxCtx->channels = DemuxMediaInfo.mAudioStreamInfo[nIndex].mChannelNum;
        dmxCtx->bitsPerSample = DemuxMediaInfo.mAudioStreamInfo[nIndex].mBitsPerSample;
        dmxCtx->sampleRate = DemuxMediaInfo.mAudioStreamInfo[nIndex].mSampleRate;
        LOGD("stream info %dHz,%dch,%dbits,codec=%d", dmxCtx->sampleRate,
             dmxCtx->channels,
             dmxCtx->bitsPerSample,
             dmxCtx->aCodecType);

        if (dmxCtx->sampleRate == 0) {
            dmxCtx->channels = AUDIO_defChannels;
            dmxCtx->bitsPerSample = AUDIO_defSampleBits;
            dmxCtx->sampleRate = AUDIO_defSampleRate;
            LOGD("stream info filled: %dHz,%dch,%dbits,codec=%d", dmxCtx->sampleRate,
                 dmxCtx->channels,
                 dmxCtx->bitsPerSample,
                 dmxCtx->aCodecType);
        }
    }

    ret = awdmx_createClockChn(dmxCtx);
    if (ret != SUCCESS) {
        LOGE("create clock chn fail");
        goto failed;
    }

    MPP_CHN_S DmxChn = {MOD_ID_DEMUX, 0, dmxCtx->dmxChn};
    MPP_CHN_S ClockChn = {MOD_ID_CLOCK, 0, dmxCtx->clkChn};

    ret = AW_MPI_SYS_Bind(&ClockChn, &DmxChn);
    LOGD("bind demux %d & clock %d: %x", dmxCtx->dmxChn, dmxCtx->clkChn, ret);

    return dmxCtx;

failed:
    awdmx_close(dmxCtx);
    return NULL;
}

void awdmx_close(AwdmxContext_t *dmxCtx) {
    if (dmxCtx == NULL) {
        return;
    }

    if (dmxCtx->srcFd < 0) {
        return;
    }

    if (dmxCtx->dmxChn >= 0) {
        AW_MPI_DEMUX_DestroyChn(dmxCtx->dmxChn);
        dmxCtx->dmxChn = MM_INVALID_CHN;
    }

    if (dmxCtx->clkChn >= 0) {
        AW_MPI_CLOCK_DestroyChn(dmxCtx->clkChn);
        dmxCtx->clkChn = MM_INVALID_CHN;
    }

    close(dmxCtx->srcFd);
    dmxCtx->srcFd = -1;
    dmxCtx->videoNum = 0;
    dmxCtx->bEof = false;
    free(dmxCtx);
}

ERRORTYPE awdmx_bindVdecAndClock(AwdmxContext_t *dmxCtx, VDEC_CHN vdecChn, CLOCK_CHN clkChn) {
    ERRORTYPE ret = SUCCESS;

    if (dmxCtx->videoNum > 0) {
        dmxCtx->clkChn = clkChn;

        MPP_CHN_S DmxChn = {MOD_ID_DEMUX, 0, dmxCtx->dmxChn};
        MPP_CHN_S VdecChn = {MOD_ID_VDEC, 0, vdecChn};
        MPP_CHN_S ClockChn = {MOD_ID_CLOCK, 0, dmxCtx->clkChn};

        ret = AW_MPI_SYS_Bind(&DmxChn, &VdecChn);
        LOGD("bind demux %d & vdec %d: %x", dmxCtx->dmxChn, vdecChn, ret);

        ret = AW_MPI_SYS_Bind(&ClockChn, &DmxChn);
        LOGD("bind demux %d & clock %d: %x", dmxCtx->dmxChn, dmxCtx->clkChn, ret);
    }

    return ret;
}

ERRORTYPE awdmx_unbindVdecAndClock(AwdmxContext_t *dmxCtx, VDEC_CHN vdecChn, CLOCK_CHN clkChn) {
    ERRORTYPE ret = SUCCESS;

    if (dmxCtx->videoNum > 0) {
        dmxCtx->clkChn = clkChn;

        MPP_CHN_S DmxChn = {MOD_ID_DEMUX, 0, dmxCtx->dmxChn};
        MPP_CHN_S VdecChn = {MOD_ID_VDEC, 0, vdecChn};
        MPP_CHN_S ClockChn = {MOD_ID_CLOCK, 0, dmxCtx->clkChn};

        ret = AW_MPI_SYS_UnBind(&DmxChn, &VdecChn);
        LOGD("unbind demux %d & vdec %d: %x", dmxCtx->dmxChn, vdecChn, ret);

        ret = AW_MPI_SYS_UnBind(&ClockChn, &DmxChn);
        LOGD("unbind demux %d & clock %d: %x", dmxCtx->dmxChn, dmxCtx->clkChn, ret);
    }

    return ret;
}

ERRORTYPE awdmx_start(AwdmxContext_t *dmxCtx) {
    ERRORTYPE ret = SUCCESS;

    if (dmxCtx->dmxChn >= 0) {
        ret = AW_MPI_DEMUX_Start(dmxCtx->dmxChn);
        LOGD("start: %x", ret);
    }

    if (dmxCtx->clkChn >= 0) {
        ret = AW_MPI_CLOCK_Start(dmxCtx->clkChn);
    }

    dmxCtx->bEof = false;

    return ret;
}

ERRORTYPE awdmx_pause(AwdmxContext_t *dmxCtx) {
    ERRORTYPE ret = SUCCESS;

    if (dmxCtx->dmxChn >= 0) {
        ret = AW_MPI_DEMUX_Pause(dmxCtx->dmxChn);
        LOGD("pause: %x", ret);

        if (ret == FAILURE) {
            ret = SUCCESS;
        }
    }

    if (dmxCtx->clkChn >= 0) {
        ret = AW_MPI_CLOCK_Pause(dmxCtx->clkChn);
    }
    if ((ret == ERR_CLOCK_INCORRECT_STATE_TRANSITION) && dmxCtx->bEof) {
        ret = SUCCESS;
    }

    return ret;
}

ERRORTYPE awdmx_stop(AwdmxContext_t *dmxCtx) {
    ERRORTYPE ret = SUCCESS;

    if (dmxCtx->dmxChn >= 0) {
        ret = AW_MPI_DEMUX_Stop(dmxCtx->dmxChn);
        LOGD("stop: %x", ret);

        if (ret == FAILURE) {
            ret = SUCCESS;
        }
    }

    if (dmxCtx->clkChn >= 0) {
        ret = AW_MPI_CLOCK_Stop(dmxCtx->clkChn);
    }

    return ret;
}

ERRORTYPE awdmx_seekTo(AwdmxContext_t *dmxCtx, int seekTime) {
    ERRORTYPE ret = SUCCESS;

    if (dmxCtx->dmxChn >= 0) {
        ret = AW_MPI_DEMUX_Seek(dmxCtx->dmxChn, seekTime);
        LOGD("seek to %d: %x", seekTime, ret);
        if (ret == SUCCESS) {
            dmxCtx->seekTime = seekTime;
        }
    }

    if (dmxCtx->clkChn >= 0) {
        AW_MPI_CLOCK_Seek(dmxCtx->clkChn);
    }

    return ret;
}

bool awdmx_isEOF(AwdmxContext_t *dmxCtx) {
    return dmxCtx->bEof;
}
