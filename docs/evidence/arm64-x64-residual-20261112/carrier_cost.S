.intel_syntax noprefix
.text
.macro FN name
.globl \name
.type \name,@function
.p2align 6
\name:
.endm
.macro ADMIT reg, okl, slowl
 movq rax, \reg
 add rax, rax
 je \okl
 shr rax, 53
 sub eax, 513
 cmp eax, 1020
 ja \slowl
.endm

FN k_base
 mov rcx, rdi
 xor edx, edx
1: add rdx, 3
 sub rcx, 1
 jne 1b
 ret

FN k_pushf_save
 sub rsp, 24
 mov rcx, rdi
 xor edx, edx
1: add rdx, 3
 sub rcx, 1
 pushfq
 pop r11
 mov [rsp], r11
 jne 1b
 add rsp, 24
 ret

FN k_pushf_popf
 sub rsp, 24
 mov rcx, rdi
 xor edx, edx
1: sub rcx, 1
 pushfq
 pop r11
 mov [rsp], r11
 add rdx, 3
 push qword ptr [rsp]
 popfq
 jne 1b
 add rsp, 24
 ret

FN k_lahf_save
 sub rsp, 24
 mov rcx, rdi
 xor edx, edx
1: add rdx, 3
 sub rcx, 1
 lahf
 seto al
 mov [rsp], eax
 jne 1b
 add rsp, 24
 ret

FN k_lahf_sahf
 sub rsp, 24
 mov rcx, rdi
 xor edx, edx
1: sub rcx, 1
 lahf
 seto al
 mov [rsp], eax
 add rdx, 3
 mov eax, [rsp]
 add al, 0x7f
 sahf
 jne 1b
 add rsp, 24
 ret

FN k_setcc_test
 mov rcx, rdi
 xor edx, edx
1: sub rcx, 1
 setne r11b
 add rdx, 3
 test r11b, r11b
 jne 1b
 ret

FN k_recompare
 mov rcx, rdi
 xor edx, edx
1: sub rcx, 1
 add rdx, 3
 test rcx, rcx
 jne 1b
 ret

FN k_fp_plain
 mov rcx, rdi
 movsd xmm0, [rip+one]
 movsd xmm1, [rip+step]
1: addsd xmm0, xmm1
 sub rcx, 1
 jne 1b
 ret

FN k_fp_nan
 mov rcx, rdi
 movsd xmm0, [rip+one]
 movsd xmm1, [rip+step]
1: addsd xmm0, xmm1
 ucomisd xmm0, xmm0
 jp 2f
 sub rcx, 1
 jne 1b
2: ret

FN k_fp_post
 mov rcx, rdi
 movsd xmm0, [rip+one]
 movsd xmm1, [rip+step]
1: addsd xmm0, xmm1
 ucomisd xmm0, xmm0
 jp 2f
 movq rax, xmm0
 add rax, rax
 ror rax, 53
 dec rax
 jz 2f
 sub rcx, 1
 jne 1b
2: ret

FN k_fp_admit
 mov rcx, rdi
 movsd xmm0, [rip+one]
 movsd xmm1, [rip+step]
1: ADMIT xmm0, 3f, 2f
3: ADMIT xmm1, 4f, 2f
4: addsd xmm0, xmm1
 sub rcx, 1
 jne 1b
2: ret

FN k_fp_region
 sub rsp, 24
 mov dword ptr [rsp+8], 0
 mov rcx, rdi
 movsd xmm0, [rip+one]
 movsd xmm1, [rip+step]
1: stmxcsr [rsp]
 mov dword ptr [rsp+4], 0x1f80
 ldmxcsr [rsp+4]
 ADMIT xmm0, 3f, 2f
3: ADMIT xmm1, 4f, 2f
4: addsd xmm0, xmm1
 stmxcsr [rsp+4]
 ldmxcsr [rsp]
 mov r11d, [rsp+4]
 shr r11d, 1
 and r11d, 16
 or [rsp+8], r11d
 sub rcx, 1
 jne 1b
2: add rsp, 24
 ret

FN k_vec_admit
 mov rcx, rdi
 movapd xmm0, [rip+vone]
 movapd xmm1, [rip+vstep]
1: ADMIT xmm0, 3f, 2f
3: movdqa xmm14, xmm0
 psrldq xmm14, 8
 ADMIT xmm14, 4f, 2f
4: ADMIT xmm1, 5f, 2f
5: movdqa xmm14, xmm1
 psrldq xmm14, 8
 ADMIT xmm14, 6f, 2f
6: addpd xmm0, xmm1
 sub rcx, 1
 jne 1b
2: ret

FN k_vec_post
 mov rcx, rdi
 movapd xmm0, [rip+vone]
 movapd xmm1, [rip+vstep]
 movdqa xmm12, [rip+vabs]
 movdqa xmm13, [rip+vmin]
1: addpd xmm0, xmm1
 movdqa xmm14, xmm0
 pand xmm14, xmm12
 pcmpeqq xmm14, xmm13
 movapd xmm15, xmm0
 cmpunordpd xmm15, xmm15
 por xmm14, xmm15
 ptest xmm14, xmm14
 jnz 2f
 sub rcx, 1
 jne 1b
2: ret

.section .rodata
.p2align 4
vone: .double 1.0, 1.0
vstep: .double 1.5, 1.5
vabs: .quad 0x7fffffffffffffff, 0x7fffffffffffffff
vmin: .quad 0x0010000000000000, 0x0010000000000000
one: .double 1.0
step: .double 1.5
.section .note.GNU-stack,"",@progbits
