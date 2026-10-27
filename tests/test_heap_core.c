#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../kernel/heap_core.h"

static union {
    uint8_t bytes[4096];
    uint64_t align;
} arena;

int main(void) {
    heap_core_t heap = {0};
    heap_core_stats_t stats;
    assert(!heap_core_init(NULL, arena.bytes, sizeof(arena.bytes)));
    assert(!heap_core_init(&heap, NULL, sizeof(arena.bytes)));
    assert(!heap_core_init(&heap, arena.bytes + 1, sizeof(arena.bytes) - 1));
    assert(!heap_core_init(&heap, arena.bytes, 32));
    assert(heap_core_init(&heap, arena.bytes, sizeof(arena.bytes)));
    assert(!heap_core_alloc(&heap, 0));
    assert(!heap_core_alloc(&heap, SIZE_MAX));

    void *a = heap_core_alloc(&heap, 33);
    void *b = heap_core_alloc(&heap, 64);
    void *c = heap_core_alloc(&heap, 127);
    assert(a && b && c);
    assert(!((uintptr_t)a & 15u) && !((uintptr_t)b & 15u) && !((uintptr_t)c & 15u));
    memset(a, 0xA5, 33);
    for (size_t i = 0; i < 33; ++i) assert(((uint8_t *)a)[i] == 0xA5);
    heap_core_get_stats(&heap, &stats);
    assert(stats.allocated_blocks == 3 && stats.used_bytes > 0 && stats.free_bytes < stats.capacity);

    assert(heap_core_free(&heap, b));
    assert(!heap_core_free(&heap, b));
    assert(!heap_core_free(&heap, arena.bytes + 1));
    assert(!heap_core_free(&heap, arena.bytes + 16));
    void *reuse = heap_core_alloc(&heap, 32);
    assert(reuse == b);
    assert(heap_core_free(&heap, reuse));
    assert(heap_core_free(&heap, a));
    assert(heap_core_free(&heap, c));

    a = heap_core_alloc(&heap, 80);
    b = heap_core_alloc(&heap, 80);
    c = heap_core_alloc(&heap, 80);
    assert(a && b && c);
    assert(heap_core_free(&heap, b));
    assert(heap_core_free(&heap, a));
    reuse = heap_core_alloc(&heap, 176);
    assert(reuse == a); /* Adjacent free extents coalesce. */
    assert(heap_core_free(&heap, reuse));
    assert(heap_core_free(&heap, c));

    heap_core_get_stats(&heap, &stats);
    assert(stats.free_bytes == stats.capacity && stats.allocated_blocks == 0 && stats.used_bytes == 0);
    assert(heap_core_alloc(&heap, stats.capacity + 1) == NULL);

    void *allocations[128];
    size_t count = 0;
    while (count < sizeof(allocations) / sizeof(allocations[0]) &&
           (allocations[count] = heap_core_alloc(&heap, 128)) != NULL) ++count;
    assert(count > 0 && count < sizeof(allocations) / sizeof(allocations[0]));
    assert(heap_core_alloc(&heap, 16) == NULL);
    for (size_t i = 0; i < count; ++i) assert(heap_core_free(&heap, allocations[i]));
    heap_core_get_stats(&heap, &stats);
    assert(stats.free_bytes == stats.capacity && stats.allocated_blocks == 0);
    return 0;
}
