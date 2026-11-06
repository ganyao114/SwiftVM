// Freestanding x86-64 runtime-service fixture. Exit 0 means all checks passed;
// the small nonzero exit codes identify the failing architectural operation.
.intel_syntax noprefix
.global _start
.text
_start:
    sub rsp, 4096
    and rsp, -64
    vmovdqu ymm0, [rip + vector]
pi_instruction:
    fldpi
    mov byte ptr [rsp + 464], 0x7b
    fxsave64 [rsp]
    cmp byte ptr [rsp + 464], 0x7b
    jne fail1
    lea rax, [rip + pi_instruction]
    cmp qword ptr [rsp + 8], rax
    jne fail2
    pxor xmm0, xmm0
    fninit
    mov dword ptr [rsp + 960], 0x5f80
    ldmxcsr [rsp + 960]
    fxrstor64 [rsp]
    stmxcsr [rsp + 964]
    cmp dword ptr [rsp + 964], 0x1f80
    jne fail3
    vpcmpeqb ymm2, ymm0, [rip + vector]
    vpmovmskb eax, ymm2
    cmp eax, -1
    jne fail4
    cld
    lea rdi, [rsp + 1024]
    mov ecx, 104
    xor eax, eax
    rep stosq
    mov eax, 7
    xor edx, edx
    xsave64 [rsp + 1024]
    vxorps ymm0, ymm0, ymm0
    fninit
    ldmxcsr [rsp + 960]
    mov eax, 7
    xor edx, edx
    xrstor64 [rsp + 1024]
    vpcmpeqb ymm2, ymm0, [rip + vector]
    vpmovmskb eax, ymm2
    cmp eax, -1
    jne fail5
    stmxcsr [rsp + 964]
    cmp dword ptr [rsp + 964], 0x1f80
    jne fail6
    fstp tbyte ptr [rsp + 2048]
    movabs rax, 0xc90fdaa22168c235
    cmp qword ptr [rsp + 2048], rax
    jne fail7
    cmp word ptr [rsp + 2056], 0x4000
    jne fail7
    // All three supported components are non-INIT. Compact saves replace the
    // old bitmaps but preserve the rest of the header and software bytes.
    fldpi
    mov qword ptr [rsp + 1536], -1
    mov qword ptr [rsp + 1544], -1
    mov byte ptr [rsp + 1552], 0x5a
    mov byte ptr [rsp + 1488], 0x7b
    mov eax, -1
    mov edx, -1
    xsavec64 [rsp + 1024]
    cmp qword ptr [rsp + 1536], 7
    jne fail16
    movabs rax, 0x8000000000000007
    cmp qword ptr [rsp + 1544], rax
    jne fail16
    cmp byte ptr [rsp + 1552], 0x5a
    jne fail16
    cmp byte ptr [rsp + 1488], 0x7b
    jne fail16
    mov byte ptr [rsp + 1552], 0
    vxorps ymm0, ymm0, ymm0
    fninit
    ldmxcsr [rsp + 960]
    mov eax, 7
    xor edx, edx
    xrstor64 [rsp + 1024]
    vpcmpeqb ymm2, ymm0, [rip + vector]
    vpmovmskb eax, ymm2
    cmp eax, -1
    jne fail17
    stmxcsr [rsp + 964]
    cmp dword ptr [rsp + 964], 0x1f80
    jne fail17
    fstp tbyte ptr [rsp + 2048]
    movabs rax, 0xc90fdaa22168c235
    cmp qword ptr [rsp + 2048], rax
    jne fail17
    cmp word ptr [rsp + 2056], 0x4000
    jne fail17
    // AVX-only compacted state neither saves nor restores MXCSR/XMM. Poison
    // that payload so an accidental standard-format access is observable.
    mov dword ptr [rsp + 1048], -1
    mov byte ptr [rsp + 1184], 0xa5
    mov eax, 4
    mov edx, -1
    xsavec [rsp + 1024]
    cmp qword ptr [rsp + 1536], 4
    jne fail18
    movabs rax, 0x8000000000000004
    cmp qword ptr [rsp + 1544], rax
    jne fail18
    cmp dword ptr [rsp + 1048], -1
    jne fail18
    cmp byte ptr [rsp + 1184], 0xa5
    jne fail18
    vxorps ymm0, ymm0, ymm0
    ldmxcsr [rsp + 960]
    mov eax, 4
    xor edx, edx
    xrstor64 [rsp + 1024]
    vpcmpeqb ymm2, ymm0, [rip + vector_high_only]
    vpmovmskb eax, ymm2
    cmp eax, -1
    jne fail18
    stmxcsr [rsp + 964]
    cmp dword ptr [rsp + 964], 0x5f80
    jne fail18
    // Mask zero still replaces both header words. A requested component not
    // present in that compacted image restores INIT without reading payload.
    xor eax, eax
    mov edx, -1
    xsavec64 [rsp + 1024]
    cmp qword ptr [rsp + 1536], 0
    jne fail19
    movabs rax, 0x8000000000000000
    cmp qword ptr [rsp + 1544], rax
    jne fail19
    movdqu xmm0, [rip + vector]
    mov eax, 2
    xor edx, edx
    xrstor64 [rsp + 1024]
    vpcmpeqb ymm2, ymm0, [rip + vector_high_only]
    vpmovmskb eax, ymm2
    cmp eax, -1
    jne fail19
    stmxcsr [rsp + 964]
    cmp dword ptr [rsp + 964], 0x1f80
    jne fail19
    xor ecx, ecx
    xgetbv
    cmp eax, 7
    jne fail8
    test edx, edx
    jne fail8
    mov eax, 0x33445566
    wrfsbase rax
    rdfsbase rbx
    cmp rax, rbx
    jne fail9
    rdtsc
    shl rdx, 32
    or rax, rdx
    mov r12, rax
    rdtscp
    shl rdx, 32
    or rax, rdx
    cmp rax, r12
    jb fail10
    rdrand rax
    jnc fail11
    rdseed dx
    jnc fail11
    lea rsi, [rsp + 2048]
    movabs rax, 0x1122334455667788
    mov qword ptr [rsi], rax
    movabs rax, 0xdeadbeef55667788
    movabs rdx, 0xfacefeed11223344
    mov ebx, 0x12345678
    mov ecx, 0xabcdef01
    lock cmpxchg8b [rsi]
    jne fail12
    movabs r12, 0xdeadbeef55667788
    cmp rax, r12
    jne fail12
    movabs r12, 0xfacefeed11223344
    cmp rdx, r12
    jne fail12
    lock cmpxchg8b [rsi]
    je fail13
    cmp rax, rbx
    jne fail13
    cmp rdx, rcx
    jne fail13
    movabs rax, 0x0123456789abcdef
    movabs rdx, 0xfedcba9876543210
    mov qword ptr [rsi], rax
    mov qword ptr [rsi + 8], rdx
    movabs rbx, 0x5555555555555555
    movabs rcx, 0xaaaaaaaaaaaaaaaa
    lock cmpxchg16b [rsi]
    jne fail14
    cmp qword ptr [rsi], rbx
    jne fail14
    cmp qword ptr [rsi + 8], rcx
    jne fail14
    lock cmpxchg16b [rsi]
    je fail15
    cmp rax, rbx
    jne fail15
    cmp rdx, rcx
    jne fail15
    mfence
    lfence
    pause
    clflush [rsi]
    xor edi, edi
exit:
    mov eax, 60
    syscall
.macro failure number
fail\number:
    mov edi, \number
    jmp exit
.endm
failure 1
failure 2
failure 3
failure 4
failure 5
failure 6
failure 7
failure 8
failure 9
failure 10
failure 11
failure 12
failure 13
failure 14
failure 15
failure 16
failure 17
failure 18
failure 19
.section .rodata
.balign 32
vector:
    .quad 0x0123456789abcdef, 0xfedcba9876543210
    .quad 0x1020304050607080, 0x8070605040302010
vector_high_only:
    .quad 0, 0
    .quad 0x1020304050607080, 0x8070605040302010
.section .note.GNU-stack,"",@progbits
