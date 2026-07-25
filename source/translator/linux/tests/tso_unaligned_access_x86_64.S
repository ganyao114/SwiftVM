// Ordered non-atomic x86 loads/stores deliberately crossing a cache-line and
// a page boundary. In AcqRel mode these must take the plain-access + DMB arm,
// never the naturally-aligned LDAPR/STLR arm.

    .global _start
    .text
_start:
    lea     cache_line_area+63(%rip), %rdi
    movabs  $0x0123456789abcdef, %rax
    mov     %rax, (%rdi)
    mov     (%rdi), %rdx
    cmp     %rax, %rdx
    jne     fail_cache_line

    lea     page_area+4095(%rip), %rdi
    movabs  $0xfedcba9876543210, %rax
    mov     %rax, (%rdi)
    mov     (%rdi), %rdx
    cmp     %rax, %rdx
    jne     fail_page

    xor     %edi, %edi
    mov     $60, %eax
    syscall

fail_cache_line:
    mov     $1, %edi
    mov     $60, %eax
    syscall

fail_page:
    mov     $2, %edi
    mov     $60, %eax
    syscall

    .data
    .balign 64
cache_line_area:
    .zero   128

    .balign 4096
page_area:
    .zero   8192
