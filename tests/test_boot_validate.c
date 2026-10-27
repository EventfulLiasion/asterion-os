#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../include/boot_validate.h"

static EFI_MEMORY_DESCRIPTOR map[2];

static boot_info_t valid_info(void) {
    memset(map, 0, sizeof(map));
    map[0].type = 7; map[0].physical_start = 0x1000; map[0].pages = 2;
    map[1].type = 0; map[1].physical_start = 0x8000; map[1].pages = 1;
    boot_info_t info = {0};
    info.version = 1;
    info.framebuffer_width = 640; info.framebuffer_height = 480;
    info.framebuffer_stride = 640; info.pixel_format = 0;
    info.framebuffer_base = 0x100000; info.framebuffer_size = 640u * 480u * 4u;
    info.memory_map = map; info.memory_map_size = sizeof(map);
    info.memory_descriptor_size = sizeof(map[0]); info.memory_descriptor_count = 2;
    info.conventional_memory_bytes = 8192;
    return info;
}

int main(void) {
    boot_info_t info = valid_info();
    assert(boot_info_validate(&info));
    assert(!boot_info_validate(NULL));

    info = valid_info(); info.version = 2; assert(!boot_info_validate(&info));
    info = valid_info(); info.memory_descriptor_count = 1; assert(!boot_info_validate(&info));
    info = valid_info(); info.memory_descriptor_size = 39; assert(!boot_info_validate(&info));
    info = valid_info(); info.memory_map_size = 79; assert(!boot_info_validate(&info));
    info = valid_info(); info.memory_map = NULL; assert(!boot_info_validate(&info));
    info = valid_info(); info.memory_map = (const EFI_MEMORY_DESCRIPTOR *)(uintptr_t)(UINTPTR_MAX - 7); assert(!boot_info_validate(&info));
    info = valid_info(); info.cpu_vendor[12] = 'X'; assert(!boot_info_validate(&info));

    info = valid_info(); info.pixel_format = 2; assert(!boot_info_validate(&info));
    info = valid_info(); info.framebuffer_width = UINT32_MAX; assert(!boot_info_validate(&info));
    info = valid_info(); info.framebuffer_stride = 639; assert(!boot_info_validate(&info));
    info = valid_info(); info.framebuffer_size--; assert(!boot_info_validate(&info));
    info = valid_info(); info.framebuffer_base = UINT64_MAX - 3; assert(!boot_info_validate(&info));

    info = valid_info(); map[0].physical_start = 0x1001; assert(!boot_info_validate(&info));
    info = valid_info(); map[0].pages = 0; assert(!boot_info_validate(&info));
    info = valid_info(); map[0].pages = UINT64_MAX / 4096 + 1; assert(!boot_info_validate(&info));
    info = valid_info(); map[0].physical_start = UINT64_MAX - 0xFFF; map[0].pages = 2; assert(!boot_info_validate(&info));
    info = valid_info(); info.conventional_memory_bytes = 4096; assert(!boot_info_validate(&info));
    return 0;
}
