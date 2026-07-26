.global _start
.text
_start:
    rdrand %r8
    jnc 1f
    rdseed %r9
    jnc 1f
    xor %edi, %edi
    mov $60, %eax
    syscall
1:
    mov $1, %edi
    mov $60, %eax
    syscall
