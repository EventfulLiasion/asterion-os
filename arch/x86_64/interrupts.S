.text

.globl x86_load_gdt
x86_load_gdt:
    lgdt (%rcx)
    pushq $0x08
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    retq

.globl x86_test_ud
x86_test_ud:
    ud2
.globl x86_test_ud_resume
x86_test_ud_resume:
    retq

.globl x86_test_gp
x86_test_gp:
    int $0x80
.globl x86_test_gp_resume
x86_test_gp_resume:
    retq

.globl x86_test_pf
x86_test_pf:
    movq (%rcx), %rax
.globl x86_test_pf_resume
x86_test_pf_resume:
    retq

.globl x86_try_read64
x86_try_read64:
    movq (%rcx), %rax
    movq %rax, (%rdx)
    movl $1, %eax
    retq
.globl x86_try_read64_resume
x86_try_read64_resume:
    xorl %eax, %eax
    retq

.globl x86_test_execute
x86_test_execute:
    call *%rcx
    retq
.globl x86_test_execute_resume
x86_test_execute_resume:
    addq $8, %rsp
    retq

.macro ISR_NO_ERROR number
.globl x86_isr_\number
x86_isr_\number:
    pushq $0
    pushq $\number
    jmp x86_interrupt_common
.endm

.macro ISR_ERROR number
.globl x86_isr_\number
x86_isr_\number:
    pushq $\number
    jmp x86_interrupt_common
.endm

ISR_NO_ERROR 0
ISR_NO_ERROR 1
ISR_NO_ERROR 2
ISR_NO_ERROR 3
ISR_NO_ERROR 4
ISR_NO_ERROR 5
ISR_NO_ERROR 6
ISR_NO_ERROR 7
ISR_ERROR 8
ISR_NO_ERROR 9
ISR_ERROR 10
ISR_ERROR 11
ISR_ERROR 12
ISR_ERROR 13
ISR_ERROR 14
ISR_NO_ERROR 15
ISR_NO_ERROR 16
ISR_NO_ERROR 17
ISR_NO_ERROR 18
ISR_NO_ERROR 19
ISR_NO_ERROR 20
ISR_ERROR 21
ISR_NO_ERROR 22
ISR_NO_ERROR 23
ISR_NO_ERROR 24
ISR_NO_ERROR 25
ISR_NO_ERROR 26
ISR_NO_ERROR 27
ISR_NO_ERROR 28
ISR_ERROR 29
ISR_ERROR 30
ISR_NO_ERROR 31

.macro IRQ_STUB number, vector
.globl x86_irq_\number
x86_irq_\number:
    pushq $0
    pushq $\vector
    jmp x86_interrupt_common
.endm

IRQ_STUB 0, 32
IRQ_STUB 1, 33
IRQ_STUB 2, 34
IRQ_STUB 3, 35
IRQ_STUB 4, 36
IRQ_STUB 5, 37
IRQ_STUB 6, 38
IRQ_STUB 7, 39
IRQ_STUB 8, 40
IRQ_STUB 9, 41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

.globl x86_yield_stub
x86_yield_stub:
    pushq $0
    pushq $48
    jmp x86_interrupt_common

.globl x86_thread_bootstrap
x86_thread_bootstrap:
    subq $32, %rsp
    call thread_start_current
    ud2

.globl x86_enter_user
x86_enter_user:
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, x86_user_saved_kernel_rsp(%rip)
    pushq $0x33
    pushq %rdx
    pushq %r8
    pushq $0x2B
    pushq %rcx
    iretq

.globl x86_user_return_resume
x86_user_return_resume:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    retq

.globl x86_user_exit_interrupt_stub
x86_user_exit_interrupt_stub:
    pushq $0
    pushq $49
    jmp x86_interrupt_common

.globl x86_user_exit_stub
x86_user_exit_stub:
    int $0x31
    ud2

.globl x86_syscall_interrupt_stub
x86_syscall_interrupt_stub:
    pushq $0
    pushq $50
    jmp x86_interrupt_common

.globl x86_apic_spurious_stub
x86_apic_spurious_stub:
    pushq $0
    pushq $255
    jmp x86_interrupt_common

x86_interrupt_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %r12
    andq $-16, %rsp
    subq $32, %rsp
    cld
    movq %r12, %rcx
    call x86_interrupt_dispatch
    movq %rax, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq
