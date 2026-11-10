#include <stdint.h>
#include <stdio.h>

#include "util/wav_info.h"

static int failures;

// 48 kHz 16-bit stereo; the data chunk announces `claim` bytes and holds `have`.
static uint32_t duration(uint32_t claim, uint32_t have) {
    const char hdr[40] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x80\xbb\0\0\0\xee\x02\0\x04\0\x10\0data";
    FILE *fp = fopen("wav_info_case.wav", "wb");
    fwrite(hdr, 1, sizeof(hdr), fp);
    fwrite(&claim, 4, 1, fp);
    for (uint32_t i = 0; i < have; i++)
        fputc(0, fp);
    fclose(fp);
    uint32_t ms = wav_info_duration_ms("wav_info_case.wav");
    remove("wav_info_case.wav");
    return ms;
}

static void expect(const char *what, uint32_t got, uint32_t want) {
    if (got != want) {
        printf("FAIL %s: %u, wanted %u\n", what, got, want);
        failures++;
    }
}

int main(void) {
    expect("9.5 s", duration(1824000, 1824000), 9500);
    expect("announced size beyond the file", duration(0xffffffffu, 96000), 500);
    expect("empty file", wav_info_duration_ms("/dev/null"), 0);
    expect("missing file", wav_info_duration_ms("/nonexistent.wav"), 0);
    printf(failures ? "wav_info: FAILED\n" : "wav_info: ok\n");
    return failures != 0;
}
