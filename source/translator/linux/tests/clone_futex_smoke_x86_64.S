// Minimal libc-free clone + CHILD_CLEARTID/futex join smoke test.
// This is buildable with the local clang cross assembler while the Orb x64
// VM is unavailable; the pthread C tests remain the qualification target.

    .section .bss
    .balign 16
child_stack:
    .skip 262144
    .balign 4
tid_words:
    .zero 16
lock_word:
    .long 0
    .balign 8
counter:
    .quad 0
atomic_counter:
    .quad 0

    .section .text
    .global _start
_start:
    xor     %r12d, %r12d
spawn:
    // CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|
    // PARENT_SETTID|CHILD_CLEARTID
    mov     $0x350f00, %edi
    lea     child_stack+65536(%rip), %rsi
    mov     %r12, %rax
    shl     $16, %rax
    add     %rax, %rsi
    lea     tid_words(%rip), %rdx
    lea     (%rdx,%r12,4), %rdx
    mov     %rdx, %r10
    xor     %r8d, %r8d
    mov     $56, %eax
    syscall
    test    %eax, %eax
    jz      child
    js      fail
    inc     %r12d
    cmp     $4, %r12d
    jne     spawn

    xor     %r12d, %r12d
wait_child:
    lea     tid_words(%rip), %rdi
    lea     (%rdi,%r12,4), %rdi
    mov     (%rdi), %edx
    test    %edx, %edx
    jz      next_child
    mov     $128, %esi              // FUTEX_WAIT_PRIVATE
    xor     %r10d, %r10d            // timeout = NULL
    xor     %r8d, %r8d
    xor     %r9d, %r9d
    mov     $202, %eax
    syscall
    jmp     wait_child

child:
    mov     $20000, %ecx
child_loop:
    test    $1, %r12d
    jnz     acquire_cmpxchg
    mov     $1, %eax
acquire:
    xchg    %eax, lock_word(%rip)
    test    %eax, %eax
    jz      critical
    pause
    mov     $1, %eax
    jmp     acquire
acquire_cmpxchg:
    xor     %eax, %eax
    mov     $1, %edx
    lock cmpxchg %edx, lock_word(%rip)
    jz      critical
    pause
    jmp     acquire_cmpxchg
critical:
    incq    counter(%rip)
    mov     $1, %rax
    lock xaddq %rax, atomic_counter(%rip)
    movl    $0, lock_word(%rip)
    dec     %ecx
    jne     child_loop
    xor     %edi, %edi
    mov     $60, %eax               // exit current thread
    syscall

next_child:
    inc     %r12d
    cmp     $4, %r12d
    jne     wait_child
joined:
    cmpq    $80000, counter(%rip)
    jne     fail
    cmpq    $80000, atomic_counter(%rip)
    jne     fail
    xor     %edi, %edi
    jmp     finish
fail:
    mov     $1, %edi
finish:
    mov     $231, %eax              // exit_group
    syscall
