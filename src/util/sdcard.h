#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#define SD_BLOCK_DEVICE "/dev/mmcblk0"

bool sdcard_mounted();
bool sdcard_inserted();
void sdcard_update_free_size();
int sdcard_free_size();
bool sdcard_is_full();
// Returns true when the FAT clean-shutdown flag says a check is warranted, or
// when the volume cannot be identified safely.
bool sdcard_filesystem_dirty();

#ifdef __cplusplus
}
#endif
