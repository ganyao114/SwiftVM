// Raw Linux x86_64 stress test for block-local x87 TOP virtualization.
//
// The loop and conditional deliberately carry a live x87 stack across CFG
// edges. FSIN forces a SoftFloat helper in the middle of otherwise inline
// stack/arithmetic work. The final checks cover both the numeric result and
// reconstructed TOP/tag state.
    .global _start
    .text
_start:
    fninit

    // Fill all eight stack slots, exercise a deep FXCH, then collapse the
    // stack with seven pop-form additions. Eight exact ones become 8.0.
    fld1
    fld1
    fld1
    fld1
    fld1
    fld1
    fld1
    fld1
    fxch    %st(7)
    faddp
    faddp
    faddp
    faddp
    faddp
    faddp
    faddp

    // Transcendental helper bailout between two register operations. sin(0)
    // is exact, and FADDP restores a one-value stack containing 8.0.
    fldz
    fsin
    faddp

    // Keep accumulator/factor live across a backedge. Net stack depth of the
    // loop is zero, which is the join-uniformity condition.
    fld1
    fxch    %st(1)
    mov     $4, %ecx
loop:
    fadd    %st(1), %st(0)
    fxch    %st(1)
    fxch    %st(1)
    dec     %ecx
    jne     loop

    // Compare in the middle of the chain. Duplicate/pop leaves both TOP and
    // stack depth unchanged while FCOMIP defines the branch flags.
    fld     %st(0)
    fcomip  %st(1), %st
    jne     fail_compare

    // Both arms have identical stack effects at the join. The untaken arm is
    // retained so function-mode CFG analysis must prove the merge.
    test    %eax, %eax
    jz      else_path
then_path:
    fld1
    faddp
    jmp     joined
else_path:
    fld1
    faddp
joined:
    fistpll result(%rip)       // accumulator = 8 + 4 + 1 = 13
    cmpq    $13, result(%rip)
    jne     fail_value
    fstp    %st(0)             // discard the loop factor; stack becomes empty

    fnstsw  %ax
    mov     %ax, %dx
    and     $0x3800, %dx
    jne     fail_state         // TOP wrapped back to zero

    // A ninth push is now legal only if every prior pop/tag update was
    // reconstructed correctly.
    fld1
    fstp    %st(0)

    xor     %edi, %edi
    jmp     exit
fail_compare:
    mov     $1, %edi
    jmp     exit
fail_value:
    mov     $2, %edi
    jmp     exit
fail_state:
    mov     $3, %edi
exit:
    mov     $231, %eax
    syscall

    .p2align 3
result:
    .quad 0
