#ifndef ASTERION_KERNEL_HEAP_CORE_H
#define ASTERION_KERNEL_HEAP_CORE_H

#include <stddef.h>
#include <stdint.h>

#define HEAP_CORE_ALIGNMENT 16u

typedef struct heap_block heap_block_t;

typedef struct {
    uint8_t *base;
    size_t size;
    size_t free_bytes;
    size_t allocated_blocks;
    heap_block_t *free_head;
    uint32_t initialized;
} heap_core_t;

typedef struct {
    size_t capacity;
    size_t free_bytes;
    size_t used_bytes;
    size_t allocated_blocks;
} heap_core_stats_t;

int heap_core_init(heap_core_t *heap, void *memory, size_t size);
void *heap_core_alloc(heap_core_t *heap, size_t size);
int heap_core_free(heap_core_t *heap, void *pointer);
void heap_core_get_stats(const heap_core_t *heap, heap_core_stats_t *stats);

#endif
