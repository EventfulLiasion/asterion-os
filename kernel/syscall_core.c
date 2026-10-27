#include "syscall_core.h"

#define SYSCALL_PAGE_SIZE UINT64_C(4096)

int64_t asterion_syscall_dispatch(const asterion_syscall_ops_t *ops,
                                 uint64_t number, uint64_t arg0,
                                 uint64_t arg1, uint64_t arg2) {
    if (!ops) return -(int64_t)ASTERION_SYS_EINVAL;
    switch (number) {
        case ASTERION_SYS_WRITE:
            if (!arg0 || !arg1) return -(int64_t)ASTERION_SYS_EINVAL;
            if (arg1 > ASTERION_SYSCALL_WRITE_LIMIT) return -(int64_t)ASTERION_SYS_E2BIG;
            return ops->write ? ops->write(ops->context, arg0, arg1)
                              : -(int64_t)ASTERION_SYS_ENOSYS;
        case ASTERION_SYS_EXIT:
            return ops->exit ? ops->exit(ops->context, arg0)
                             : -(int64_t)ASTERION_SYS_ENOSYS;
        case ASTERION_SYS_YIELD:
            return ops->yield ? ops->yield(ops->context)
                              : -(int64_t)ASTERION_SYS_ENOSYS;
        case ASTERION_SYS_GETPID:
            return ops->getpid ? (int64_t)ops->getpid(ops->context)
                               : -(int64_t)ASTERION_SYS_ENOSYS;
        default:
            (void)arg2;
            return -(int64_t)ASTERION_SYS_ENOSYS;
    }
}

int asterion_syscall_user_range_valid(uint64_t address, uint64_t length,
                                     uint64_t user_limit, uint64_t maximum_length,
                                     asterion_user_page_readable_fn page_readable,
                                     void *context) {
    if (!page_readable || !length || length > maximum_length || !user_limit ||
        address >= user_limit || length - 1 > UINT64_MAX - address)
        return 0;
    uint64_t last = address + length - 1;
    if (last >= user_limit) return 0;

    uint64_t page = address & ~(SYSCALL_PAGE_SIZE - 1);
    uint64_t last_page = last & ~(SYSCALL_PAGE_SIZE - 1);
    for (;;) {
        if (!page_readable(context, page)) return 0;
        if (page == last_page) return 1;
        if (page > UINT64_MAX - SYSCALL_PAGE_SIZE) return 0;
        page += SYSCALL_PAGE_SIZE;
    }
}
