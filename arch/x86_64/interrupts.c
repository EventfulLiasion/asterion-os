#include <stdint.h>
#include <stddef.h>
#include "interrupts.h"
#include "../../drivers/serial.h"
#include "../../kernel/threads.h"
#include "../../kernel/process.h"
#include "../../kernel/syscall_core.h"

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3, iomap_base;
} x86_tss_t;

_Static_assert(sizeof(x86_tss_t) == 104, "x86-64 TSS layout must be 104 bytes");

static uint64_t gdt[7] __attribute__((aligned(16)));
static x86_tss_t tss __attribute__((aligned(16)));
static x86_idt_entry_t idt[256] __attribute__((aligned(16)));
static x86_descriptor_pointer_t active_idtr;
static uint8_t double_fault_stack[16384] __attribute__((aligned(16)));
static uint8_t nmi_stack[16384] __attribute__((aligned(16)));
static uint8_t page_fault_stack[16384] __attribute__((aligned(16)));
static uint8_t privilege_stack[16384] __attribute__((aligned(16)));
static volatile uint64_t timer_ticks;
static volatile uint64_t expected_vector;
static volatile uint64_t observed_vector;
static volatile uint64_t observed_error;
static volatile uint64_t observed_cr2;
static uintptr_t expected_resume;
static uint8_t master_pic_mask;
static uint8_t slave_pic_mask;

extern void x86_load_gdt(const x86_descriptor_pointer_t *gdtr);
extern void x86_test_ud(uintptr_t unused), x86_test_gp(uintptr_t unused), x86_test_pf(uintptr_t address);
extern void x86_test_ud_resume(void), x86_test_gp_resume(void), x86_test_pf_resume(void);
extern int x86_try_read64(uintptr_t address, uint64_t *value);
extern void x86_try_read64_resume(void);
extern void x86_test_execute(uintptr_t address), x86_test_execute_resume(void);
extern void x86_apic_spurious_stub(void);
extern void x86_yield_stub(void);
extern void x86_user_exit_interrupt_stub(void);
extern void x86_syscall_interrupt_stub(void);
extern void x86_syscall_interrupt_stub(void);

#define DECLARE_ISR(n) extern void x86_isr_##n(void)
DECLARE_ISR(0); DECLARE_ISR(1); DECLARE_ISR(2); DECLARE_ISR(3);
DECLARE_ISR(4); DECLARE_ISR(5); DECLARE_ISR(6); DECLARE_ISR(7);
DECLARE_ISR(8); DECLARE_ISR(9); DECLARE_ISR(10); DECLARE_ISR(11);
DECLARE_ISR(12); DECLARE_ISR(13); DECLARE_ISR(14); DECLARE_ISR(15);
DECLARE_ISR(16); DECLARE_ISR(17); DECLARE_ISR(18); DECLARE_ISR(19);
DECLARE_ISR(20); DECLARE_ISR(21); DECLARE_ISR(22); DECLARE_ISR(23);
DECLARE_ISR(24); DECLARE_ISR(25); DECLARE_ISR(26); DECLARE_ISR(27);
DECLARE_ISR(28); DECLARE_ISR(29); DECLARE_ISR(30); DECLARE_ISR(31);
#undef DECLARE_ISR

#define DECLARE_IRQ(n) extern void x86_irq_##n(void)
DECLARE_IRQ(0); DECLARE_IRQ(1); DECLARE_IRQ(2); DECLARE_IRQ(3);
DECLARE_IRQ(4); DECLARE_IRQ(5); DECLARE_IRQ(6); DECLARE_IRQ(7);
DECLARE_IRQ(8); DECLARE_IRQ(9); DECLARE_IRQ(10); DECLARE_IRQ(11);
DECLARE_IRQ(12); DECLARE_IRQ(13); DECLARE_IRQ(14); DECLARE_IRQ(15);
#undef DECLARE_IRQ

static inline void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1" :: "a"(value), "Nd"(port));
}

static inline uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint64_t read_cr2(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr2,%0" : "=r"(value));
    return value;
}

