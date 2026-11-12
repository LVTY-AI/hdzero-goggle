#include "util/wav_info.h"

#include <stdio.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t wav_info_duration_ms(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return 0;

    uint8_t head[12], chunk[8], fmt[16];
    uint32_t rate = 0, bytes_per_frame = 0;
    uint64_t data_len = 0;
    int ok = fread(head, 1, sizeof(head), fp) == sizeof(head) && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4);

    while (ok && fread(chunk, 1, sizeof(chunk), fp) == sizeof(chunk)) {
        uint32_t size = rd32(chunk + 4);
        if (!memcmp(chunk, "data", 4)) {
            long pos = ftell(fp);
            if (pos >= 0 && fseek(fp, 0, SEEK_END) == 0) {
                long end = ftell(fp);
                data_len = (end > pos && (uint64_t)(end - pos) < size) ? (uint64_t)(end - pos) : size;
            }
            break;
        }
        long skip = (long)size + (size & 1); // chunks are padded to an even size
        if (!memcmp(chunk, "fmt ", 4) && size >= sizeof(fmt)) {
            if (fread(fmt, 1, sizeof(fmt), fp) != sizeof(fmt) || rd16(fmt) != 1)
                break;
            rate = rd32(fmt + 4);
            bytes_per_frame = rd16(fmt + 2) * (rd16(fmt + 14) / 8);
            skip -= sizeof(fmt);
        }
        if (fseek(fp, skip, SEEK_CUR) != 0)
            break;
    }
    fclose(fp);

    if (rate == 0 || bytes_per_frame == 0)
        return 0;
    return (uint32_t)(data_len * 1000 / ((uint64_t)rate * bytes_per_frame));
}
