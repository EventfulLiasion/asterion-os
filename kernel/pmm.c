#include <stddef.h>
#include <stdint.h>
#include "pmm.h"

/* Two bits per 4 KiB frame: reserved=0, free=1, allocated=2. The bitmap
   itself is kernel image storage, which UEFI reports as LoaderData. */
#define FRAME_COUNT (PMM_MAX_PHYSICAL_ADDRESS / PMM_PAGE_SIZE)
#define STATE_BYTES ((FRAME_COUNT + 3) / 4)
#define FRAME_RESERVED 0u
#define FRAME_FREE 1u
#define FRAME_ALLOCATED 2u

static uint8_t frame_state[STATE_BYTES];
static uint64_t conventional_pages;
static uint64_t initial_usable_pages;
static uint64_t free_pages;
static uint64_t allocated_pages;
static int initialized;

static uint8_t state_get(uint64_t frame) {
    unsigned shift = (unsigned)((frame & 3u) * 2u);
    return (uint8_t)((frame_state[frame >> 2] >> shift) & 3u);
}

static void state_set(uint64_t frame, uint8_t state) {
    unsigned shift = (unsigned)((frame & 3u) * 2u);
    uint8_t mask = (uint8_t)(3u << shift);
    frame_state[frame >> 2] = (uint8_t)((frame_state[frame >> 2] & (uint8_t)~mask) |
                                         (uint8_t)(state << shift));
}

static uint64_t clipped_end(uint64_t start, uint64_t pages) {
    uint64_t bytes = pages * PMM_PAGE_SIZE;
    uint64_t end = start + bytes;
    return end < PMM_MAX_PHYSICAL_ADDRESS ? end : PMM_MAX_PHYSICAL_ADDRESS;
}

static void mark_range(uint64_t start, uint64_t pages, uint8_t state) {
    if (start >= PMM_MAX_PHYSICAL_ADDRESS) return;
    uint64_t end = clipped_end(start, pages);
    for (uint64_t frame = start / PMM_PAGE_SIZE; frame < end / PMM_PAGE_SIZE; ++frame)
        state_set(frame, state);
}

int pmm_init(const boot_info_t *info) {
    initialized = 0;
    conventional_pages = 0;
    initial_usable_pages = 0;
    free_pages = 0;
    allocated_pages = 0;
    for (uint64_t i = 0; i < sizeof(frame_state); ++i) frame_state[i] = 0;

    if (!info || info->version != 1 || !info->memory_map ||
        !info->memory_map_size || !info->memory_descriptor_count ||
        info->memory_map_size > SIZE_MAX || info->memory_descriptor_size > SIZE_MAX ||
        info->memory_descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        (uintptr_t)info->memory_map % _Alignof(EFI_MEMORY_DESCRIPTOR) ||
        (uintptr_t)info->memory_map > UINTPTR_MAX - (uintptr_t)info->memory_map_size ||
        info->memory_map_size % info->memory_descriptor_size ||
        info->memory_descriptor_count != info->memory_map_size / info->memory_descriptor_size ||
        info->memory_map_size % info->memory_descriptor_size) return 0;

    const uint8_t *map = (const uint8_t *)info->memory_map;
    for (uint64_t i = 0; i < info->memory_descriptor_count; ++i) {
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)(map + i * info->memory_descriptor_size);
        if (!d->pages || d->physical_start % PMM_PAGE_SIZE ||
            d->pages > UINT64_MAX / PMM_PAGE_SIZE ||
            d->physical_start > UINT64_MAX - d->pages * PMM_PAGE_SIZE) return 0;
        if (d->type == EFI_MEMORY_CONVENTIONAL && d->physical_start < PMM_MAX_PHYSICAL_ADDRESS) {
            uint64_t end = clipped_end(d->physical_start, d->pages);
            uint64_t first = d->physical_start / PMM_PAGE_SIZE;
            uint64_t last = end / PMM_PAGE_SIZE;
            for (uint64_t frame = first; frame < last; ++frame) {
                /* Firmware descriptors should be disjoint. Reject ambiguous maps
                   instead of accidentally making overlapping ranges allocatable. */
                if (state_get(frame) != FRAME_RESERVED) return 0;
                state_set(frame, FRAME_FREE);
                ++conventional_pages;
            }
        }
    }

    /* Any non-conventional descriptor wins over a contradictory conventional
       range. UEFI loader/image/map allocations and MMIO therefore stay reserved. */
    for (uint64_t i = 0; i < info->memory_descriptor_count; ++i) {
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)(map + i * info->memory_descriptor_size);
        if (d->type != EFI_MEMORY_CONVENTIONAL)
            mark_range(d->physical_start, d->pages, FRAME_RESERVED);
    }

    /* Frame zero is never handed out, even if firmware reports it as free. */
    state_set(0, FRAME_RESERVED);
    if (info->framebuffer_size) {
        if (info->framebuffer_base > UINT64_MAX - info->framebuffer_size) return 0;
        uint64_t start = info->framebuffer_base & ~(PMM_PAGE_SIZE - 1);
        uint64_t end = info->framebuffer_base + info->framebuffer_size;
        if (end > PMM_MAX_PHYSICAL_ADDRESS) end = PMM_MAX_PHYSICAL_ADDRESS;
        if (start < end)
            mark_range(start, (end - start + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE, FRAME_RESERVED);
    }

    for (uint64_t frame = 0; frame < FRAME_COUNT; ++frame)
        if (state_get(frame) == FRAME_FREE) ++initial_usable_pages;
    free_pages = initial_usable_pages;
    initialized = 1;
    return 1;
}