static void set_gate_dpl(uint32_t vector, void (*handler)(void), uint8_t ist, uint8_t dpl) {
    x86_idt_entry_set(&idt[vector], (uintptr_t)handler, ist,
                      (uint8_t)(0x8E | ((dpl & 3u) << 5)));
}

static void set_gate(uint32_t vector, void (*handler)(void), uint8_t ist) {
    set_gate_dpl(vector, handler, ist, 0);
}

uintptr_t x86_set_kernel_stack(uintptr_t stack_top) {
    uintptr_t previous = (uintptr_t)tss.rsp0;
    if (stack_top && !(stack_top & 15u)) tss.rsp0 = stack_top;
    return previous;
}

static void install_gdt(void) {
    const uint64_t tss_base = (uint64_t)(uintptr_t)&tss;
    const uint32_t tss_limit = (uint32_t)sizeof(tss) - 1;
    x86_descriptor_pointer_t gdtr;

    gdt[0] = 0;
    gdt[1] = UINT64_C(0x00AF9A000000FFFF); /* ring-0, present, long-mode code */
    gdt[2] = UINT64_C(0x00CF92000000FFFF); /* ring-0, present data */
    x86_tss_descriptor_set(&gdt[3], (uintptr_t)tss_base, tss_limit);
    gdt[5] = UINT64_C(0x00AFFA000000FFFF); /* ring-3, present, long-mode code (0x2B) */
    gdt[6] = UINT64_C(0x00CFF2000000FFFF); /* ring-3, present data (0x33) */

    tss.rsp0 = (uintptr_t)(privilege_stack + sizeof(privilege_stack));
    tss.ist1 = (uintptr_t)(double_fault_stack + sizeof(double_fault_stack));
    tss.ist2 = (uintptr_t)(page_fault_stack + sizeof(page_fault_stack));
    tss.ist3 = (uintptr_t)(nmi_stack + sizeof(nmi_stack));
    tss.iomap_base = sizeof(tss);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (uintptr_t)gdt;
    x86_load_gdt(&gdtr);
    const uint16_t tss_selector = 0x18;
    __asm__ volatile("ltr %0" :: "r"(tss_selector) : "memory");
}

static void install_idt(void) {
    void (*const exceptions[32])(void) = {
        x86_isr_0, x86_isr_1, x86_isr_2, x86_isr_3,
        x86_isr_4, x86_isr_5, x86_isr_6, x86_isr_7,
        x86_isr_8, x86_isr_9, x86_isr_10, x86_isr_11,
        x86_isr_12, x86_isr_13, x86_isr_14, x86_isr_15,
        x86_isr_16, x86_isr_17, x86_isr_18, x86_isr_19,
        x86_isr_20, x86_isr_21, x86_isr_22, x86_isr_23,
        x86_isr_24, x86_isr_25, x86_isr_26, x86_isr_27,
        x86_isr_28, x86_isr_29, x86_isr_30, x86_isr_31
    };
    void (*const irqs[16])(void) = {
        x86_irq_0, x86_irq_1, x86_irq_2, x86_irq_3,
        x86_irq_4, x86_irq_5, x86_irq_6, x86_irq_7,
        x86_irq_8, x86_irq_9, x86_irq_10, x86_irq_11,
        x86_irq_12, x86_irq_13, x86_irq_14, x86_irq_15
    };
    for (uint32_t i = 0; i < 256; ++i) {
        idt[i] = (x86_idt_entry_t){0};
    }
    for (uint32_t i = 0; i < 32; ++i) {
        uint8_t ist = i == 8 ? 1 : (i == 14 ? 2 : (i == 2 ? 3 : 0));
        set_gate(i, exceptions[i], ist);
    }
    for (uint32_t i = 0; i < 16; ++i) set_gate(32 + i, irqs[i], 0);
    set_gate(255, x86_apic_spurious_stub, 0);
    set_gate(48, x86_yield_stub, 0);
    set_gate_dpl(49, x86_user_exit_interrupt_stub, 0, 3);
    set_gate_dpl(ASTERION_SYSCALL_VECTOR, x86_syscall_interrupt_stub, 0, 3);
    set_gate_dpl(ASTERION_SYSCALL_VECTOR, x86_syscall_interrupt_stub, 0, 3);

    active_idtr = (x86_descriptor_pointer_t){
        .limit = sizeof(idt) - 1,
        .base = (uintptr_t)idt
    };
    __asm__ volatile("lidt %0" :: "m"(active_idtr) : "memory");
}

