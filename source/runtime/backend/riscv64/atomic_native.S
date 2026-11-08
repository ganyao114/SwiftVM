// Faultable, naturally aligned CAS64 for the runtime's cold atomic service.
// a0=address, a1=expected, a2=desired, a3=observed*, a4=fault_address*.
// FaultScope redirects only [Begin,End) to Fault and supplies si_addr in a5.
// No host locks, stack frame or callee-saved registers are involved.
#if defined(__linux__) && defined(__riscv) && __riscv_xlen == 64
.text
.option push
.option norvc
.option norelax
.p2align 2
.global SvmRVCompareExchange64
.type SvmRVCompareExchange64, @function
SvmRVCompareExchange64:
    fence rw,rw
.global SvmRVCompareExchange64Begin
SvmRVCompareExchange64Begin:
1:
    lr.d.aq t0,(a0)
    bne t0,a1,2f
    sc.d.rl t1,a2,(a0)
    bnez t1,1b
2:
.global SvmRVCompareExchange64End
SvmRVCompareExchange64End:
    sd t0,0(a3)
    sd zero,0(a4)
    li a0,0
    fence rw,rw
    ret
.global SvmRVCompareExchange64Fault
SvmRVCompareExchange64Fault:
    sd a5,0(a4)
    sd zero,0(a3)
    li a0,1
    fence rw,rw
    ret
.size SvmRVCompareExchange64, .-SvmRVCompareExchange64
.option pop
.section .note.GNU-stack,"",@progbits
#endif
