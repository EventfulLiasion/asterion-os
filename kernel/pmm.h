#ifndef ASTERION_KERNEL_PMM_H
#define ASTERION_KERNEL_PMM_H

#include <stdint.h>
#include "../include/boot_info.h"

#define PMM_PAGE_SIZE UINT64_C(4096)
#define PMM_MAX_PHYSICAL_ADDRESS (UINT64_C(16) * 1024 * 1024 * 1024)

typedef struct {
    uint64_t conventional_pages;
    uint64_t usable_pages;
    uint64_t free_pages;
    uint64_t allocated_pages;
    uint64_t reserved_conventional_pages;
} pmm_stats_t;

/* Single-CPU allocator. Only UEFI conventional pages enter the free pool. */
int pmm_init(const boot_info_t *info);
int pmm_alloc(uint64_t page_count, uint64_t alignment_pages,
              uint64_t max_address_exclusive, uint64_t *physical_address);
int pmm_free(uint64_t physical_address, uint64_t page_count);
void pmm_get_stats(pmm_stats_t *stats);
int pmm_self_test(void);

#endif