static void log_hex(serial_level_t level, const char *label, uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char message[64];
    size_t n = 0;
    while (label[n] && n < sizeof(message) - 19) { message[n] = label[n]; ++n; }
    message[n++] = '0'; message[n++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) message[n++] = digits[(value >> shift) & 0xF];
    message[n] = 0;
    serial_log(level, message);
}

static void fatal_exception(const x86_interrupt_frame_t *frame) {
    serial_log(SERIAL_PANIC, "UNHANDLED CPU EXCEPTION");
    log_hex(SERIAL_PANIC, "vector=", frame->vector);
    log_hex(SERIAL_PANIC, "error=", frame->error);
    log_hex(SERIAL_PANIC, "saved RIP=", frame->rip);
    log_hex(SERIAL_PANIC, "saved CS=", frame->cs);
    log_hex(SERIAL_PANIC, "saved RFLAGS=", frame->rflags);
    log_hex(SERIAL_PANIC, "interrupt frame address=", (uintptr_t)frame);
    log_hex(SERIAL_PANIC, "saved RSP=", frame->old_rsp);
    log_hex(SERIAL_PANIC, "saved SS=", frame->old_ss);
    log_hex(SERIAL_PANIC, "raw frame vector/error=", frame->vector);
    log_hex(SERIAL_PANIC, "raw frame error=", frame->error);
    log_hex(SERIAL_PANIC, "raw frame r15=", frame->r15);
    log_hex(SERIAL_PANIC, "raw frame r14=", frame->r14);
    log_hex(SERIAL_PANIC, "raw frame r13=", frame->r13);
    log_hex(SERIAL_PANIC, "raw frame r12=", frame->r12);
    log_hex(SERIAL_PANIC, "raw frame r11=", frame->r11);
    log_hex(SERIAL_PANIC, "raw frame r10=", frame->r10);
    log_hex(SERIAL_PANIC, "raw frame r9=", frame->r9);
    log_hex(SERIAL_PANIC, "raw frame r8=", frame->r8);
    log_hex(SERIAL_PANIC, "raw frame rdi=", frame->rdi);
    log_hex(SERIAL_PANIC, "raw frame rsi=", frame->rsi);
    log_hex(SERIAL_PANIC, "raw frame rbp=", frame->rbp);
    log_hex(SERIAL_PANIC, "raw frame rdx=", frame->rdx);
    log_hex(SERIAL_PANIC, "raw frame rcx=", frame->rcx);
    log_hex(SERIAL_PANIC, "raw frame rbx=", frame->rbx);
    log_hex(SERIAL_PANIC, "raw frame rax=", frame->rax);
    log_hex(SERIAL_PANIC, "raw frame RIP=", frame->rip);
    log_hex(SERIAL_PANIC, "raw frame CS=", frame->cs);
    log_hex(SERIAL_PANIC, "raw frame RFLAGS=", frame->rflags);
    log_hex(SERIAL_PANIC, "raw frame RSP=", frame->old_rsp);
    log_hex(SERIAL_PANIC, "raw frame SS=", frame->old_ss);
    if (frame->vector == 13) {
        log_hex(SERIAL_PANIC, "#GP selector index=", frame->error >> 3);
        log_hex(SERIAL_PANIC, "#GP IDT/TI/EXT bits=", frame->error & 7u);
    }
    if (frame->vector == 14) log_hex(SERIAL_PANIC, "CR2=", read_cr2());
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

void *x86_interrupt_dispatch(x86_interrupt_frame_t *frame) {
    if (frame->vector < 32) {
        if ((frame->cs & 3u) == 3u &&
            process_handle_user_event(frame, frame->vector,
                                      frame->vector == 14 ? read_cr2() : 0))
            return frame;
        if (expected_vector == frame->vector && expected_resume) {
            observed_vector = frame->vector;
            observed_error = frame->error;
            observed_cr2 = frame->vector == 14 ? read_cr2() : 0;
            frame->rip = expected_resume;
            expected_vector = 0;
            expected_resume = 0;
            return frame;
        }
        fatal_exception(frame);
    }

    if (frame->vector >= 32 && frame->vector < 48) {
        uint32_t irq = (uint32_t)frame->vector - 32;
        if (irq == 0) {
            ++timer_ticks;
            out8(0x20, 0x20);
            if ((frame->cs & 3u) == 3u) return frame;
            return thread_schedule_interrupt(frame, frame->vector);
        }
        if (irq < 8) {
            master_pic_mask |= (uint8_t)(1u << irq);
            out8(0x21, master_pic_mask);
            out8(0x20, 0x20);
        } else {
            uint32_t line = irq - 8;
            slave_pic_mask |= (uint8_t)(1u << line);
            out8(0xA1, slave_pic_mask);
            out8(0xA0, 0x20);
            out8(0x20, 0x20);
        }
        serial_log(SERIAL_WARN, "Unexpected PIC IRQ masked after delivery.");
        return frame;
    }
    if (frame->vector == 49 && (frame->cs & 3u) == 3u &&
        process_handle_user_event(frame, frame->vector, 0)) return frame;
    if (frame->vector == ASTERION_SYSCALL_VECTOR && (frame->cs & 3u) == 3u &&
        process_handle_syscall(frame)) return frame;
    if (frame->vector == ASTERION_SYSCALL_VECTOR && (frame->cs & 3u) == 3u &&
        process_handle_syscall(frame)) return frame;
    if (frame->vector == 48) return thread_schedule_interrupt(frame, frame->vector);
    if (frame->vector == 255) return frame; /* Local APIC spurious vector requires no EOI. */

    serial_log(SERIAL_PANIC, "Unexpected interrupt vector outside configured PIC range.");
    log_hex(SERIAL_PANIC, "vector=", frame->vector);
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

static int run_exception_test(uint64_t vector, uintptr_t resume,
                              void (*test)(uintptr_t), uintptr_t argument) {
    observed_vector = UINT64_MAX;
    observed_error = 0;
    observed_cr2 = 0;
    expected_resume = resume;
    expected_vector = vector;
    if (vector == 13) {
        active_idtr.limit = 0x07FF; /* Make INT 0x80 exceed the IDT limit and raise #GP. */
        __asm__ volatile("lidt %0" :: "m"(active_idtr) : "memory");
    }
    test(argument);
    if (vector == 13) {
        active_idtr.limit = sizeof(idt) - 1;
        __asm__ volatile("lidt %0" :: "m"(active_idtr) : "memory");
    }
    expected_vector = 0;
    expected_resume = 0;
    return observed_vector == vector;
}

int x86_probe_read64(uintptr_t address, uint64_t *value) {
    if (!value) return 0;
    observed_vector = UINT64_MAX;
    expected_resume = (uintptr_t)x86_try_read64_resume;
    expected_vector = 14;
    int read_ok = x86_try_read64(address, value);
    expected_vector = 0;
    expected_resume = 0;
    return read_ok && observed_vector != 14;
}

int x86_expect_page_fault(uintptr_t address, uint64_t *error_code, uint64_t *cr2) {
    if (!run_exception_test(14, (uintptr_t)x86_test_pf_resume, x86_test_pf, address) ||
        (observed_error & 1u) || observed_cr2 != address) return 0;
    if (error_code) *error_code = observed_error;
    if (cr2) *cr2 = observed_cr2;
    return 1;
}

int x86_expect_execute_fault(uintptr_t address, uint64_t *error_code, uint64_t *cr2) {
    if (!run_exception_test(14, (uintptr_t)x86_test_execute_resume,
                            x86_test_execute, address) ||
        !(observed_error & 1u) || !(observed_error & (1u << 4)) || observed_cr2 != address)
        return 0;
    if (error_code) *error_code = observed_error;
    if (cr2) *cr2 = observed_cr2;
    return 1;
}

static int exception_self_test(void) {
    if (!run_exception_test(6, (uintptr_t)x86_test_ud_resume, x86_test_ud, 0)) {
        serial_log(SERIAL_ERROR, "SELFTEST FAIL: invalid opcode (#UD).");
        return 0;
    }
    serial_log(SERIAL_INFO, "SELFTEST PASS: invalid opcode (#UD) recovered.");

    if (!run_exception_test(13, (uintptr_t)x86_test_gp_resume, x86_test_gp, 0) || !observed_error) {
        serial_log(SERIAL_ERROR, "SELFTEST FAIL: general protection fault (#GP).");
        return 0;
    }
    serial_log(SERIAL_INFO, "SELFTEST PASS: general protection fault (#GP) recovered.");
    log_hex(SERIAL_INFO, "#GP error code=", observed_error);

    const volatile uint64_t *unmapped = (const volatile uint64_t *)(uintptr_t)UINT64_C(0x0000400000000000);
    if (!run_exception_test(14, (uintptr_t)x86_test_pf_resume, x86_test_pf,
                            (uintptr_t)unmapped) || (observed_error & 7u) != 0 ||
        observed_cr2 != (uintptr_t)unmapped) {
        serial_log(SERIAL_ERROR, "SELFTEST FAIL: page fault (#PF) or CR2 capture.");
        return 0;
    }
    serial_log(SERIAL_INFO, "SELFTEST PASS: page fault (#PF) recovered; CR2 captured.");
    log_hex(SERIAL_INFO, "#PF error code=", observed_error);
    log_hex(SERIAL_INFO, "#PF CR2=", observed_cr2);
    return 1;
}

static void pic_remap_and_mask(void) {
    out8(0x20, 0x11); out8(0xA0, 0x11);
    out8(0x80, 0);
    out8(0x21, 0x20); out8(0xA1, 0x28);
    out8(0x80, 0);
    out8(0x21, 0x04); out8(0xA1, 0x02);
    out8(0x80, 0);
    out8(0x21, 0x01); out8(0xA1, 0x01);
    out8(0x80, 0);
    master_pic_mask = 0xFF;
    slave_pic_mask = 0xFF;
    out8(0x21, master_pic_mask);
    out8(0xA1, slave_pic_mask);
}

static void pit_start_100hz(void) {
    const uint16_t divisor = 11932;
    out8(0x43, 0x36);
    out8(0x40, (uint8_t)divisor);
    out8(0x40, (uint8_t)(divisor >> 8));
    master_pic_mask = 0xFE; /* IRQ0 only; keyboard remains polled. */
    out8(0x21, master_pic_mask);
}

int x86_interrupts_init(void) {
    __asm__ volatile("cli" ::: "memory");
    install_gdt();
    install_idt();
    serial_log(SERIAL_INFO, "CPU tables installed: GDT, TSS, and 256-entry IDT.");

    if (!exception_self_test()) return 0;

    pic_remap_and_mask();
    pit_start_100hz();
    __asm__ volatile("sti" ::: "memory");
    for (volatile uint32_t spins = 0; spins < 50000000u && timer_ticks == 0; ++spins)
        __asm__ volatile("pause");
    if (timer_ticks == 0) {
        __asm__ volatile("cli" ::: "memory");
        serial_log(SERIAL_ERROR, "SELFTEST FAIL: PIT IRQ0 produced no timer tick.");
        return 0;
    }
    serial_log(SERIAL_INFO, "SELFTEST PASS: remapped PIC and PIT IRQ0 timer is ticking.");
    return 1;
}

uint64_t x86_timer_ticks(void) {
    return timer_ticks;
}
