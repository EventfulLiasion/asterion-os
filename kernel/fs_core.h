#ifndef ASTERION_KERNEL_FS_CORE_H
#define ASTERION_KERNEL_FS_CORE_H
#include <stddef.h>
#include <stdint.h>
#include "block_core.h"
#define AOSFS_MAX_FILES 7u
#define AOSFS_NAME_CAPACITY 32u
enum { FS_OK=0, FS_INVALID=-1, FS_IO_ERROR=-2, FS_CORRUPT=-3, FS_NOT_FOUND=-4, FS_BUFFER_TOO_SMALL=-5 };
typedef struct {
    char name[AOSFS_NAME_CAPACITY];
    uint32_t first_sector, byte_size, sector_count, checksum;
} fs_file_t;
typedef struct {
    block_device_t *device;
    uint32_t total_sectors;
    uint32_t data_start;
    uint32_t file_count;
    fs_file_t files[AOSFS_MAX_FILES];
    uint8_t mounted;
} fs_volume_t;
int fs_mount(fs_volume_t *volume, block_device_t *device);
uint32_t fs_file_count(const fs_volume_t *volume);
const fs_file_t *fs_file_at(const fs_volume_t *volume, uint32_t index);
const fs_file_t *fs_lookup(const fs_volume_t *volume, const char *name);
int fs_read_file(const fs_volume_t *volume, const char *name, void *buffer,
                 size_t capacity, size_t *bytes_read);
uint32_t fs_checksum(const void *data, size_t size);
#endif
