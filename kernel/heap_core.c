#include <stdint.h>
#include "heap_core.h"

#define BLOCK_MAGIC UINT32_C(0x48454150)
#define BLOCK_FREE UINT8_C(1)
#define BLOCK_USED UINT8_C(2)
#define MIN_PAYLOAD HEAP_CORE_ALIGNMENT

struct heap_block {
    size_t payload_size;
    heap_block_t *previous;
    heap_block_t *next;
    heap_block_t *free_previous;
    heap_block_t *free_next;
    uint32_t magic;
    uint8_t state;
};

#define BLOCK_HEADER_SIZE ((sizeof(heap_block_t) + HEAP_CORE_ALIGNMENT - 1) & ~(size_t)(HEAP_CORE_ALIGNMENT - 1))

static int align_size(size_t value, size_t *result) {
    size_t extra = HEAP_CORE_ALIGNMENT - 1;
    if (value > SIZE_MAX - extra) return 0;
    *result = (value + extra) & ~(size_t)extra;
    return 1;
}

static void free_remove(heap_core_t *heap, heap_block_t *block) {
    if (block->free_previous) block->free_previous->free_next = block->free_next;
    else heap->free_head = block->free_next;
    if (block->free_next) block->free_next->free_previous = block->free_previous;
    block->free_previous = NULL;
    block->free_next = NULL;
}

static void free_insert(heap_core_t *heap, heap_block_t *block) {
    block->state = BLOCK_FREE;
    block->free_previous = NULL;
    block->free_next = heap->free_head;
    if (heap->free_head) heap->free_head->free_previous = block;
    heap->free_head = block;
}

int heap_core_init(heap_core_t *heap, void *memory, size_t size) {
    if (!heap || !memory || ((uintptr_t)memory & (HEAP_CORE_ALIGNMENT - 1)) ||
        size < BLOCK_HEADER_SIZE + MIN_PAYLOAD || (uintptr_t)memory > UINTPTR_MAX - size)
        return 0;
    size &= ~(size_t)(HEAP_CORE_ALIGNMENT - 1);
    if (size < BLOCK_HEADER_SIZE + MIN_PAYLOAD) return 0;

    heap->base = (uint8_t *)memory;
    heap->size = size;
    heap->free_bytes = size - BLOCK_HEADER_SIZE;
    heap->allocated_blocks = 0;
    heap->free_head = (heap_block_t *)memory;
    heap->initialized = 1;

    heap_block_t *first = heap->free_head;
    first->payload_size = heap->free_bytes;
    first->previous = NULL;
    first->next = NULL;
    first->free_previous = NULL;
    first->free_next = NULL;
    first->magic = BLOCK_MAGIC;
    first->state = BLOCK_FREE;
    return 1;
}

void *heap_core_alloc(heap_core_t *heap, size_t size) {
    if (!heap || !heap->initialized || !size) return NULL;
    size_t wanted;
    if (!align_size(size, &wanted)) return NULL;

    heap_block_t *block = heap->free_head;
    while (block && (block->magic != BLOCK_MAGIC || block->state != BLOCK_FREE ||
                     block->payload_size < wanted)) block = block->free_next;
    if (!block) return NULL;

    size_t remainder = block->payload_size - wanted;
    if (remainder >= BLOCK_HEADER_SIZE + MIN_PAYLOAD) {
        heap_block_t *split = (heap_block_t *)((uint8_t *)block + BLOCK_HEADER_SIZE + wanted);
        split->payload_size = remainder - BLOCK_HEADER_SIZE;
        split->previous = block;
        split->next = block->next;
        if (split->next) split->next->previous = split;
        split->free_previous = NULL;
        split->free_next = NULL;
        split->magic = BLOCK_MAGIC;
        split->state = BLOCK_FREE;
        block->next = split;
        block->payload_size = wanted;
        free_remove(heap, block);
        free_insert(heap, split);
        heap->free_bytes -= wanted + BLOCK_HEADER_SIZE;
    } else {
        free_remove(heap, block);
        heap->free_bytes -= block->payload_size;
    }
    block->state = BLOCK_USED;
    block->free_previous = NULL;
    block->free_next = NULL;
    ++heap->allocated_blocks;
    return (uint8_t *)block + BLOCK_HEADER_SIZE;
}

static int valid_block_chain(const heap_core_t *heap, heap_block_t **found,
                             const void *pointer) {
    uintptr_t base = (uintptr_t)heap->base;
    uintptr_t end = base + heap->size;
    uintptr_t target = (uintptr_t)pointer;
    if (target < base + BLOCK_HEADER_SIZE || target >= end ||
        (target & (HEAP_CORE_ALIGNMENT - 1))) return 0;

    heap_block_t *block = (heap_block_t *)heap->base;
    size_t traversed = 0;
    while (block && traversed < heap->size) {
        uintptr_t at = (uintptr_t)block;
        if (at < base || at > end - BLOCK_HEADER_SIZE || block->magic != BLOCK_MAGIC ||
            block->payload_size > end - at - BLOCK_HEADER_SIZE ||
            (block->payload_size & (HEAP_CORE_ALIGNMENT - 1))) return 0;
        if (at + BLOCK_HEADER_SIZE == target) {
            *found = block;
            return 1;
        }
        size_t step = BLOCK_HEADER_SIZE + block->payload_size;
        if (!step || step > heap->size - traversed) return 0;
        traversed += step;
        block = block->next;
    }
    return 0;
}

int heap_core_free(heap_core_t *heap, void *pointer) {
    if (!heap || !heap->initialized || !pointer) return 0;
    heap_block_t *block = NULL;
    if (!valid_block_chain(heap, &block, pointer) || block->state != BLOCK_USED) return 0;

    block->state = BLOCK_FREE;
    heap->free_bytes += block->payload_size;
    --heap->allocated_blocks;

    if (block->previous && block->previous->state == BLOCK_FREE) {
        heap_block_t *previous = block->previous;
        free_remove(heap, previous);
        previous->payload_size += BLOCK_HEADER_SIZE + block->payload_size;
        previous->next = block->next;
        if (block->next) block->next->previous = previous;
        heap->free_bytes += BLOCK_HEADER_SIZE;
        block = previous;
    }
    if (block->next && block->next->state == BLOCK_FREE) {
        heap_block_t *next = block->next;
        free_remove(heap, next);
        block->payload_size += BLOCK_HEADER_SIZE + next->payload_size;
        block->next = next->next;
        if (next->next) next->next->previous = block;
        heap->free_bytes += BLOCK_HEADER_SIZE;
    }
    free_insert(heap, block);
    return 1;
}

void heap_core_get_stats(const heap_core_t *heap, heap_core_stats_t *stats) {
    if (!heap || !stats) return;
    stats->capacity = heap->initialized ? heap->size - BLOCK_HEADER_SIZE : 0;
    stats->free_bytes = heap->initialized ? heap->free_bytes : 0;
    stats->used_bytes = stats->capacity - stats->free_bytes;
    stats->allocated_blocks = heap->initialized ? heap->allocated_blocks : 0;
}
