// Real x86 raises #GP for LOCK CMPXCHG16B unless m128 is 16-byte aligned.
// SwiftVM maps that synchronous guest fault to its PageFatal driver exit.
// Reaching exit_group means the required fault was not delivered.

    .section .bss
    .balign 16
storage:
    .zero 32

    .section .text
    .global _start
_start:
    xor     %eax, %eax
    xor     %edx, %edx
    mov     $1, %ebx
    mov     $-1, %rcx
    lea     storage+1(%rip), %rdi
    lock cmpxchg16b (%rdi)
    mov     $77, %edi
    mov     $231, %eax
    syscall
