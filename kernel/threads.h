#ifndef ASTERION_KERNEL_THREADS_H
#define ASTERION_KERNEL_THREADS_H

#include <stdint.h>
#include "sched_core.h"

#define KTHREAD_MAX 16u
#define KTHREAD_STACK_SIZE 16384u

typedef void (*kthread_entry_t)(void *argument);
typedef struct {
    uint32_t id;
    sched_state_t state;
    uint64_t switches;
} kthread_info_t;

int threads_init(void);
uint32_t thread_create(kthread_entry_t entry, void *argument);
void thread_yield(void);
void thread_exit(void) __attribute__((noreturn));
void thread_reap_terminated(void);
uint32_t thread_list(kthread_info_t *output, uint32_t capacity);
uint32_t thread_preemption_count(void);
uint32_t thread_voluntary_switch_count(void);
uint64_t thread_switch_count(void);
int thread_scheduler_self_test(void);
void *thread_schedule_interrupt(void *interrupt_frame, uint64_t vector);
void thread_start_current(void);

#endif
