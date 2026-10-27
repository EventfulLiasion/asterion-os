#ifndef ASTERION_KERNEL_ELF64_LOADER_H
#define ASTERION_KERNEL_ELF64_LOADER_H

#include <stddef.h>
#include <stdint.h>
#include "vm_core.h"

#define ELF64_MAX_LOAD_SEGMENTS 16u
#define ELF64_MAX_IMAGE_PAGES VM_MAX_OWNED_MAPPINGS

typedef enum {
    ELF64_OK = 0,
    ELF64_INVALID = -1,
    ELF64_UNSUPPORTED = -2,
    ELF64_NO_MEMORY = -3,
    ELF64_MAPPING_FAILED = -4,
    ELF64_ROLLBACK_FAILED = -5
} elf64_status_t;

typedef struct {
    uint64_t virtual_address;
    uint64_t memory_size;
    uint64_t file_offset;
    uint64_t file_size;
    uint32_t flags;
    uint32_t first_page;
    uint32_t page_count;
} elf64_segment_t;

typedef struct {
    uint64_t entry;
    uint32_t segment_count;
    uint32_t page_count;
    elf64_segment_t segments[ELF64_MAX_LOAD_SEGMENTS];
} elf64_plan_t;

typedef struct {
    uint64_t virtual_address;
    uint64_t physical_address;
} elf64_page_t;

typedef struct {
    uint64_t entry;
    uint32_t page_count;
    elf64_page_t pages[ELF64_MAX_IMAGE_PAGES];
} elf64_loaded_image_t;

/* Memory operations are supplied by the kernel or by a host test harness. */
typedef struct {
    void *context;
    int (*page_is_unmapped)(void *context, uint64_t virtual_address);
    int (*page_allocate)(void *context, uint64_t *physical_address, uint8_t **writable);
    int (*page_release)(void *context, uint64_t physical_address);
    int (*map_page)(void *context, uint64_t virtual_address,
                    uint64_t physical_address, uint32_t flags);
    int (*unmap_page)(void *context, uint64_t virtual_address);
} elf64_memory_ops_t;

elf64_status_t elf64_parse(const void *image, size_t image_size, uint8_t levels,
                elf64_plan_t *plan);
elf64_status_t elf64_load(const void *image, size_t image_size, uint8_t levels,
                          uint8_t nx_enabled, const elf64_memory_ops_t *ops,
                          elf64_loaded_image_t *loaded);
elf64_status_t elf64_unload(const elf64_memory_ops_t *ops,
                            elf64_loaded_image_t *loaded);

#endif

