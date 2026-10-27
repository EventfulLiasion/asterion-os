#include <stddef.h>
#include <stdint.h>
#include "heap.h"
#include "heap_core.h"
#include "pmm.h"
#include "vm.h"
#include "vm_core.h"
#include "../drivers/serial.h"

static heap_core_t kernel_heap;
static uint64_t heap_frames[KHEAP_PAGE_COUNT];
static uint32_t heap_mapped_pages;

static int heap_map_backing(void) {
    uint64_t ignored;
    for (uint32_t i = 0; i < KHEAP_PAGE_COUNT; ++i) {
        if (vm_translate_address(KHEAP_BASE + (uint64_t)i * VM_PAGE_SIZE,
                                 &ignored, NULL) != VM_NOT_MAPPED) return 0;
    }

    uint32_t allocated = 0;
    for (uint32_t i = 0; i < KHEAP_PAGE_COUNT; ++i) {
        if (!pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, &heap_frames[i])) goto rollback;
        ++allocated;
        if (vm_map_page(KHEAP_BASE + (uint64_t)i * VM_PAGE_SIZE,
                        heap_frames[i], VM_MAP_WRITE) != VM_OK) goto rollback;
        ++heap_mapped_pages;
    }

    volatile uint8_t *memory = (volatile uint8_t *)(uintptr_t)KHEAP_BASE;
    for (size_t i = 0; i < KHEAP_CAPACITY; ++i) memory[i] = 0;
    return 1;

rollback:
    for (uint32_t i = 0; i < heap_mapped_pages; ++i) {
        if (vm_unmap_page(KHEAP_BASE + (uint64_t)i * VM_PAGE_SIZE) != VM_OK) {
            serial_log(SERIAL_ERROR, "HEAP rollback could not remove a mapped backing page.");
            return 0;
        }
    }
    heap_mapped_pages = 0;
    for (uint32_t i = 0; i < allocated; ++i)
        if (!pmm_free(heap_frames[i], 1))
            serial_log(SERIAL_ERROR, "HEAP rollback could not return a backing frame to PMM.");
    return 0;
}

int kheap_init(void) {
    if (kernel_heap.initialized || !heap_map_backing()) return 0;
    if (!heap_core_init(&kernel_heap, (void *)(uintptr_t)KHEAP_BASE, KHEAP_CAPACITY)) {
        for (uint32_t i = 0; i < heap_mapped_pages; ++i) {
            (void)vm_unmap_page(KHEAP_BASE + (uint64_t)i * VM_PAGE_SIZE);
            (void)pmm_free(heap_frames[i], 1);
        }
        heap_mapped_pages = 0;
        return 0;
    }
    char message[96] = "Kernel heap mapped at ";
    char address[19];
    static const char digits[] = "0123456789ABCDEF";
    address[0] = '0'; address[1] = 'x';
    for (unsigned i = 0; i < 16; ++i)
        address[2 + i] = digits[(KHEAP_BASE >> (60 - i * 4)) & 15];
    address[18] = 0;
    size_t at = 22;
    for (unsigned i = 0; address[i] && at + 1 < sizeof(message); ++i) message[at++] = address[i];
    message[at] = 0;
    serial_log(SERIAL_INFO, message);
    return 1;
}

void *kmalloc(size_t size) {
    return heap_core_alloc(&kernel_heap, size);
}

int kfree(void *pointer) {
    return heap_core_free(&kernel_heap, pointer);
}

void kheap_get_stats(kheap_stats_t *stats) {
    if (!stats) return;
    heap_core_stats_t core;
    heap_core_get_stats(&kernel_heap, &core);
    stats->base = KHEAP_BASE;
    stats->capacity = core.capacity;
    stats->free_bytes = core.free_bytes;
    stats->used_bytes = core.used_bytes;
    stats->allocated_blocks = core.allocated_blocks;
}

int kheap_self_test(void) {
    kheap_stats_t before;
    kheap_get_stats(&before);
    if (!before.capacity || kmalloc(0) != NULL || kmalloc(SIZE_MAX) != NULL) return 0;

    uint8_t *a = (uint8_t *)kmalloc(65);
    uint8_t *b = (uint8_t *)kmalloc(80);
    uint8_t *c = (uint8_t *)kmalloc(127);
    if (!a || !b || !c || ((uintptr_t)a & 15u) || ((uintptr_t)b & 15u) ||
        ((uintptr_t)c & 15u)) return 0;
    for (size_t i = 0; i < 65; ++i) a[i] = (uint8_t)(i ^ 0xA5u);
    for (size_t i = 0; i < 65; ++i) if (a[i] != (uint8_t)(i ^ 0xA5u)) return 0;

    if (!kfree(b) || kfree(b) || kfree((void *)(uintptr_t)(KHEAP_BASE + 1))) return 0;
    void *reused = kmalloc(32);
    if (reused != b || !reused || !kfree(reused)) return 0;
    if (!kfree(a) || !kfree(c)) return 0;

    /* Adjacent frees coalesce so a larger block can reuse the combined span. */
    a = (uint8_t *)kmalloc(80);
    b = (uint8_t *)kmalloc(80);
    c = (uint8_t *)kmalloc(80);
    if (!a || !b || !c || !kfree(b) || !kfree(a)) return 0;
    reused = kmalloc(176);
    if (reused != a || !reused || !kfree(reused) || !kfree(c)) return 0;

    enum { MAX_TEST_ALLOCS = 2048 };
    static void *allocations[MAX_TEST_ALLOCS];
    size_t count = 0;
    while (count < MAX_TEST_ALLOCS && (allocations[count] = kmalloc(128)) != NULL) ++count;
    if (!count || count == MAX_TEST_ALLOCS) return 0;
    for (size_t i = 0; i < count; ++i) if (!kfree(allocations[i])) return 0;

    kheap_stats_t after;
    kheap_get_stats(&after);
    return after.free_bytes == before.free_bytes && after.allocated_blocks == 0 &&
           after.used_bytes == before.used_bytes;
}
