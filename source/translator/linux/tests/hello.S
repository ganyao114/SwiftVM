// Minimal statically-linked ARM64 Linux hello-world for SwiftVM.
// write(1, msg, len); exit_group(42);
// No relocations: the message is reached with PC-relative `adr`, so the
// .text section can be dumped raw and wrapped in an ELF by gen_hello_elf.py.
    .global _start
    .text
_start:
    mov     x0, #1              // fd = stdout
    adr     x1, msg             // buf
    mov     x2, #16             // len
    mov     x8, #64             // __NR_write
    svc     #0
    mov     x0, #42             // exit status
    mov     x8, #94             // __NR_exit_group
    svc     #0
msg:
    .ascii  "Hello, SwiftVM!\n"
