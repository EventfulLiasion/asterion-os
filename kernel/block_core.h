#ifndef ASTERION_KERNEL_BLOCK_CORE_H
#define ASTERION_KERNEL_BLOCK_CORE_H
#include <stddef.h>
#include <stdint.h>
#define BLOCK_MAX_READ_SECTORS 128u
enum { BLOCK_OK=0, BLOCK_INVALID=-1, BLOCK_BOUNDS=-2, BLOCK_IO_ERROR=-3 };
typedef int (*block_read_fn)(void *context, uint64_t lba, uint32_t count, void *buffer);
typedef struct {
    void *context;
    block_read_fn read;
    uint64_t sector_count;
    uint32_t sector_size;
} block_device_t;
int block_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer);
#endif
