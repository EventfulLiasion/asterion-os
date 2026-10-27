#ifndef ASTERION_KERNEL_SYNC_CORE_H
#define ASTERION_KERNEL_SYNC_CORE_H
#include <stdint.h>
#define SYNC_MAX_LOCK_DEPTH 8u
enum { SYNC_OK=0, SYNC_RECURSIVE=-1, SYNC_ORDER_VIOLATION=-2, SYNC_DEPTH_EXCEEDED=-3, SYNC_NOT_LIFO=-4 };
typedef struct { uintptr_t locks[SYNC_MAX_LOCK_DEPTH]; uint8_t ranks[SYNC_MAX_LOCK_DEPTH]; uint32_t depth; } sync_tracker_t;
int sync_core_before_lock(const sync_tracker_t *tracker, uintptr_t lock, uint8_t rank);
int sync_core_push(sync_tracker_t *tracker, uintptr_t lock, uint8_t rank);
int sync_core_pop(sync_tracker_t *tracker, uintptr_t lock);
#endif
