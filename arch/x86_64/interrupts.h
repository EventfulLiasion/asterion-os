#ifndef ASTERION_X86_64_INTERRUPTS_H
#define ASTERION_X86_64_INTERRUPTS_H

#include <stdint.h>
#include <stddef.h>

#define X86_USER_CODE_SELECTOR UINT64_C(0x2B)
#define X86_USER_DATA_SELECTOR UINT64_C(0x33)

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} x86_idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} x86_descriptor_pointer_t;

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, error, rip, cs, rflags;
    /* Always pushed in 64-bit mode, including same-CPL interrupts. */
    uint64_t old_rsp, old_ss;
} x86_interrupt_frame_t;

/* Exact 64-bit frame consumed by the common iretq epilogue (176 bytes). */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, error, rip, cs, rflags, old_rsp, old_ss;
} x86_interrupt_return_frame_t;

/* Ring-3 IRETQ frame: unlike a same-ring frame, selectors carry RPL=3. */
typedef struct {
    uint64_t rip, cs, rflags, rsp, ss;
} x86_user_iret_frame_t;

static inline int x86_user_iret_frame_init(x86_user_iret_frame_t *frame,
                                           uintptr_t rip, uintptr_t rsp,
                                           uint64_t user_code_selector,
                                           uint64_t user_data_selector,
                                           uint8_t levels) {
    uint8_t bits = levels == 4 ? 48 : (levels == 5 ? 57 : 0);
    if (!frame || !bits || !rip || rip >= (UINT64_C(1) << (bits - 1)) ||
        !rsp || rsp >= (UINT64_C(1) << (bits - 1)) || (rsp & 15u) ||
        user_code_selector != X86_USER_CODE_SELECTOR ||
        user_data_selector != X86_USER_DATA_SELECTOR)
        return 0;
    frame->rip = (uint64_t)rip;
    frame->cs = user_code_selector;
    frame->rflags = UINT64_C(0x002); /* IF, IOPL, NT, and VM clear for controlled entry. */
    frame->rsp = (uint64_t)rsp;
    frame->ss = user_data_selector;
    return 1;
}

_Static_assert(sizeof(x86_idt_entry_t) == 16, "x86-64 IDT gate must be 16 bytes");
_Static_assert(sizeof(x86_descriptor_pointer_t) == 10, "x86-64 descriptor pointer must be 10 bytes");
_Static_assert(offsetof(x86_interrupt_frame_t, vector) == 120, "interrupt stub register frame layout changed");
_Static_assert(offsetof(x86_interrupt_frame_t, error) == 128, "interrupt error-code offset changed");
_Static_assert(offsetof(x86_interrupt_frame_t, rip) == 136, "interrupt saved-RIP offset changed");
_Static_assert(offsetof(x86_interrupt_frame_t, rflags) == 152, "interrupt saved-RFLAGS offset changed");
_Static_assert(offsetof(x86_interrupt_frame_t, old_rsp) == 160, "interrupt saved-RSP offset changed");
_Static_assert(offsetof(x86_interrupt_frame_t, old_ss) == 168, "interrupt saved-SS offset changed");
_Static_assert(sizeof(x86_interrupt_frame_t) == 176, "interrupt frame layout changed");
_Static_assert(offsetof(x86_interrupt_return_frame_t, vector) == 120, "return frame register layout changed");
_Static_assert(offsetof(x86_interrupt_return_frame_t, rip) == 136, "return frame RIP offset changed");
_Static_assert(offsetof(x86_interrupt_return_frame_t, cs) == 144, "return frame CS offset changed");
_Static_assert(offsetof(x86_interrupt_return_frame_t, rflags) == 152, "return frame flags offset changed");
_Static_assert(offsetof(x86_interrupt_return_frame_t, old_rsp) == 160, "return frame RSP offset changed");
_Static_assert(offsetof(x86_interrupt_return_frame_t, old_ss) == 168, "return frame SS offset changed");
_Static_assert(sizeof(x86_interrupt_return_frame_t) == 176, "64-bit return frame must be 176 bytes");
_Static_assert(sizeof(x86_user_iret_frame_t) == 40, "ring-3 IRETQ frame must be five qwords");

static inline void x86_interrupt_return_frame_init(x86_interrupt_return_frame_t *frame,
                                                     uintptr_t rip, uint64_t cs,
                                                     uint64_t rflags, uintptr_t rsp,
                                                     uint64_t ss) {
    uint64_t *words = (uint64_t *)frame;
    for (size_t i = 0; i < sizeof(*frame) / sizeof(*words); ++i) words[i] = 0;
    frame->rip = (uint64_t)rip;
    frame->cs = cs;
    frame->rflags = rflags;
    frame->old_rsp = (uint64_t)rsp;
    frame->old_ss = ss;
}

static inline int x86_interrupt_return_frame_valid(const x86_interrupt_return_frame_t *frame,
                                                    uint64_t kernel_code_selector,
                                                    uint64_t kernel_data_selector) {
    return frame && frame->cs == kernel_code_selector && frame->old_ss == kernel_data_selector &&
           frame->old_rsp != 0 && (frame->rflags & 2u) != 0;
}

static inline void x86_idt_entry_set(x86_idt_entry_t *entry, uintptr_t address,
                                     uint8_t ist, uint8_t attributes) {
    entry->offset_low = (uint16_t)address;
    entry->selector = 0x08;
    entry->ist = ist & 7u;
    entry->attributes = attributes;
    entry->offset_middle = (uint16_t)(address >> 16);
    entry->offset_high = (uint32_t)(address >> 32);
    entry->reserved = 0;
}

static inline void x86_tss_descriptor_set(uint64_t descriptor[2], uintptr_t base,
                                          uint32_t limit) {
    descriptor[0] = (limit & 0xFFFFu) |
                    (((uint64_t)base & 0xFFFFFFu) << 16) |
                    (UINT64_C(0x89) << 40) |
                    (((uint64_t)limit & 0xF0000u) << 32) |
                    (((uint64_t)base & UINT64_C(0xFF000000)) << 32);
    descriptor[1] = (uint64_t)base >> 32;
}

int x86_interrupts_init(void);
uintptr_t x86_set_kernel_stack(uintptr_t stack_top);
void x86_enter_user(uintptr_t instruction_pointer, uintptr_t stack_pointer,
                    uint64_t rflags);
extern uintptr_t x86_user_saved_kernel_rsp;
extern void x86_user_return_resume(void);
uint64_t x86_timer_ticks(void);
int x86_probe_read64(uintptr_t address, uint64_t *value);
int x86_expect_page_fault(uintptr_t address, uint64_t *error_code, uint64_t *cr2);
int x86_expect_execute_fault(uintptr_t address, uint64_t *error_code, uint64_t *cr2);

#endif
