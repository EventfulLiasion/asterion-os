#ifndef ASTERION_KERNEL_SYNC_H
#define ASTERION_KERNEL_SYNC_H
#include <stdint.h>
#include "sync_core.h"
typedef struct { volatile uint32_t held; uint8_t rank; } sync_lock_t;
typedef struct { uint64_t flags; uint8_t acquired; } sync_guard_t;
#define SYNC_LOCK_INIT(r) { 0u, (r) }
int sync_lock_acquire(sync_lock_t *lock, sync_guard_t *guard);
void sync_lock_release(sync_lock_t *lock, sync_guard_t *guard);
#endif
