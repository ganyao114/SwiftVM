// Small Linux guest covering the recorded byte-table loop and four SSE2 loops.
// stdout contains the byte table, final arrays and saved addition results.
    .global _start
    .text
_start:
    lea table(%rip), %rdi
    mov $288, %ecx
    mov $0x5a, %al
    rep stosb
    lea input(%rip), %rbp
    lea table(%rip), %rsp
    mov $8, %edx
    movzbl (%rbp), %r12d

    // The instruction sequence from the 7-Zip match-finder loop.
    mov $1, %ecx
    and $-2, %rdx
    add $3, %rdx
byte_loop:
    movzbl (%rbp,%rcx), %eax
    shl $3, %r12
    mov %rax, %rsi
    shl $3, %rax
    sub %r12, %rsi
    movzbl 1(%rbp,%rcx), %r12d
    movzbl %sil, %esi
    mov %cl, 16(%rsp,%rsi)
    mov %r12, %rsi
    sub %rax, %rsi
    movzbl %sil, %eax
    lea 1(%rcx), %esi
    mov %sil, 16(%rsp,%rax)
    mov %rcx, %rax
    add $2, %rcx
    cmp %rdx, %rcx
    jne byte_loop
    cmp $11, %rcx
    jne failed
    cmp $11, %rdx
    jne failed
    cmp $0xa5, %r12
    jne failed

    mov $1, %edi
    lea table(%rip), %rsi
    mov $288, %edx
    mov $1, %eax
    syscall
    cmp $288, %rax
    jne failed

    lea arrays(%rip), %rdi
    lea 256(%rdi), %rsi
    lea 512(%rdi), %rdx
    mov $32, %ecx
    mov $2, %r9d
    lea add_results(%rip), %r10
    movupd factor(%rip), %xmm3
round:
    xor %r8d, %r8d
copy_loop:
    movupd (%rdi,%r8,8), %xmm0
    movupd %xmm0, (%rdx,%r8,8)
    add $2, %r8
    cmp %rcx, %r8
    jb copy_loop
    xor %r8d, %r8d
scale_loop:
    movupd (%rdx,%r8,8), %xmm0
    mulpd %xmm3, %xmm0
    movupd %xmm0, (%rsi,%r8,8)
    add $2, %r8
    cmp %rcx, %r8
    jb scale_loop
    xor %r8d, %r8d
add_loop:
    movupd (%rsi,%r8,8), %xmm0
    movupd (%rdx,%r8,8), %xmm1
    addpd %xmm1, %xmm0
    movupd %xmm0, (%rdi,%r8,8)
    add $2, %r8
    cmp %rcx, %r8
    jb add_loop
    // Preserve each addition result before the triad overwrites array A.
    xor %r11d, %r11d
save_add:
    mov (%rdi,%r11,8), %rax
    mov %rax, (%r10,%r11,8)
    inc %r11
    cmp %rcx, %r11
    jb save_add
    add $256, %r10
    xor %r8d, %r8d
triad_loop:
    movupd (%rdx,%r8,8), %xmm0
    movupd (%rsi,%r8,8), %xmm1
    mulpd %xmm3, %xmm0
    addpd %xmm1, %xmm0
    movupd %xmm0, (%rdi,%r8,8)
    add $2, %r8
    cmp %rcx, %r8
    jb triad_loop
    dec %r9d
    jne round

    mov $1, %edi
    lea arrays(%rip), %rsi
    mov $1280, %edx
    mov $1, %eax
    syscall
    cmp $1280, %rax
    jne failed
    mov $42, %edi
    jmp exit_guest
failed:
    mov $85, %edi
exit_guest:
    mov $231, %eax
    syscall
    ud2

    .balign 4096
input:
    .byte 0xff,0x80,0x7f,0x01,0xfe,0x20,0xc0,0x04,0xf0,0x08,0xa5,0x11
factor:
    .double 3,3
table:
    .space 288
    .balign 16
arrays:
    .double 0.5,1.5,2.5,3.5,4.5,5.5,6.5,7.5
    .double 8.5,9.5,10.5,11.5,12.5,13.5,14.5,15.5
    .double 16.5,17.5,18.5,19.5,20.5,21.5,22.5,23.5
    .double 24.5,25.5,26.5,27.5,28.5,29.5,30.5,31.5
    .space 512
add_results:
    .space 512
