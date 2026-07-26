.global _start
.text
_start:
    rdtsc
    shl $32, %rdx
    or %rdx, %rax
    mov %rax, %r8

    mov $1000000, %ecx
1:
    dec %ecx
    jnz 1b

    rdtscp
    shl $32, %rdx
    or %rdx, %rax
    test %ecx, %ecx
    jne 2f
    cmp %r8, %rax
    jbe 2f

    xor %edi, %edi
    mov $60, %eax
    syscall
2:
    mov $1, %edi
    mov $60, %eax
    syscall
