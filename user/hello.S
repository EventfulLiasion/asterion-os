.section .text
.global _start
_start:
    lea message(%rip), %rdi
    mov $1, %eax
    mov $message_length, %esi
    int $0x32
    cmp $message_length, %rax
    jne fail
    mov $4, %eax
    int $0x32
    cmp $1, %rax
    jne fail
    mov $3, %eax
    int $0x32
    test %rax, %rax
    jne fail
    mov $2, %eax
    xor %edi, %edi
    int $0x32
    ud2
fail:
    mov $2, %eax
    mov $1, %edi
    int $0x32
    ud2
message:
    .ascii "Hello from independently assembled hello.elf via syscall write.\r\n"
.set message_length, . - message
