#ifndef ASTERION_KERNEL_SYSCALL_CORE_H
#define ASTERION_KERNEL_SYSCALL_CORE_H

#include <stdint.h>

#define ASTERION_SYSCALL_ABI_VERSION 1u
#define ASTERION_SYSCALL_VECTOR 0x32u
#define ASTERION_SYSCALL_WRITE_LIMIT 256u

typedef enum {
    ASTERION_SYS_WRITE = 1,
    ASTERION_SYS_EXIT = 2,
    ASTERION_SYS_YIELD = 3,
    ASTERION_SYS_GETPID = 4
} asterion_syscall_number_t;

enum {
    ASTERION_SYS_EINVAL = 22,
    ASTERION_SYS_EFAULT = 14,
    ASTERION_SYS_ENOSYS = 38,
    ASTERION_SYS_E2BIG = 7
};

typedef int64_t (*asterion_syscall_write_fn)(void *context, uint64_t user_buffer,
                                             uint64_t length);
typedef int64_t (*asterion_syscall_exit_fn)(void *context, uint64_t status);
typedef int64_t (*asterion_syscall_yield_fn)(void *context);
typedef uint64_t (*asterion_syscall_getpid_fn)(void *context);

typedef struct {
    void *context;
    asterion_syscall_write_fn write;
    asterion_syscall_exit_fn exit;
    asterion_syscall_yield_fn yield;
    asterion_syscall_getpid_fn getpid;
} asterion_syscall_ops_t;

typedef int (*asterion_user_page_readable_fn)(void *context, uint64_t page_address);

/* ABI arguments are supplied in order: arg0, arg1, arg2. Results are signed in RAX. */
int64_t asterion_syscall_dispatch(const asterion_syscall_ops_t *ops,
                                 uint64_t number, uint64_t arg0,
                                 uint64_t arg1, uint64_t arg2);

/* Validate every page touched by [address, address + length), with checked bounds. */
int asterion_syscall_user_range_valid(uint64_t address, uint64_t length,
                                     uint64_t user_limit, uint64_t maximum_length,
                                     asterion_user_page_readable_fn page_readable,
                                     void *context);

#endif
