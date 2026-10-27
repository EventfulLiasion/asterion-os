#include <assert.h>
#include <stdint.h>
#include "../kernel/syscall_core.h"

typedef struct {
    uint64_t pid;
    uint64_t exit_status;
    uint64_t write_address;
    uint64_t write_length;
    uint32_t writes, yields, exits;
    int reject_write;
} test_context_t;

static int64_t test_write(void *opaque, uint64_t address, uint64_t length) {
    test_context_t *ctx = opaque;
    ++ctx->writes;
    ctx->write_address = address;
    ctx->write_length = length;
    return ctx->reject_write ? -(int64_t)ASTERION_SYS_EFAULT : (int64_t)length;
}

static int64_t test_exit(void *opaque, uint64_t status) {
    test_context_t *ctx = opaque;
    ++ctx->exits;
    ctx->exit_status = status;
    return 0;
}

static int64_t test_yield(void *opaque) {
    test_context_t *ctx = opaque;
    ++ctx->yields;
    return 0;
}

static uint64_t test_getpid(void *opaque) {
    return ((test_context_t *)opaque)->pid;
}

static int page_readable(void *opaque, uint64_t page) {
    const uint64_t *allowed = opaque;
    return page == allowed[0] || page == allowed[1];
}

int main(void) {
    test_context_t ctx = {.pid = 17};
    asterion_syscall_ops_t ops = {
        .context = &ctx, .write = test_write, .exit = test_exit,
        .yield = test_yield, .getpid = test_getpid
    };
    assert(ASTERION_SYSCALL_ABI_VERSION == 1);
    assert(asterion_syscall_dispatch(&ops, ASTERION_SYS_WRITE, 0x401000, 9, 0) == 9);
    assert(ctx.writes == 1 && ctx.write_address == 0x401000 && ctx.write_length == 9);
    assert(asterion_syscall_dispatch(&ops, ASTERION_SYS_WRITE, 0, 1, 0) == -ASTERION_SYS_EINVAL);
    assert(asterion_syscall_dispatch(&ops, ASTERION_SYS_WRITE, 0x401000,
                                     ASTERION_SYSCALL_WRITE_LIMIT + 1, 0) == -ASTERION_SYS_E2BIG);
    ctx.reject_write = 1;
    assert(asterion_syscall_dispatch(&ops, ASTERION_SYS_WRITE, 0xFFFF800000000000, 1, 0) == -ASTERION_SYS_EFAULT);
    assert(asterion_syscall_dispatch(&ops, ASTERION_SYS_EXIT, 23, 0, 0) == 0);
    assert(ctx.exits == 1 && ctx.exit_status == 23);
    assert(asterion_syscall_dispatch(&ops, ASTERION_SYS_YIELD, 0, 0, 0) == 0);
    assert(ctx.yields == 1);
    assert(asterion_syscall_dispatch(&ops, ASTERION_SYS_GETPID, 0, 0, 0) == 17);
    assert(asterion_syscall_dispatch(&ops, UINT64_MAX, 0, 0, 0) == -ASTERION_SYS_ENOSYS);
    assert(asterion_syscall_dispatch(NULL, ASTERION_SYS_GETPID, 0, 0, 0) == -ASTERION_SYS_EINVAL);

    const uint64_t allowed[] = {0x400000, 0x401000};
    assert(asterion_syscall_user_range_valid(0x400FF0, 32, 0x800000000000,
                                             256, page_readable, (void *)allowed));
    assert(!asterion_syscall_user_range_valid(0x401FF0, 32, 0x800000000000,
                                              256, page_readable, (void *)allowed));
    assert(!asterion_syscall_user_range_valid(0x7FFFFFFFFFF0, 32, 0x800000000000,
                                              256, page_readable, (void *)allowed));
    assert(!asterion_syscall_user_range_valid(UINT64_MAX - 3, 8, UINT64_MAX,
                                              256, page_readable, (void *)allowed));
    assert(!asterion_syscall_user_range_valid(0x400000, 257, 0x800000000000,
                                              256, page_readable, (void *)allowed));
    assert(!asterion_syscall_user_range_valid(0x400000, 1, 0x800000000000, 256, NULL, NULL));
    return 0;
}
