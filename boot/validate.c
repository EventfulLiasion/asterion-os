#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include "../include/boot_validate.h"

#define PAGE_BYTES UINT64_C(4096)

int boot_info_validate(const boot_info_t *info) {
    if (!info || info->version != 1) return 0;
    if (info->framebuffer_width < 320 || info->framebuffer_height < 240 ||
        info->framebuffer_width > INT_MAX || info->framebuffer_height > INT_MAX ||
        info->framebuffer_stride < info->framebuffer_width || info->pixel_format > 1 ||
        !info->framebuffer_base || !info->framebuffer_size ||
        info->framebuffer_size > UINT64_MAX - info->framebuffer_base) return 0;
    uint64_t rows = info->framebuffer_stride;
    if (rows > UINT64_MAX / info->framebuffer_height) return 0;
    uint64_t pixels = rows * info->framebuffer_height;
    if (pixels > UINT64_MAX / 4 || pixels * 4 > info->framebuffer_size) return 0;

    if (!info->memory_map || !info->memory_map_size ||
        info->memory_map_size > SIZE_MAX || info->memory_descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        info->memory_descriptor_size > SIZE_MAX ||
        (uintptr_t)info->memory_map % _Alignof(EFI_MEMORY_DESCRIPTOR) ||
        info->memory_map_size % info->memory_descriptor_size ||
        info->memory_descriptor_count != info->memory_map_size / info->memory_descriptor_size ||
        (uintptr_t)info->memory_map > UINTPTR_MAX - (uintptr_t)info->memory_map_size) return 0;
    if (info->cpu_vendor[12] != '\0' || info->cpu_brand[48] != '\0') return 0;

    uint64_t conventional = 0;
    const uint8_t *map = (const uint8_t *)info->memory_map;
    for (uint64_t i = 0; i < info->memory_descriptor_count; ++i) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(map + i * info->memory_descriptor_size);
        if (!d->pages || d->physical_start % PAGE_BYTES || d->pages > UINT64_MAX / PAGE_BYTES) return 0;
        uint64_t bytes = d->pages * PAGE_BYTES;
        if (bytes > UINT64_MAX - d->physical_start) return 0;
        if (d->type == 7) {
            if (bytes > UINT64_MAX - conventional) return 0;
            conventional += bytes;
        }
    }
    return conventional == info->conventional_memory_bytes;
}
