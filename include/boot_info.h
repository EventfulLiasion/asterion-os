#ifndef ASTERION_BOOT_INFO_H
#define ASTERION_BOOT_INFO_H

#include <stdint.h>
#include <stddef.h>

#define EFI_MEMORY_CONVENTIONAL 7U

typedef struct {
    uint32_t type, pad;
    uint64_t physical_start, virtual_start, pages, attributes;
} EFI_MEMORY_DESCRIPTOR;
_Static_assert(sizeof(EFI_MEMORY_DESCRIPTOR) == 40, "UEFI x64 descriptor ABI mismatch");

/* Version 1 is owned by the kernel after ExitBootServices succeeds. */
typedef struct {
    uint32_t version;
    uint32_t framebuffer_width, framebuffer_height, framebuffer_stride, pixel_format;
    uint64_t framebuffer_base, framebuffer_size;
    const EFI_MEMORY_DESCRIPTOR *memory_map;
    uint64_t memory_map_size, memory_descriptor_size, memory_descriptor_count;
    uint64_t conventional_memory_bytes;
    char cpu_vendor[13];
    uint32_t cpu_signature, cpu_feature_ecx, cpu_feature_edx, cpu_extended_feature_edx;
    char cpu_brand[49];
} boot_info_t;

_Static_assert(_Alignof(boot_info_t) == 8, "boot_info_t alignment is part of the handoff ABI");
_Static_assert(offsetof(boot_info_t, framebuffer_base) == 24, "boot_info_t v1 framebuffer offset changed");
_Static_assert(offsetof(boot_info_t, memory_map) == 40, "boot_info_t v1 memory-map offset changed");
_Static_assert(sizeof(boot_info_t) == 168, "boot_info_t v1 size changed; bump the ABI version");

#endif
