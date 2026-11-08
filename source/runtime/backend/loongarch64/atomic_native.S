// Faultable, naturally aligned CAS64 for the runtime's cold atomic service.
// a0=address, a1=expected, a2=desired, a3=observed*, a4=fault_address*.
// FaultScope redirects only [Begin,End) to Fault and supplies si_addr in a5.
// No host locks, stack frame or callee-saved registers are involved.
#if defined(__linux__) && defined(__loongarch64)
.text
.p2align 2
.global SvmLACompareExchange64
.type SvmLACompareExchange64, @function
SvmLACompareExchange64:
    dbar 0
.global SvmLACompareExchange64Begin
SvmLACompareExchange64Begin:
1:
    ll.d $t0,$a0,0
    bne $t0,$a1,2f
    move $t1,$a2
    sc.d $t1,$a0,0
    beqz $t1,1b
2:
.global SvmLACompareExchange64End
SvmLACompareExchange64End:
    st.d $t0,$a3,0
    st.d $zero,$a4,0
    move $a0,$zero
    dbar 0
    jr $ra
.global SvmLACompareExchange64Fault
SvmLACompareExchange64Fault:
    st.d $a5,$a4,0
    st.d $zero,$a3,0
    addi.d $a0,$zero,1
    dbar 0
    jr $ra
.size SvmLACompareExchange64, .-SvmLACompareExchange64
.section .note.GNU-stack,"",@progbits
#endif
