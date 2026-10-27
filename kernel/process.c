#include <stddef.h>
#include <stdint.h>
#include "process.h"
#include "heap.h"
#include "pmm.h"
#include "vm.h"
#include "syscall_core.h"
#include "elf64_vm.h"
#include "hello_elf_blob.h"
#include "fs_core.h"
#include "../drivers/serial.h"

#define USER_CODE_SELECTOR 0x2Bu
#define USER_DATA_SELECTOR 0x33u
#define USER_BASE UINT64_C(0x0000400000000000)
#define USER_DATA_VA (USER_BASE + VM_PAGE_SIZE)
#define USER_STACK_VA (USER_BASE + 3 * VM_PAGE_SIZE)
#define USER_STACK_TOP (USER_BASE + 4 * VM_PAGE_SIZE)
#define USER_TEST_VALUE UINT64_C(0x4153544552494F4E)
#define USER_TEST_MARKER_OFFSET 128u
#define USER_KERNEL_STACK_SIZE (16u * 1024u)
#define ELF_USER_STACK_VA (USER_BASE + UINT64_C(0x00200000))
#define ELF_USER_STACK_TOP (ELF_USER_STACK_VA + VM_PAGE_SIZE)

extern uint8_t x86_user_test_blob_start[];
extern uint8_t x86_user_test_read_start[];
extern uint8_t x86_user_test_blob_end[];
extern void x86_enter_user(uintptr_t instruction_pointer, uintptr_t stack_pointer,
                           uint64_t rflags);
extern void x86_user_return_resume(void);
extern uintptr_t x86_user_saved_kernel_rsp;

volatile uint64_t asterion_user_supervisor_probe = UINT64_C(0x5345435552454B52);
uintptr_t x86_user_saved_kernel_rsp;

typedef struct {
    uint32_t pid;
    process_state_t state;
    vm_space_t address_space;
    uint64_t code_page, data_page, stack_page;
    void *kernel_stack;
    uint64_t exit_status;
    uint64_t fault_vector, fault_error, fault_rip, fault_cr2;
} process_t;

static process_t test_process;
static process_t *active_process;

static void process_log_hex(serial_level_t level, const char *label, uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char message[72];
    size_t n = 0;
    while (label[n] && n < sizeof(message) - 19) { message[n] = label[n]; ++n; }
    message[n++] = '0'; message[n++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) message[n++] = digits[(value >> shift) & 15u];
    message[n] = 0;
    serial_log(level, message);
}

static uint64_t irq_disable(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint64_t flags) {
    if (flags & (UINT64_C(1) << 9)) __asm__ volatile("sti" ::: "memory");
}

static void process_reset(void) {
    test_process.pid = 1;
    test_process.state = PROCESS_UNUSED;
    test_process.code_page = 0;
    test_process.data_page = 0;
    test_process.stack_page = 0;
    test_process.kernel_stack = 0;
    test_process.exit_status = UINT64_MAX;
    test_process.address_space.root = 0;
    test_process.address_space.root_physical = 0;
    test_process.fault_vector = UINT64_MAX;
    test_process.fault_error = 0;
    test_process.fault_rip = 0;
    test_process.fault_cr2 = 0;
}

static int syscall_user_page_readable(void *context, uint64_t page_address) {
    process_t *process = (process_t *)context;
    uint64_t physical = 0;
    uint32_t flags = 0;
    return process && process == active_process &&
           vm_space_translate(&process->address_space, page_address, &physical, &flags) == VM_OK &&
           (flags & VM_MAP_USER) != 0;
}

static int64_t syscall_write(void *context, uint64_t user_buffer, uint64_t length) {
    process_t *process = (process_t *)context;
    uint8_t levels = vm_active_levels();
    uint8_t bits = levels == 4 ? 48 : (levels == 5 ? 57 : 0);
    if (!process || process != active_process || !bits ||
        !asterion_syscall_user_range_valid(user_buffer, length,
                                           UINT64_C(1) << (bits - 1),
                                           ASTERION_SYSCALL_WRITE_LIMIT,
                                           syscall_user_page_readable, process))
        return -(int64_t)ASTERION_SYS_EFAULT;

    char output[ASTERION_SYSCALL_WRITE_LIMIT];
    const volatile uint8_t *source = (const volatile uint8_t *)(uintptr_t)user_buffer;
    for (uint64_t i = 0; i < length; ++i) {
        uint8_t ch = source[i];
        output[i] = (ch == '\n' || ch == '\r' || ch == '\t' || (ch >= 0x20 && ch <= 0x7E))
                        ? (char)ch : '?';
    }
    serial_write_bytes(output, (unsigned long)length);
    return (int64_t)length;
}

