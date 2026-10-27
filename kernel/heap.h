#ifndef ASTERION_KERNEL_HEAP_H
#define ASTERION_KERNEL_HEAP_H

#include <stddef.h>
#include <stdint.h>

#define KHEAP_BASE UINT64_C(0x0000500000000000)
#define KHEAP_PAGE_COUNT 64u
#define KHEAP_CAPACITY (KHEAP_PAGE_COUNT * 4096u)

typedef struct {
    uint64_t base;
    size_t capacity;
    size_t free_bytes;
    size_t used_bytes;
    size_t allocated_blocks;
} kheap_stats_t;

int kheap_init(void);
void *kmalloc(size_t size);
int kfree(void *pointer);
void kheap_get_stats(kheap_stats_t *stats);
int kheap_self_test(void);

#endif
