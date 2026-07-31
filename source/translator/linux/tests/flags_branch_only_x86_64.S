/*
 * W59 branch-only flags directed test.
 *
 * Each producer family exercises all 16 architectural Jcc condition classes
 * with both a carry/zero-oriented vector and an overflow-oriented vector.
 * The first instruction on both successors is XOR, a full arithmetic-flags
 * overwrite, so these are positive branch-only candidates.
 *
 * The observer section deliberately puts LAHF, PUSHFQ, SETcc, CMOVcc, or ADC
 * between a producer and the following Jcc. The cross_flags_live case starts
 * both successors with ADC, making the producer's flags live across the edge.
 */

    .section .rodata
pass_message:
    .ascii "W59 flags branch-only directed: PASS\n"
    .set pass_message_len, . - pass_message
fail_message:
    .ascii "W59 flags branch-only directed: FAIL\n"
    .set fail_message_len, . - fail_message

    .text

    .macro cmp_cf
        movq $0, %r8
        cmpq $1, %r8
    .endm
    .macro cmp_of
        movabsq $0x8000000000000000, %r8
        cmpq $1, %r8
    .endm
    .macro test_even
        movq $3, %r8
        testq %r8, %r8
    .endm
    .macro test_zero
        movq $0, %r8
        testq %r8, %r8
    .endm
    .macro sub_cf
        movq $0, %r8
        subq $1, %r8
    .endm
    .macro sub_of
        movabsq $0x8000000000000000, %r8
        subq $1, %r8
    .endm
    .macro add_cz
        movq $-1, %r8
        addq $1, %r8
    .endm
    .macro add_of
        movabsq $0x7fffffffffffffff, %r8
        addq $1, %r8
    .endm

    .macro CHECK kind, cc, expected
        \kind
        \cc .Ltaken\@
        cmpl %r12d, %r12d
        xorl %r12d, %r12d
        jmp .Ljoin\@
.Ltaken\@:
        cmpl %r12d, %r12d
        xorl %r12d, %r12d
        movl $1, %r12d
.Ljoin\@:
        xorl $\expected, %r12d
        orl %r12d, %r15d
    .endm

    .macro CHECK_CMP_SUB_CF kind
        CHECK \kind, jo,  0
        CHECK \kind, jno, 1
        CHECK \kind, jb,  1
        CHECK \kind, jae, 0
        CHECK \kind, je,  0
        CHECK \kind, jne, 1
        CHECK \kind, jbe, 1
        CHECK \kind, ja,  0
        CHECK \kind, js,  1
        CHECK \kind, jns, 0
        CHECK \kind, jp,  1
        CHECK \kind, jnp, 0
        CHECK \kind, jl,  1
        CHECK \kind, jge, 0
        CHECK \kind, jle, 1
        CHECK \kind, jg,  0
    .endm

    .macro CHECK_CMP_SUB_OF kind
        CHECK \kind, jo,  1
        CHECK \kind, jno, 0
        CHECK \kind, jb,  0
        CHECK \kind, jae, 1
        CHECK \kind, je,  0
        CHECK \kind, jne, 1
        CHECK \kind, jbe, 0
        CHECK \kind, ja,  1
        CHECK \kind, js,  0
        CHECK \kind, jns, 1
        CHECK \kind, jp,  1
        CHECK \kind, jnp, 0
        CHECK \kind, jl,  1
        CHECK \kind, jge, 0
        CHECK \kind, jle, 1
        CHECK \kind, jg,  0
    .endm

    .macro CHECK_TEST_EVEN kind
        CHECK \kind, jo,  0
        CHECK \kind, jno, 1
        CHECK \kind, jb,  0
        CHECK \kind, jae, 1
        CHECK \kind, je,  0
        CHECK \kind, jne, 1
        CHECK \kind, jbe, 0
        CHECK \kind, ja,  1
        CHECK \kind, js,  0
        CHECK \kind, jns, 1
        CHECK \kind, jp,  1
        CHECK \kind, jnp, 0
        CHECK \kind, jl,  0
        CHECK \kind, jge, 1
        CHECK \kind, jle, 0
        CHECK \kind, jg,  1
    .endm

    .macro CHECK_TEST_ZERO kind
        CHECK \kind, jo,  0
        CHECK \kind, jno, 1
        CHECK \kind, jb,  0
        CHECK \kind, jae, 1
        CHECK \kind, je,  1
        CHECK \kind, jne, 0
        CHECK \kind, jbe, 1
        CHECK \kind, ja,  0
        CHECK \kind, js,  0
        CHECK \kind, jns, 1
        CHECK \kind, jp,  1
        CHECK \kind, jnp, 0
        CHECK \kind, jl,  0
        CHECK \kind, jge, 1
        CHECK \kind, jle, 1
        CHECK \kind, jg,  0
    .endm

    .macro CHECK_ADD_CZ kind
        CHECK \kind, jo,  0
        CHECK \kind, jno, 1
        CHECK \kind, jb,  1
        CHECK \kind, jae, 0
        CHECK \kind, je,  1
        CHECK \kind, jne, 0
        CHECK \kind, jbe, 1
        CHECK \kind, ja,  0
        CHECK \kind, js,  0
        CHECK \kind, jns, 1
        CHECK \kind, jp,  1
        CHECK \kind, jnp, 0
        CHECK \kind, jl,  0
        CHECK \kind, jge, 1
        CHECK \kind, jle, 1
        CHECK \kind, jg,  0
    .endm

    .macro CHECK_ADD_OF kind
        CHECK \kind, jo,  1
        CHECK \kind, jno, 0
        CHECK \kind, jb,  0
        CHECK \kind, jae, 1
        CHECK \kind, je,  0
        CHECK \kind, jne, 1
        CHECK \kind, jbe, 0
        CHECK \kind, ja,  1
        CHECK \kind, js,  1
        CHECK \kind, jns, 0
        CHECK \kind, jp,  1
        CHECK \kind, jnp, 0
        CHECK \kind, jl,  0
        CHECK \kind, jge, 1
        CHECK \kind, jle, 0
        CHECK \kind, jg,  1
    .endm

    .globl _start
    .type _start, @function
