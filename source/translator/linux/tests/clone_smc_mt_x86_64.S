// MT self-modifying-code stress.
//
// The child enters `worker`, snapshots its patchable mov-immediate, and
// spins inside that translated block. The parent then rewrites the immediate
// while the child is still executing the old JIT allocation, executes getpid
// to retire that allocation, and only then releases the child. The child returns
// the old, internally consistent value, validates it, executes its own getpid
// boundary, and re-enters the newly translated worker. Repeating this forces
// cache invalidation and epoch reclamation while another host thread owns the
// previous epoch.
//
// Exit 0: 512 alternating 1/2 observations and exact checksum.
// Exit 1: clone failed.
// Exit 2: a returned value did not match the fully published encoding.
// Exit 3: checksum mismatch after joining the child.

    .set ITERS, 512
    .set EXPECTED_SUM, (ITERS / 2) * 3

    // Keep code and writable test storage in one locally wrappable section.
    // build_real_tests.sh links this section as an RWX PT_LOAD for native
    // ground truth; SwiftVM's existing SMC test likewise writes executable
    // guest mappings.
    .section .smc,"awx",@progbits
    .global _start
_start:
    // CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|
    // PARENT_SETTID|CHILD_CLEARTID
    mov     $0x350f00, %edi
    lea     child_stack_end(%rip), %rsi
    lea     tid_word(%rip), %rdx
    mov     %rdx, %r10
    xor     %r8d, %r8d
    mov     $56, %eax
    syscall
    test    %eax, %eax
    jz      child
    js      clone_fail

    mov     $1, %r12d
parent_loop:
    // `entered == iteration` proves the child already loaded the old
    // immediate and is executing/spinning in worker's stale allocation.
    cmpl    %r12d, entered(%rip)
    je      patch_worker
    pause
    jmp     parent_loop

patch_worker:
    test    $1, %r12b
    jz      patch_one
    mov     $2, %eax
    jmp     write_patch
patch_one:
    mov     $1, %eax
write_patch:
    lea     worker(%rip), %r15
    movb    %al, 1(%r15)             // B8 imm32: low immediate byte

    // Force the writer through Runtime::Run's host-side close/retire path
    // while the child is still spinning in the old allocation. This makes
    // every iteration exercise deferred rather than immediately-safe free.
    mov     $39, %eax                 // __NR_getpid
    syscall

    // Publish only after retirement. xchg is a full x86 ordering point in
    // both SwiftVM TSO modes and now releases the epoch-protecting child.
    mov     %r12d, %ecx
    xchgl   %ecx, release_word(%rip)

    inc     %r12d
    cmp     $ITERS + 1, %r12d
    jne     parent_loop

wait_child:
    mov     tid_word(%rip), %edx
    test    %edx, %edx
    jz      joined
    lea     tid_word(%rip), %rdi
    mov     $128, %esi                // FUTEX_WAIT_PRIVATE
    xor     %r10d, %r10d
    xor     %r8d, %r8d
    xor     %r9d, %r9d
    mov     $202, %eax                // __NR_futex
    syscall
    jmp     wait_child

joined:
    cmpl    $0, failures(%rip)
    jne     value_fail
    cmpq    $EXPECTED_SUM, checksum(%rip)
    jne     checksum_fail
    xor     %edi, %edi
    jmp     finish

clone_fail:
    mov     $1, %edi
    jmp     finish
value_fail:
    mov     $2, %edi
    jmp     finish
checksum_fail:
    mov     $3, %edi
finish:
    mov     $231, %eax                // __NR_exit_group
    syscall

child:
    mov     $1, %r12d
    xor     %r13d, %r13d
child_loop:
    call    worker
    mov     %eax, %r14d

    // Iteration 1 sees the initial 1; each parent patch installs the value
    // for the following iteration, so odd/even calls alternate 1/2.
    test    $1, %r12b
    jz      expect_two
    cmp     $1, %r14d
    je      value_ok
    jmp     record_failure
expect_two:
    cmp     $2, %r14d
    je      value_ok
record_failure:
    lock incl failures(%rip)
value_ok:
    add     %r14, %r13

    // Serializing observation boundary before the next worker dispatch.
    mov     $39, %eax                 // __NR_getpid
    syscall

    inc     %r12d
    cmp     $ITERS + 1, %r12d
    jne     child_loop

    mov     %r13, checksum(%rip)
    xor     %edi, %edi
    mov     $60, %eax                 // exit current thread
    syscall

    // Put worker on a host-page-distinct guest page from the parent patcher.
    .p2align 14
worker:
    mov     $1, %eax                  // B8 01 00 00 00; byte +1 is patched
    lock incl entered(%rip)
    mov     entered(%rip), %ecx
worker_wait:
    cmp     release_word(%rip), %ecx
    je      worker_done
    pause
    jmp     worker_wait
worker_done:
    ret

    // Keep writable synchronization state and stack off worker's tracked
    // host page so their ordinary writes do not cause spurious SMC faults.
    .p2align 14
    .balign 8
checksum:
    .quad 0
    .balign 4
tid_word:
    .long 0
entered:
    .long 0
release_word:
    .long 0
failures:
    .long 0
    .balign 16
child_stack:
    .zero 65536
child_stack_end:
