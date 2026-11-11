.global _start
.text
_start:
    mov $1, %rax
    mov $1, %rdi
    lea msg(%rip), %rsi
    mov $14, %rdx
    syscall
    mov $60, %rax
    mov $42, %rdi
    syscall
.data
msg: .ascii "tiny-x86-64-ok\n"
