// FaultScope supplies si_addr in x5 and redirects a guest access fault to the
// corresponding Fault epilogue. Output pointers refer to owner stack storage.
#if defined(__linux__) && defined(__aarch64__)
.text
.p2align 2
.global SvmA64CompareExchange64
.type SvmA64CompareExchange64, %function
SvmA64CompareExchange64:
    dmb ish
.global SvmA64CompareExchange64Begin
SvmA64CompareExchange64Begin:
1:
    ldaxr x6,[x0]
    cmp x6,x1
    b.ne 2f
    stlxr w7,x2,[x0]
    cbnz w7,1b
    b 3f
2:
    clrex
3:
.global SvmA64CompareExchange64End
SvmA64CompareExchange64End:
    str x6,[x3]
    str xzr,[x4]
    mov x0,0
    dmb ish
    ret
.global SvmA64CompareExchange64Fault
SvmA64CompareExchange64Fault:
    str x5,[x4]
    str xzr,[x3]
    mov x0,1
    dmb ish
    ret
.size SvmA64CompareExchange64, .-SvmA64CompareExchange64

// x0=address, x1=expected*, x2=desired*, x3=observed*, x4=fault_address*.
// A mismatching LDAXP can be torn; the service retries ordinary CAS with an
// old->old successful STLXP. StoreExclusive is restricted to private execution.
.p2align 2
.global SvmA64CompareExchange128
.type SvmA64CompareExchange128, %function
SvmA64CompareExchange128:
    ldp x6,x7,[x1]
    ldp x8,x9,[x2]
    dmb ish
.global SvmA64CompareExchange128Begin
SvmA64CompareExchange128Begin:
1:
    ldaxp x10,x11,[x0]
    cmp x10,x6
    b.ne 2f
    cmp x11,x7
    b.ne 2f
    stlxp w12,x8,x9,[x0]
    cbnz w12,1b
    b 3f
2:
    clrex
3:
.global SvmA64CompareExchange128End
SvmA64CompareExchange128End:
    stp x10,x11,[x3]
    str xzr,[x4]
    mov x0,0
    dmb ish
    ret
.global SvmA64CompareExchange128Fault
SvmA64CompareExchange128Fault:
    str x5,[x4]
    stp xzr,xzr,[x3]
    mov x0,1
    dmb ish
    ret
.size SvmA64CompareExchange128, .-SvmA64CompareExchange128
.section .note.GNU-stack,"",%progbits
#endif
