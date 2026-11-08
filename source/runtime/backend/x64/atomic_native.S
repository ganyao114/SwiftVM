// SysV address/expected/desired/observed*/fault* use rdi/rsi/rdx/rcx/r8.
// FaultScope redirects an access fault to Fault and supplies si_addr in r9.
#if defined(__linux__) && defined(__x86_64__)
.text
.p2align 4
.global SvmX64CompareExchange64
.type SvmX64CompareExchange64, @function
SvmX64CompareExchange64:
    movq %rsi,%rax
    mfence
.global SvmX64CompareExchange64Begin
SvmX64CompareExchange64Begin:
    lock cmpxchgq %rdx,(%rdi)
.global SvmX64CompareExchange64End
SvmX64CompareExchange64End:
    movq %rax,(%rcx)
    movq $0,(%r8)
    xorl %eax,%eax
    mfence
    ret
.global SvmX64CompareExchange64Fault
SvmX64CompareExchange64Fault:
    movq %r9,(%r8)
    movq $0,(%rcx)
    movl $1,%eax
    mfence
    ret
.size SvmX64CompareExchange64, .-SvmX64CompareExchange64

// SysV address/expected*/desired*/observed*/fault* use the same registers.
.p2align 4
.global SvmX64CompareExchange128
.type SvmX64CompareExchange128, @function
SvmX64CompareExchange128:
    pushq %rbx
    movq %rcx,%r10
    movq %rdx,%r11
    movq (%rsi),%rax
    movq 8(%rsi),%rdx
    movq (%r11),%rbx
    movq 8(%r11),%rcx
    mfence
.global SvmX64CompareExchange128Begin
SvmX64CompareExchange128Begin:
    lock cmpxchg16b (%rdi)
.global SvmX64CompareExchange128End
SvmX64CompareExchange128End:
    movq %rax,(%r10)
    movq %rdx,8(%r10)
    movq $0,(%r8)
    xorl %eax,%eax
    mfence
    popq %rbx
    ret
.global SvmX64CompareExchange128Fault
SvmX64CompareExchange128Fault:
    movq %r9,(%r8)
    movq $0,(%r10)
    movq $0,8(%r10)
    movl $1,%eax
    mfence
    popq %rbx
    ret
.size SvmX64CompareExchange128, .-SvmX64CompareExchange128
.section .note.GNU-stack,"",@progbits
#endif
