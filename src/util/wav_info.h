#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Playing time in milliseconds of a PCM WAV file, read from its header. The data
// length is capped at what the file actually holds. 0 if the file is missing or
// is not a PCM WAV.
uint32_t wav_info_duration_ms(const char *path);

#ifdef __cplusplus
}
#endif
