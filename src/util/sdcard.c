#include "sdcard.h"
#include "platform/paths.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif
#include <unistd.h>

static int g_sdcard_free_size = 0;

bool sdcard_mounted() {
    struct stat mountpoint;
    struct stat mountpoint_parent;

    // Fetch mountpoint and mountpoint parent dev_id
    if (stat(path_extsd(), &mountpoint) == 0 &&
        stat("/mnt", &mountpoint_parent) == 0) {
        // iff the dev ids _do not_ match there is a filesystem mounted
        return (mountpoint.st_dev != mountpoint_parent.st_dev);
    }

    return false;
}

bool sdcard_inserted() {
    return access(SD_BLOCK_DEVICE, F_OK) == 0;
}

void sdcard_update_free_size() {
    struct statfs info;
    if (statfs(path_extsd(), &info) == 0)
        g_sdcard_free_size = (info.f_bsize * info.f_bavail) >> 20; // in MB
    else
        g_sdcard_free_size = 0;
}

bool sdcard_is_full() {
    return g_sdcard_free_size < 103;
}

/*
return in MB
*/
int sdcard_free_size() {
    return g_sdcard_free_size;
}

// FAT16/32 volumes carry a clean-shutdown flag in the second FAT entry. A
// clean flag means the automatic unmount/fsck/remount cycle adds no value;
// unreadable or unsupported media stays on the safe, dirty path.
bool sdcard_filesystem_dirty() {
    int fd = open(SD_BLOCK_DEVICE "p1", O_RDONLY);
    if (fd < 0)
        fd = open(SD_BLOCK_DEVICE, O_RDONLY);
    if (fd < 0)
        return true;

    bool dirty = true;
    uint8_t boot_sector[512];
    if (read(fd, boot_sector, sizeof(boot_sector)) == (ssize_t)sizeof(boot_sector)) {
        uint16_t const bytes_per_sector = boot_sector[11] | (boot_sector[12] << 8);
        uint16_t const reserved_sectors = boot_sector[14] | (boot_sector[15] << 8);
        uint16_t const fat_size_16 = boot_sector[22] | (boot_sector[23] << 8);

        if (bytes_per_sector >= 512 && reserved_sectors) {
            uint8_t fat_entry[8];
            off_t const fat_offset = (off_t)reserved_sectors * bytes_per_sector;
            if (pread(fd, fat_entry, sizeof(fat_entry), fat_offset) == (ssize_t)sizeof(fat_entry)) {
                if (fat_size_16 == 0) { // FAT32: clean-shutdown is bit 27.
                    uint32_t const fat_entry_1 = fat_entry[4] |
                                                 (fat_entry[5] << 8) |
                                                 (fat_entry[6] << 16) |
                                                 ((uint32_t)fat_entry[7] << 24);
                    dirty = !(fat_entry_1 & 0x08000000);
                } else { // FAT16: clean-shutdown is bit 15.
                    uint16_t const fat_entry_1 = fat_entry[2] | (fat_entry[3] << 8);
                    dirty = !(fat_entry_1 & 0x8000);
                }
            }
        }
    }

    close(fd);
    return dirty;
}
