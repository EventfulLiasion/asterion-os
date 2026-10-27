.text

.globl x86_user_test_blob_start
x86_user_test_blob_start:
    movabs $0x0000400000001080, %rax
    movabs $0x4153544552494F4E, %rbx
    movq %rbx, (%rax)
    movl $1, %eax
    movabs $0x0000400000001000, %rdi
    movl $53, %esi
    int $0x32
    cmpq $53, %rax
    jne x86_user_test_fail
    movl $4, %eax
    int $0x32
    cmpq $1, %rax
    jne x86_user_test_fail
    movl $3, %eax
    int $0x32
    testq %rax, %rax
    jne x86_user_test_fail
    movl $0xFFFFFFFF, %eax
    int $0x32
    cmpq $-38, %rax
    jne x86_user_test_fail
    movl $1, %eax
    movabs $asterion_user_supervisor_probe, %rdi
    movl $1, %esi
    int $0x32
    cmpq $-14, %rax
    jne x86_user_test_fail
    movl $2, %eax
    xorl %edi, %edi
    int $0x32
    ud2

x86_user_test_fail:
    ud2

.globl x86_user_test_read_start
x86_user_test_read_start:
    movabs $asterion_user_supervisor_probe, %rax
    movq (%rax), %rbx
    int $0x31
    ud2

.globl x86_user_test_blob_end
x86_user_test_blob_end:
