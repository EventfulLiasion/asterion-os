#ifndef ASTERION_KERNEL_PROCESS_H
#define ASTERION_KERNEL_PROCESS_H

#include <stdint.h>
#include "../arch/x86_64/interrupts.h"
#include "fs_core.h"

typedef enum {
    PROCESS_UNUSED = 0,
    PROCESS_CREATED,
    PROCESS_RUNNING,
    PROCESS_EXITED,
    PROCESS_FAULTED
} process_state_t;

typedef struct {
    uint32_t pid;
    process_state_t state;
    uint64_t vector;
    uint64_t error;
    uint64_t rip;
    uint64_t cr2;
} process_fault_info_t;

/* This is a single-process architecture self-test, not a scheduler interface. */
int process_user_mode_self_test(const fs_volume_t *volume);
int process_handle_user_event(x86_interrupt_frame_t *frame, uint64_t vector,
                              uint64_t cr2);
int process_handle_syscall(x86_interrupt_frame_t *frame);

#endif
