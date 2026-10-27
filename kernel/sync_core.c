#include "sync_core.h"
int sync_core_before_lock(const sync_tracker_t *t,uintptr_t lock,uint8_t rank) {
    if(!t||!lock||!rank) return SYNC_ORDER_VIOLATION;
    if(t->depth>=SYNC_MAX_LOCK_DEPTH) return SYNC_DEPTH_EXCEEDED;
    for(uint32_t i=0;i<t->depth;++i) if(t->locks[i]==lock) return SYNC_RECURSIVE;
    if(t->depth&&rank<=t->ranks[t->depth-1]) return SYNC_ORDER_VIOLATION;
    return SYNC_OK;
}
int sync_core_push(sync_tracker_t *t,uintptr_t lock,uint8_t rank) {
    int status=sync_core_before_lock(t,lock,rank); if(status!=SYNC_OK) return status;
    t->locks[t->depth]=lock; t->ranks[t->depth]=rank; ++t->depth; return SYNC_OK;
}
int sync_core_pop(sync_tracker_t *t,uintptr_t lock) {
    if(!t||!t->depth||t->locks[t->depth-1]!=lock) return SYNC_NOT_LIFO;
    --t->depth; t->locks[t->depth]=0; t->ranks[t->depth]=0; return SYNC_OK;
}