static int64_t syscall_exit(void *context, uint64_t status) {
    process_t *process = (process_t *)context;
    if (!process || process != active_process || process->state != PROCESS_RUNNING)
        return -(int64_t)ASTERION_SYS_EINVAL;
    process->exit_status = status;
    process->state = PROCESS_EXITED;
    return 0;
}

static int64_t syscall_yield(void *context) {
    process_t *process = (process_t *)context;
    /* With one synchronous user process there is no peer to schedule. */
    return process && process == active_process && process->state == PROCESS_RUNNING
               ? 0 : -(int64_t)ASTERION_SYS_EINVAL;
}

static uint64_t syscall_getpid(void *context) {
    process_t *process = (process_t *)context;
    return process && process == active_process ? process->pid : 0;
}

static int return_to_kernel(x86_interrupt_frame_t *frame) {
    if (!frame || !x86_user_saved_kernel_rsp) return 0;
    frame->rip = (uintptr_t)x86_user_return_resume;
    frame->cs = 0x08;
    frame->rflags = 0x02;
    frame->old_rsp = x86_user_saved_kernel_rsp;
    frame->old_ss = 0x10;
    return 1;
}

int process_handle_user_event(x86_interrupt_frame_t *frame, uint64_t vector, uint64_t cr2) {
    if (!frame || !active_process || (frame->cs & 3u) != 3u ||
        (vector >= 32 && vector != 49)) return 0;

    active_process->fault_vector = vector;
    active_process->fault_error = frame->error;
    active_process->fault_rip = frame->rip;
    active_process->fault_cr2 = vector == 14 ? cr2 : 0;
    active_process->state = vector == 49 ? PROCESS_EXITED : PROCESS_FAULTED;

    /* Resume only at the kernel continuation saved by x86_enter_user. */
    return_to_kernel(frame);
    return frame->old_rsp != 0;
}

int process_handle_syscall(x86_interrupt_frame_t *frame) {
    if (!frame || !active_process || active_process->state != PROCESS_RUNNING ||
        (frame->cs & 3u) != 3u || !x86_user_saved_kernel_rsp) return 0;
    const asterion_syscall_ops_t ops = {
        .context = active_process,
        .write = syscall_write,
        .exit = syscall_exit,
        .yield = syscall_yield,
        .getpid = syscall_getpid
    };
    uint64_t number = frame->rax;
    int64_t result = asterion_syscall_dispatch(&ops, number,
                                               frame->rdi, frame->rsi, frame->rdx);
    frame->rax = (uint64_t)result;
    if (number == ASTERION_SYS_EXIT && active_process->state == PROCESS_EXITED)
        return return_to_kernel(frame);
    return 1;
}

static int user_pages_are_unmapped(void) {
    const uint64_t addresses[] = {USER_BASE, USER_DATA_VA, USER_BASE + 2 * VM_PAGE_SIZE,
                                  USER_STACK_VA};
    for (uint32_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        uint64_t physical;
        int result = vm_translate_address(addresses[i], &physical, 0);
        if (result != VM_NOT_MAPPED) return 0;
    }
    return 1;
}

static int process_cleanup(uintptr_t previous_rsp0) {
    active_process = 0;
    int restored = vm_activate_kernel_space();
    (void)x86_set_kernel_stack(previous_rsp0);
    vm_address_space_destroy(&test_process.address_space);
    if (test_process.code_page && !pmm_free(test_process.code_page, 1)) restored = 0;
    if (test_process.data_page && !pmm_free(test_process.data_page, 1)) restored = 0;
    if (test_process.stack_page && !pmm_free(test_process.stack_page, 1)) restored = 0;
    if (test_process.kernel_stack && !kfree(test_process.kernel_stack)) restored = 0;
    test_process.state = PROCESS_UNUSED;
    test_process.code_page = test_process.data_page = test_process.stack_page = 0;
    test_process.kernel_stack = 0;
    return restored;
}