_start:
    xorl %r15d, %r15d

positive_cmp:
    CHECK_CMP_SUB_CF cmp_cf
    CHECK_CMP_SUB_OF cmp_of

positive_test:
    CHECK_TEST_EVEN test_even
    CHECK_TEST_ZERO test_zero

positive_sub:
    CHECK_CMP_SUB_CF sub_cf
    CHECK_CMP_SUB_OF sub_of

positive_add:
    CHECK_ADD_CZ add_cz
    CHECK_ADD_OF add_of

negative_lahf:
    movq $0, %r8
    cmpq $1, %r8
    lahf
    movzbl %ah, %eax
    jb .Llahf_taken
    xorl %r12d, %r12d
    jmp .Llahf_join
.Llahf_taken:
    xorl %r12d, %r12d
    movl $1, %r12d
.Llahf_join:
    xorl $1, %r12d
    /* Rosetta's x86 runner leaves AF clear here; AF is not under test. */
    andl $0xe7, %eax
    xorl $0x87, %eax
    negl %eax
    sbbl %eax, %eax
    andl $2, %eax
    orl %r12d, %r15d
    orl %eax, %r15d

negative_pushf:
    movq $0, %r8
    cmpq $1, %r8
    pushfq
    popq %r10
    jb .Lpushf_taken
    xorl %r12d, %r12d
    jmp .Lpushf_join
.Lpushf_taken:
    xorl %r12d, %r12d
    movl $1, %r12d
.Lpushf_join:
    andl $0x8c5, %r10d
    xorl $0x85, %r10d
    negl %r10d
    sbbl %r10d, %r10d
    andl $4, %r10d
    xorl $1, %r12d
    orl %r10d, %r15d
    orl %r12d, %r15d

negative_setcc:
    movq $0, %r8
    cmpq $1, %r8
    setb %r10b
    movzbl %r10b, %r10d
    jb .Lsetcc_taken
    xorl %r12d, %r12d
    jmp .Lsetcc_join
.Lsetcc_taken:
    xorl %r12d, %r12d
    movl $1, %r12d
.Lsetcc_join:
    xorl $1, %r10d
    xorl $1, %r12d
    orl %r10d, %r15d
    orl %r12d, %r15d

negative_cmov:
    movl $11, %r10d
    movl $22, %r11d
    movq $0, %r8
    cmpq $1, %r8
    cmovbl %r11d, %r10d
    jb .Lcmov_taken
    xorl %r12d, %r12d
    jmp .Lcmov_join
.Lcmov_taken:
    xorl %r12d, %r12d
    movl $1, %r12d
.Lcmov_join:
    xorl $22, %r10d
    xorl $1, %r12d
    orl %r10d, %r15d
    orl %r12d, %r15d

negative_adc:
    movl $5, %r10d
    movq $0, %r8
    cmpq $1, %r8
    adcl $0, %r10d
    je .Ladc_taken
    xorl %r12d, %r12d
    jmp .Ladc_join
.Ladc_taken:
    xorl %r12d, %r12d
    movl $1, %r12d
.Ladc_join:
    xorl $6, %r10d
    /* 6 != 0, so JE above must not be taken. */
    orl %r10d, %r15d
    orl %r12d, %r15d

    .globl cross_flags_live
    .type cross_flags_live, @function
cross_flags_live:
    movl $5, %r13d
    movq $0, %r8
    cmpq $1, %r8
    jb .Lcross_taken
    adcl $0, %r13d
    jmp .Lcross_join
.Lcross_taken:
    adcl $0, %r13d
.Lcross_join:
    xorl $6, %r13d
    orl %r13d, %r15d

    testl %r15d, %r15d
    jne .Lfail
    movl $1, %eax
    movl $1, %edi
    leaq pass_message(%rip), %rsi
    movl $pass_message_len, %edx
    syscall
    xorl %edi, %edi
    jmp .Lexit
.Lfail:
    movl $1, %eax
    movl $1, %edi
    leaq fail_message(%rip), %rsi
    movl $fail_message_len, %edx
    syscall
    movl %r15d, %edi
.Lexit:
    movl $60, %eax
    syscall
    .size _start, . - _start

    .section .note.GNU-stack,"",@progbits
