#include <assert.h>
#include <stdint.h>
#include "../arch/x86_64/interrupts.h"

int main(void) {
    assert(offsetof(x86_interrupt_frame_t, vector) == 120);
    assert(offsetof(x86_interrupt_frame_t, rip) == 136);
    assert(sizeof(x86_interrupt_frame_t) == 176);
    assert(offsetof(x86_interrupt_frame_t, old_rsp) == 160);
    assert(offsetof(x86_interrupt_frame_t, old_ss) == 168);

    x86_interrupt_return_frame_t initial;
    x86_interrupt_return_frame_init(&initial, (uintptr_t)0x12345678, 0x08, 0x202,
                                    (uintptr_t)0xABCDEF00, 0x10);
    assert(sizeof(initial) == 176);
    assert(initial.vector == 0 && initial.error == 0);
    assert(initial.rip == 0x12345678 && initial.cs == 0x08 && initial.rflags == 0x202);
    assert(initial.old_rsp == 0xABCDEF00 && initial.old_ss == 0x10);
    assert(x86_interrupt_return_frame_valid(&initial, 0x08, 0x10));
    initial.cs = 0x4030;
    assert(!x86_interrupt_return_frame_valid(&initial, 0x08, 0x10));

    x86_user_iret_frame_t user_frame;
    assert(x86_user_iret_frame_init(&user_frame, 0x400000, 0x800000,
                                    0x2B, 0x33, 4));
    assert(user_frame.rip == 0x400000 && user_frame.rsp == 0x800000);
    assert(user_frame.cs == 0x2B && user_frame.ss == 0x33);
    assert(user_frame.rflags == 0x002 && sizeof(user_frame) == 40);
    /* Match the controlled ELF smoke launch's user entry and stack top. */
    assert(x86_user_iret_frame_init(&user_frame, UINT64_C(0x0000400000000000),
                                    UINT64_C(0x0000400000201000),
                                    X86_USER_CODE_SELECTOR, X86_USER_DATA_SELECTOR, 4));
    assert(user_frame.rip == UINT64_C(0x0000400000000000));
    assert(user_frame.rsp == UINT64_C(0x0000400000201000));
    assert((user_frame.rsp & 15u) == 0 && user_frame.cs == 0x2B && user_frame.ss == 0x33);
    assert(!x86_user_iret_frame_init(&user_frame, 0x0000800000000000,
                                     0x800000, 0x2B, 0x33, 4));
    assert(!x86_user_iret_frame_init(&user_frame, 0x400000, 0x800008,
                                     0x2B, 0x33, 4));
    assert(!x86_user_iret_frame_init(&user_frame, 0x400000,
                                     UINT64_C(0x0000800000000000), 0x2B, 0x33, 4));
    assert(!x86_user_iret_frame_init(&user_frame, 0x400000, 0x800000,
                                     0x08, 0x10, 4));
    assert(!x86_user_iret_frame_init(&user_frame, 0x400000, 0x800000,
                                     0x3B, 0x43, 4));
    initial.cs = 0x08;
    initial.old_ss = 0x4030;
    assert(!x86_interrupt_return_frame_valid(&initial, 0x08, 0x10));
    initial.old_ss = 0x10;
    initial.old_rsp = 0;
    assert(!x86_interrupt_return_frame_valid(&initial, 0x08, 0x10));

    x86_idt_entry_t gate = {0};
    const uintptr_t handler = UINT64_C(0x123456789ABCDEF0);
    x86_idt_entry_set(&gate, handler, 10, 0x8E);
    assert(gate.offset_low == 0xDEF0);
    assert(gate.offset_middle == 0x9ABC);
    assert(gate.offset_high == 0x12345678);
    assert(gate.selector == 0x08);
    assert(gate.ist == 2);
    assert(gate.attributes == 0x8E);
    assert(gate.reserved == 0);

    uint64_t tss[2];
    const uintptr_t tss_base = UINT64_C(0xFEDCBA9876543210);
    const uint32_t tss_limit = 103;
    x86_tss_descriptor_set(tss, tss_base, tss_limit);
    assert(((tss[0] >> 40) & 0xFF) == 0x89);
    assert((tss[0] & 0xFFFF) == (tss_limit & 0xFFFF));
    assert(((tss[0] >> 48) & 0xF) == ((tss_limit >> 16) & 0xF));
    uint64_t reconstructed_base = ((tss[0] >> 16) & 0xFFFFFF) |
                                  (((tss[0] >> 56) & 0xFF) << 24) |
                                  (tss[1] << 32);
    assert(reconstructed_base == tss_base);
    return 0;
}