static int process_elf_self_test(const fs_volume_t *volume) {
    int success = 0;
    uintptr_t previous_rsp0 = 0;
    elf64_loaded_image_t *loaded = 0;
    uint64_t stack_page = 0;
    uint8_t *elf_file = 0;
    const uint8_t *elf_bytes = asterion_hello_elf;
    size_t elf_size = asterion_hello_elf_size;
    int stack_mapped = 0;
    test_process.kernel_stack = 0;
    test_process.address_space.root = 0;
    test_process.address_space.root_physical = 0;
    test_process.exit_status = UINT64_MAX;
    test_process.fault_vector = UINT64_MAX;

    test_process.kernel_stack = kmalloc(USER_KERNEL_STACK_SIZE);
    loaded = (elf64_loaded_image_t *)kmalloc(sizeof(*loaded));
    if (!test_process.kernel_stack || !loaded || !vm_address_space_create(&test_process.address_space)) {
        serial_log(SERIAL_ERROR, "ELF self-test setup failed: kernel stack/address-space allocation.");
        goto cleanup;
    }
    test_process.state = PROCESS_CREATED;
    loaded->entry = 0;
    loaded->page_count = 0;
    if (volume && volume->mounted) {
        const fs_file_t *file = fs_lookup(volume, "hello.elf");
        size_t loaded_bytes = 0;
        if (!file || !(elf_file = kmalloc(file->byte_size)) ||
            fs_read_file(volume, "hello.elf", elf_file, file->byte_size, &loaded_bytes) != FS_OK ||
            loaded_bytes != file->byte_size) {
            serial_log(SERIAL_ERROR, "ELF disk self-test failed: hello.elf read/checksum.");
            goto cleanup;
        }
        elf_bytes = elf_file;
        elf_size = loaded_bytes;
    }
    if (elf64_load_user_image(&test_process.address_space, elf_bytes, elf_size, loaded) != ELF64_OK) {
        serial_log(SERIAL_ERROR, "ELF self-test failed: standalone ELF load rejected.");
        goto cleanup;
    }
    uint64_t stack_translation = 0;
    int stack_mapping_status = vm_space_translate(&test_process.address_space,
                                                 ELF_USER_STACK_VA, &stack_translation, 0);
    if (stack_mapping_status != VM_NOT_MAPPED) {
        process_log_hex(SERIAL_ERROR, "ELF stack mapping conflict VA=", ELF_USER_STACK_VA);
        process_log_hex(SERIAL_ERROR, "ELF stack translate rc=",
                        (uint64_t)(int64_t)stack_mapping_status);
        goto cleanup;
    }
    if (!pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, &stack_page)) {
        pmm_stats_t stats;
        pmm_get_stats(&stats);
        serial_log(SERIAL_ERROR, "ELF self-test failed: PMM stack-frame allocation failed.");
        process_log_hex(SERIAL_ERROR, "PMM free frames=", stats.free_pages);
        goto cleanup;
    }
    for (size_t i = 0; i < VM_PAGE_SIZE; ++i) ((uint8_t *)(uintptr_t)stack_page)[i] = 0;
    if (vm_space_map_page(&test_process.address_space, ELF_USER_STACK_VA, stack_page,
                          VM_MAP_USER | VM_MAP_WRITE) != VM_OK) {
        serial_log(SERIAL_ERROR, "ELF self-test failed: user stack mapping.");
        goto cleanup;
    }
    stack_mapped = 1;
    if (!vm_space_audit_user_mappings(&test_process.address_space, USER_BASE,
                                      ELF_USER_STACK_TOP) ||
        !vm_kernel_mapping_is_supervisor((uintptr_t)&asterion_user_supervisor_probe)) {
        serial_log(SERIAL_ERROR, "ELF self-test failed: address-space permission audit.");
        goto cleanup;
    }

    uint64_t physical = 0;
    uint32_t flags = 0;
    if (vm_space_translate(&test_process.address_space, loaded->entry, &physical, &flags) != VM_OK ||
        !(flags & VM_MAP_USER) || !(flags & VM_MAP_EXEC) || (flags & VM_MAP_WRITE) ||
        vm_space_translate(&test_process.address_space, ELF_USER_STACK_VA,
                           &physical, &flags) != VM_OK ||
        !(flags & VM_MAP_USER) || !(flags & VM_MAP_WRITE) || (flags & VM_MAP_EXEC) ||
        vm_space_translate(&test_process.address_space, ELF_USER_STACK_TOP,
                           &physical, 0) != VM_NOT_MAPPED) {
        serial_log(SERIAL_ERROR, "ELF self-test failed: RX/RW-NX permissions or stack guard.");
        goto cleanup;
    }

    x86_user_iret_frame_t user_frame;
    if (!x86_user_iret_frame_init(&user_frame, loaded->entry, ELF_USER_STACK_TOP,
                                  USER_CODE_SELECTOR, USER_DATA_SELECTOR,
                                  vm_active_levels())) {
        serial_log(SERIAL_ERROR, "ELF self-test failed: invalid user entry frame.");
        goto cleanup;
    }
    uintptr_t kernel_stack_top = ((uintptr_t)test_process.kernel_stack + USER_KERNEL_STACK_SIZE) & ~(uintptr_t)15u;
    previous_rsp0 = x86_set_kernel_stack(kernel_stack_top);
    if (!previous_rsp0 || !vm_space_activate(&test_process.address_space)) {
        serial_log(SERIAL_ERROR, "ELF self-test failed: TSS.RSP0 or address-space activation.");
        goto cleanup;
    }
    active_process = &test_process;
    test_process.state = PROCESS_RUNNING;
    x86_enter_user((uintptr_t)user_frame.rip, (uintptr_t)user_frame.rsp, user_frame.rflags);
    if (test_process.state != PROCESS_EXITED || test_process.exit_status != 0) {
        serial_log(SERIAL_ERROR, "ELF self-test failed: user ELF did not exit with status zero.");
        goto cleanup;
    }
    success = 1;
    serial_log(SERIAL_INFO, volume && volume->mounted ? "SELFTEST PASS: disk-sourced ELF64 ran in ring 3; write/getpid/yield/exit verified." : "SELFTEST PASS: embedded ELF64 ran in ring 3; disk ELF unavailable.");

