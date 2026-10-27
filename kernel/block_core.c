#include <stdint.h>
#include "block_core.h"
int block_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer) {
    if (!device || !device->read || !device->sector_size || !count || !buffer ||
        count > BLOCK_MAX_READ_SECTORS) return BLOCK_INVALID;
    if (lba >= device->sector_count || (uint64_t)count > device->sector_count - lba)
        return BLOCK_BOUNDS;
    if ((size_t)count > SIZE_MAX / device->sector_size) return BLOCK_BOUNDS;
    size_t bytes = (size_t)count * device->sector_size;
    uintptr_t start = (uintptr_t)buffer;
    if (start > UINTPTR_MAX - bytes) return BLOCK_BOUNDS;
    return device->read(device->context, lba, count, buffer) == BLOCK_OK
               ? BLOCK_OK : BLOCK_IO_ERROR;
}
