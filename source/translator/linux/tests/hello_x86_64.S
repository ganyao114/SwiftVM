// Minimal statically-linked x86_64 Linux hello-world for SwiftVM.
// write(1, msg, len); exit_group(42);
// No relocations: the message is reached with RIP-relative `lea`, so the
// .text section can be dumped raw and wrapped in an ELF by
// gen_hello_x86_64.py.
    .global _start
    .text
_start:
    mov     $1, %edi            // fd = stdout
    lea     msg(%rip), %rsi     // buf
    mov     $16, %edx           // len
    mov     $1, %eax            // __NR_write
    syscall
    mov     $42, %edi           // exit status
    mov     $231, %eax          // __NR_exit_group
    syscall
msg:
    .ascii  "Hello, SwiftVM!\n"