cleanup:
    active_process = 0;
    if (!vm_activate_kernel_space()) {
        serial_log(SERIAL_ERROR, "ELF cleanup failed: kernel CR3 restore failed; retaining user allocations.");
        return 0;
    }
    if (previous_rsp0) (void)x86_set_kernel_stack(previous_rsp0);
    if (loaded && loaded->page_count && elf64_unload_user_image(&test_process.address_space, loaded) != ELF64_OK) {
        serial_log(SERIAL_ERROR, "ELF cleanup failed: image mappings retained after unload failure.");
        return 0;
    }
    if (stack_mapped) {
        if (vm_space_unmap_page(&test_process.address_space, ELF_USER_STACK_VA) != VM_OK) {
            serial_log(SERIAL_ERROR, "ELF cleanup failed: stack mapping retained.");
            return 0;
        }
        stack_mapped = 0;
    }
    if (stack_page && !pmm_free(stack_page, 1)) success = 0;
    if (test_process.address_space.root) vm_address_space_destroy(&test_process.address_space);
    if (loaded && !kfree(loaded)) success = 0;
    if (elf_file && !kfree(elf_file)) success = 0;
    if (test_process.kernel_stack && !kfree(test_process.kernel_stack)) success = 0;
    test_process.kernel_stack = 0;
    test_process.state = PROCESS_UNUSED;
    return success;
}

