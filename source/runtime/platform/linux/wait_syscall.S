// A normal host-ABI call. Cancellation only changes PC inside Begin..Complete;
// both exits restore this stub's frame and return through the C++ caller.
.text
.globl SvmHostWaitSyscall
.hidden SvmHostWaitSyscall
.globl SvmHostWaitBegin
.hidden SvmHostWaitBegin
.globl SvmHostWaitComplete
.hidden SvmHostWaitComplete
.globl SvmHostWaitCancelled
.hidden SvmHostWaitCancelled

#if defined(__x86_64__)
.intel_syntax noprefix
.p2align 4
.type SvmHostWaitSyscall,@function
SvmHostWaitSyscall:
push rbx
mov rbx,rdi
SvmHostWaitBegin:
mov eax,14
mov edi,2
lea rsi,[rbx+56]
xor edx,edx
mov r10d,8
syscall
test rax,rax
js SvmHostWaitComplete
mov rax,qword ptr [rbx]
mov rdi,qword ptr [rbx+8]
mov rsi,qword ptr [rbx+16]
mov rdx,qword ptr [rbx+24]
mov r10,qword ptr [rbx+32]
mov r8,qword ptr [rbx+40]
mov r9,qword ptr [rbx+48]
syscall
SvmHostWaitComplete:
pop rbx
ret
SvmHostWaitCancelled:
mov rax,-4096
jmp SvmHostWaitComplete
.size SvmHostWaitSyscall,.-SvmHostWaitSyscall

#elif defined(__aarch64__)
.p2align 2
.type SvmHostWaitSyscall,%function
SvmHostWaitSyscall:
stp x19,x30,[sp,#-16]!
mov x19,x0
SvmHostWaitBegin:
mov x8,#135
mov x0,#2
add x1,x19,#56
mov x2,xzr
mov x3,#8
svc #0
tbnz x0,#63,SvmHostWaitComplete
ldr x8,[x19]
ldp x0,x1,[x19,#8]
ldp x2,x3,[x19,#24]
ldp x4,x5,[x19,#40]
svc #0
SvmHostWaitComplete:
ldp x19,x30,[sp],#16
ret
SvmHostWaitCancelled:
mov x0,#-4096
b SvmHostWaitComplete
.size SvmHostWaitSyscall,.-SvmHostWaitSyscall

#elif defined(__riscv) && __riscv_xlen == 64
.option push
.option norvc
.option norelax
.p2align 2
.type SvmHostWaitSyscall,@function
SvmHostWaitSyscall:
addi sp,sp,-16
sd s0,0(sp)
mv s0,a0
SvmHostWaitBegin:
li a7,135
li a0,2
addi a1,s0,56
li a2,0
li a3,8
ecall
bltz a0,SvmHostWaitComplete
ld a7,0(s0)
ld a0,8(s0)
ld a1,16(s0)
ld a2,24(s0)
ld a3,32(s0)
ld a4,40(s0)
ld a5,48(s0)
ecall
SvmHostWaitComplete:
ld s0,0(sp)
addi sp,sp,16
ret
SvmHostWaitCancelled:
li a0,-4096
j SvmHostWaitComplete
.size SvmHostWaitSyscall,.-SvmHostWaitSyscall
.option pop

#elif defined(__loongarch64)
.p2align 2
.type SvmHostWaitSyscall,@function
SvmHostWaitSyscall:
addi.d $sp,$sp,-16
st.d $r23,$sp,0
or $r23,$a0,$zero
SvmHostWaitBegin:
li.d $a7,135
li.d $a0,2
addi.d $a1,$r23,56
or $a2,$zero,$zero
li.d $a3,8
syscall 0
blt $a0,$zero,SvmHostWaitComplete
ld.d $a7,$r23,0
ld.d $a0,$r23,8
ld.d $a1,$r23,16
ld.d $a2,$r23,24
ld.d $a3,$r23,32
ld.d $a4,$r23,40
ld.d $a5,$r23,48
syscall 0
SvmHostWaitComplete:
ld.d $r23,$sp,0
addi.d $sp,$sp,16
jirl $zero,$ra,0
SvmHostWaitCancelled:
li.d $a0,-4096
b SvmHostWaitComplete
.size SvmHostWaitSyscall,.-SvmHostWaitSyscall
#endif

.section .note.GNU-stack,"",@progbits