int pmm_alloc(uint64_t page_count, uint64_t alignment_pages,
              uint64_t max_address_exclusive, uint64_t *physical_address) {
    if (!initialized || !physical_address || !page_count || !alignment_pages ||
        (alignment_pages & (alignment_pages - 1)) || page_count > free_pages) return 0;
    if (max_address_exclusive > PMM_MAX_PHYSICAL_ADDRESS)
        max_address_exclusive = PMM_MAX_PHYSICAL_ADDRESS;
    uint64_t frame_limit = max_address_exclusive / PMM_PAGE_SIZE;
    if (frame_limit < page_count) return 0;
    for (uint64_t first = 1; first <= frame_limit - page_count;) {
        uint64_t remainder = first & (alignment_pages - 1);
        if (remainder) {
            uint64_t adjustment = alignment_pages - remainder;
            if (adjustment > frame_limit - first) break;
            first += adjustment;
        }
        if (first > frame_limit - page_count) break;
        uint64_t offset = 0;
        while (offset < page_count && state_get(first + offset) == FRAME_FREE) ++offset;
        if (offset == page_count) {
            for (uint64_t i = 0; i < page_count; ++i) state_set(first + i, FRAME_ALLOCATED);
            free_pages -= page_count;
            allocated_pages += page_count;
            *physical_address = first * PMM_PAGE_SIZE;
            return 1;
        }
        first += offset + 1;
    }
    return 0;
}

int pmm_free(uint64_t physical_address, uint64_t page_count) {
    if (!initialized || !page_count || physical_address % PMM_PAGE_SIZE ||
        physical_address >= PMM_MAX_PHYSICAL_ADDRESS ||
        page_count > (PMM_MAX_PHYSICAL_ADDRESS - physical_address) / PMM_PAGE_SIZE)
        return 0;
    uint64_t first = physical_address / PMM_PAGE_SIZE;
    for (uint64_t i = 0; i < page_count; ++i)
        if (state_get(first + i) != FRAME_ALLOCATED) return 0;
    for (uint64_t i = 0; i < page_count; ++i) state_set(first + i, FRAME_FREE);
    allocated_pages -= page_count;
    free_pages += page_count;
    return 1;
}

void pmm_get_stats(pmm_stats_t *stats) {
    if (!stats) return;
    stats->conventional_pages = conventional_pages;
    stats->usable_pages = initial_usable_pages;
    stats->free_pages = free_pages;
    stats->allocated_pages = allocated_pages;
    stats->reserved_conventional_pages = conventional_pages - initial_usable_pages;
}

int pmm_self_test(void) {
    uint64_t before = free_pages;
    uint64_t address = 0;
    if (!pmm_alloc(2, 4, PMM_MAX_PHYSICAL_ADDRESS, &address) ||
        address % (4 * PMM_PAGE_SIZE) || free_pages != before - 2) return 0;
    if (pmm_free(address + 1, 2) || pmm_free(0, 1) || pmm_free(address, 3)) return 0;
    if (!pmm_free(address, 2) || pmm_free(address, 2) || free_pages != before) return 0;
    /* The allocator returns the lowest free page. Restricting the next search
       to that page proves the bounded search reports exhaustion without
       consuming the rest of the machine's memory. */
    if (!pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, &address)) return 0;
    uint64_t bounded_address = address;
    uint64_t bounded_limit = bounded_address + PMM_PAGE_SIZE;
    uint64_t unexpected_address = 0;
    if (pmm_alloc(1, 1, bounded_limit, &unexpected_address)) {
        (void)pmm_free(unexpected_address, 1);
        (void)pmm_free(bounded_address, 1);
        return 0;
    }
    if (!pmm_free(bounded_address, 1)) return 0;
    if (free_pages != before) return 0;
    return 1;
}