int process_user_mode_self_test(const fs_volume_t *volume) {
    uint64_t saved_flags = irq_disable();
    uintptr_t previous_rsp0 = 0;
    int success = 0;
    process_reset();

    const size_t blob_size = (size_t)(x86_user_test_blob_end - x86_user_test_blob_start);
    const size_t read_offset = (size_t)(x86_user_test_read_start - x86_user_test_blob_start);
    if (!blob_size || blob_size > VM_PAGE_SIZE || read_offset >= blob_size ||
        !vm_user_address_valid(USER_BASE, 4 * VM_PAGE_SIZE, vm_active_levels()) ||
        !user_pages_are_unmapped() ||
        !vm_kernel_mapping_is_supervisor((uintptr_t)&asterion_user_supervisor_probe)) {
        serial_log(SERIAL_ERROR, "Process self-test setup failed: user range or kernel permissions invalid.");
        goto finish;
    }

    if (!vm_address_space_create(&test_process.address_space)) {
        serial_log(SERIAL_ERROR, "Process self-test failed: could not clone an audited kernel address space.");
        goto finish;
    }
    test_process.state = PROCESS_CREATED;
    test_process.kernel_stack = kmalloc(USER_KERNEL_STACK_SIZE);
    if (!test_process.kernel_stack ||
        !pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, &test_process.code_page) ||
        !pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, &test_process.data_page) ||
        !pmm_alloc(1, 1, PMM_MAX_PHYSICAL_ADDRESS, &test_process.stack_page)) {
        serial_log(SERIAL_ERROR, "Process self-test failed: PMM or kernel stack allocation failed.");
        goto cleanup;
    }

    uint8_t *code = (uint8_t *)(uintptr_t)test_process.code_page;
    uint8_t *data = (uint8_t *)(uintptr_t)test_process.data_page;
    uint8_t *stack = (uint8_t *)(uintptr_t)test_process.stack_page;
    for (size_t i = 0; i < VM_PAGE_SIZE; ++i) code[i] = data[i] = stack[i] = 0;
    for (size_t i = 0; i < blob_size; ++i) code[i] = x86_user_test_blob_start[i];
    static const char hello[] = "Hello from ring 3 through Asterion syscall write.\r\n";
    for (size_t i = 0; i < sizeof(hello) - 1; ++i) data[i] = (uint8_t)hello[i];

    uint64_t physical = 0;
    uint32_t flags = 0;
    if (vm_space_map_page(&test_process.address_space, USER_BASE, test_process.code_page,
                          VM_MAP_USER | VM_MAP_EXEC) != VM_OK ||
        vm_space_map_page(&test_process.address_space, USER_DATA_VA, test_process.data_page,
                          VM_MAP_USER | VM_MAP_WRITE) != VM_OK ||
        vm_space_map_page(&test_process.address_space, USER_STACK_VA, test_process.stack_page,
                          VM_MAP_USER | VM_MAP_WRITE) != VM_OK ||
        !vm_space_audit_user_mappings(&test_process.address_space, USER_BASE, USER_STACK_TOP) ||
        !vm_kernel_mapping_is_supervisor((uintptr_t)&asterion_user_supervisor_probe)) {
        serial_log(SERIAL_ERROR, "Process self-test failed: user/kernel mapping permission audit failed.");
        goto cleanup;
    }

    if (vm_space_translate(&test_process.address_space,
                           (uintptr_t)&asterion_user_supervisor_probe,
                           &physical, &flags) != VM_OK || (flags & VM_MAP_USER)) {
        serial_log(SERIAL_ERROR, "Process self-test failed: supervisor leaf became user-accessible.");
        goto cleanup;
    }

    if (vm_space_translate(&test_process.address_space, USER_BASE, &physical, &flags) != VM_OK ||
        physical != test_process.code_page || !(flags & VM_MAP_USER) || !(flags & VM_MAP_EXEC) ||
        (flags & VM_MAP_WRITE) ||
        vm_space_translate(&test_process.address_space, USER_DATA_VA, &physical, &flags) != VM_OK ||
        physical != test_process.data_page || !(flags & VM_MAP_USER) || !(flags & VM_MAP_WRITE) ||
        (flags & VM_MAP_EXEC) ||
        vm_space_translate(&test_process.address_space, USER_STACK_VA, &physical, &flags) != VM_OK ||
        physical != test_process.stack_page || !(flags & VM_MAP_USER) || !(flags & VM_MAP_WRITE) ||
        (flags & VM_MAP_EXEC) ||
        vm_space_translate(&test_process.address_space, USER_BASE + 2 * VM_PAGE_SIZE,
                           &physical, 0) != VM_NOT_MAPPED) {
        serial_log(SERIAL_ERROR, "Process self-test failed: user mapping translation or guard-page check.");
        goto cleanup;
    }

    x86_user_iret_frame_t user_frame;
    if (!x86_user_iret_frame_init(&user_frame, USER_BASE, USER_STACK_TOP,
                                  USER_CODE_SELECTOR, USER_DATA_SELECTOR,
                                  vm_active_levels())) {
        serial_log(SERIAL_ERROR, "Process self-test failed: could not construct ring-3 IRETQ frame.");
        goto cleanup;
    }
    process_log_hex(SERIAL_INFO, "User IRET CS=", user_frame.cs);
    process_log_hex(SERIAL_INFO, "User IRET SS=", user_frame.ss);

    uintptr_t kernel_stack_top = ((uintptr_t)test_process.kernel_stack + USER_KERNEL_STACK_SIZE) & ~(uintptr_t)15u;
    previous_rsp0 = x86_set_kernel_stack(kernel_stack_top);
    if (!previous_rsp0 || !vm_space_activate(&test_process.address_space)) {
        serial_log(SERIAL_ERROR, "Process self-test failed: TSS.RSP0 or address-space activation.");
        goto cleanup;
    }
    active_process = &test_process;
    test_process.state = PROCESS_RUNNING;

    x86_enter_user((uintptr_t)user_frame.rip, (uintptr_t)user_frame.rsp, user_frame.rflags);
    if (test_process.state != PROCESS_EXITED || test_process.exit_status != 0 ||
        *(volatile uint64_t *)(uintptr_t)(USER_DATA_VA + USER_TEST_MARKER_OFFSET) != USER_TEST_VALUE) {
        serial_log(SERIAL_ERROR, "Process self-test failed: syscall write/getpid/yield/exit or user data.");
        goto cleanup;
    }

    test_process.state = PROCESS_RUNNING;
    test_process.fault_vector = UINT64_MAX;
    x86_user_iret_frame_t read_frame;
    if (!x86_user_iret_frame_init(&read_frame, USER_BASE + read_offset, USER_STACK_TOP,
                                 USER_CODE_SELECTOR, USER_DATA_SELECTOR,
                                 vm_active_levels())) {
        serial_log(SERIAL_ERROR, "Process self-test failed: invalid supervisor-probe entry frame.");
        goto cleanup;
    }
    x86_enter_user((uintptr_t)read_frame.rip, (uintptr_t)read_frame.rsp, read_frame.rflags);
    if (test_process.state != PROCESS_FAULTED || test_process.fault_vector != 14 ||
        (test_process.fault_error & 5u) != 5u ||
        test_process.fault_cr2 != (uintptr_t)&asterion_user_supervisor_probe ||
        *(volatile uint64_t *)(uintptr_t)(USER_DATA_VA + USER_TEST_MARKER_OFFSET) != USER_TEST_VALUE) {
        serial_log(SERIAL_ERROR, "Process self-test failed: supervisor read was not contained as a user #PF.");
        process_log_hex(SERIAL_ERROR, "user fault vector=", test_process.fault_vector);
        process_log_hex(SERIAL_ERROR, "user fault error=", test_process.fault_error);
        process_log_hex(SERIAL_ERROR, "user fault CR2=", test_process.fault_cr2);
        goto cleanup;
    }

    process_log_hex(SERIAL_INFO, "Contained user fault vector=", test_process.fault_vector);
    process_log_hex(SERIAL_INFO, "Contained user fault error=", test_process.fault_error);
    process_log_hex(SERIAL_INFO, "Contained user fault RIP=", test_process.fault_rip);
    process_log_hex(SERIAL_INFO, "Contained user fault CR2=", test_process.fault_cr2);
    success = 1;

cleanup:
    if (test_process.address_space.root || test_process.kernel_stack ||
        test_process.code_page || test_process.data_page || test_process.stack_page) {
        if (!process_cleanup(previous_rsp0)) success = 0;
    }
    if (success && !process_elf_self_test(volume)) success = 0;
finish:
    irq_restore(saved_flags);
    if (success) {
        serial_log(SERIAL_INFO, "SELFTEST PASS: syscall ABI v1 write/getpid/yield/exit, unknown-call and bad-pointer rejection.");
        serial_log(SERIAL_INFO, "SELFTEST PASS: ring-3 writable user memory, supervisor denial, and fault containment.");
    }
    return success;
}
