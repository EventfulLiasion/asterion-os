#include <stddef.h>
#include "sched_core.h"

int sched_queue_init(sched_queue_t *queue, sched_slot_t *slots, uint32_t capacity) {
    if (!queue || !slots || !capacity) return 0;
    queue->slots = slots;
    queue->capacity = capacity;
    queue->ready_head = -1;
    queue->ready_tail = -1;
    queue->ready_count = 0;
    for (uint32_t i = 0; i < capacity; ++i) {
        slots[i].id = 0;
        slots[i].state = SCHED_UNUSED;
        slots[i].next_ready = -1;
    }
    return 1;
}

int sched_queue_push(sched_queue_t *queue, uint32_t index) {
    if (!queue || index >= queue->capacity || queue->ready_count >= queue->capacity ||
        queue->slots[index].state != SCHED_READY || queue->slots[index].next_ready != -1)
        return 0;
    for (int32_t i = queue->ready_head; i >= 0; i = queue->slots[i].next_ready)
        if ((uint32_t)i == index) return 0;
    if (queue->ready_tail >= 0) queue->slots[queue->ready_tail].next_ready = (int32_t)index;
    else queue->ready_head = (int32_t)index;
    queue->ready_tail = (int32_t)index;
    ++queue->ready_count;
    return 1;
}

int sched_queue_pop(sched_queue_t *queue, uint32_t *index) {
    if (!queue || !index || queue->ready_head < 0 || !queue->ready_count) return 0;
    uint32_t result = (uint32_t)queue->ready_head;
    queue->ready_head = queue->slots[result].next_ready;
    queue->slots[result].next_ready = -1;
    --queue->ready_count;
    if (queue->ready_head < 0) queue->ready_tail = -1;
    if (queue->slots[result].state != SCHED_READY) return 0;
    *index = result;
    return 1;
}

int sched_transition(sched_queue_t *queue, uint32_t index,
                     sched_state_t expected, sched_state_t next) {
    if (!queue || index >= queue->capacity || queue->slots[index].state != expected ||
        expected == SCHED_UNUSED || expected == SCHED_TERMINATED || next == SCHED_UNUSED)
        return 0;
    queue->slots[index].state = next;
    return 1;
}
