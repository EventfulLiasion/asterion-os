#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../kernel/pmm.h"

int main(void) {
    EFI_MEMORY_DESCRIPTOR map[2] = {0};
    map[0].type = EFI_MEMORY_CONVENTIONAL;
    map[0].physical_start = 0x1000;
    map[0].pages = 8;
    map[1].type = 0; /* Reserved firmware range must not become allocatable. */
    map[1].physical_start = 0x9000;
    map[1].pages = 1;

    boot_info_t info = {0};
    info.version = 1;
    info.framebuffer_base = 0x3000;
    info.framebuffer_size = 0x1000;
    info.memory_map = map;
    info.memory_map_size = sizeof(map);
    info.memory_descriptor_size = sizeof(map[0]);
    info.memory_descriptor_count = 2;
    assert(pmm_init(&info));
    assert(pmm_self_test());

    pmm_stats_t stats;
    pmm_get_stats(&stats);
    assert(stats.conventional_pages == 8);
    assert(stats.usable_pages == 7); /* Framebuffer page is withheld. */
    assert(stats.reserved_conventional_pages == 1);
    assert(stats.free_pages == 7 && stats.allocated_pages == 0);

    uint64_t address = 0;
    assert(!pmm_alloc(0, 1, PMM_MAX_PHYSICAL_ADDRESS, &address));
    assert(!pmm_alloc(1, 3, PMM_MAX_PHYSICAL_ADDRESS, &address)); /* Alignment must be power of two. */
    assert(!pmm_alloc(1, 1, 0x1000, &address)); /* Below frame zero / no full page. */
    assert(!pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, NULL));

    assert(pmm_alloc(2, 4, 0x9000, &address));
    assert(address == 0x4000);
    assert(!pmm_free(address + 1, 2));
    assert(!pmm_free(0, 1));
    assert(!pmm_free(0x3000, 1)); /* Framebuffer reservation. */
    assert(!pmm_free(address, 3)); /* Range includes a non-allocated frame; atomic rejection. */
    pmm_get_stats(&stats);
    assert(stats.free_pages == 5 && stats.allocated_pages == 2);

    assert(pmm_free(address, 2));
    assert(!pmm_free(address, 2)); /* Double free. */
    assert(!pmm_free(PMM_MAX_PHYSICAL_ADDRESS, 1));
    assert(!pmm_free(0x9000, 1)); /* Reserved descriptor. */

    assert(pmm_alloc(3, 1, PMM_MAX_PHYSICAL_ADDRESS, &address));
    assert(!pmm_alloc(5, 1, PMM_MAX_PHYSICAL_ADDRESS, &address)); /* No contiguous run. */
    assert(pmm_free(address, 3));

    uint64_t allocated[7];
    for (unsigned i = 0; i < 7; ++i)
        assert(pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, &allocated[i]));
    assert(!pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, &address)); /* Exhaustion. */
    for (unsigned i = 0; i < 7; ++i) assert(pmm_free(allocated[i], 1));
    pmm_get_stats(&stats);
    assert(stats.free_pages == stats.usable_pages && stats.allocated_pages == 0);

    /* A contradictory overlapping conventional range is rejected. */
    map[1].type = EFI_MEMORY_CONVENTIONAL;
    map[1].physical_start = 0x8000;
    map[1].pages = 2;
    assert(!pmm_init(&info));
    return 0;
}
