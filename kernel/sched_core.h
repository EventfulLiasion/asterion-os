#ifndef ASTERION_SCHED_CORE_H
#define ASTERION_SCHED_CORE_H

#include <stdint.h>

typedef enum {
    SCHED_UNUSED = 0,
    SCHED_READY,
    SCHED_RUNNING,
    SCHED_BLOCKED,
    SCHED_TERMINATED
} sched_state_t;

typedef struct {
    uint32_t id;
    sched_state_t state;
    int32_t next_ready;
} sched_slot_t;

typedef struct {
    sched_slot_t *slots;
    uint32_t capacity;
    int32_t ready_head;
    int32_t ready_tail;
    uint32_t ready_count;
} sched_queue_t;

int sched_queue_init(sched_queue_t *queue, sched_slot_t *slots, uint32_t capacity);
int sched_queue_push(sched_queue_t *queue, uint32_t slot_index);
int sched_queue_pop(sched_queue_t *queue, uint32_t *slot_index);
int sched_transition(sched_queue_t *queue, uint32_t slot_index,
                     sched_state_t expected, sched_state_t next);

#endif
