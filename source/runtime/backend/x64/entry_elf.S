#if defined(__x86_64__)
#include "abi_layout.inc"
#include "runtime/backend/dispatch_layout.inc"
#include "runtime/exit.inc"
.intel_syntax noprefix
.text
.macro SVM_FUNCTION name
.p2align 4
.globl \name
.hidden \name
.type \name,@function
\name:
.endm
.macro SVM_END name
.size \name,.-\name
.endm
.macro PUBLISH_FLAGS
 mov ecx,dword ptr [r12+SVM_X64_CTX_FLAGS-SVM_X64_CTX_STATE]
 cmp ecx,-1
 je 1f
 mov qword ptr [r12+rcx],rbx
1:
.endm
SVM_FUNCTION SvmRunNative
 push rbx
 push rbp
 push r12
 push r13
 push r14
 push r15
 sub rsp,24
 mov qword ptr [rsp],rdi
 stmxcsr dword ptr [rsp+8]
 stmxcsr dword ptr [rdi+SVM_X64_CTX_HOST_MXCSR]
 lea r12,[rdi+SVM_X64_CTX_STATE]
 mov eax,dword ptr [rdi+SVM_X64_CTX_FLAGS]
 xor ebx,ebx
 cmp eax,-1
 je 1f
 mov rbx,qword ptr [r12+rax]
1:
 ldmxcsr dword ptr [rdi+SVM_X64_CTX_GUEST_MXCSR]
 mov eax,dword ptr [rdi+SVM_X64_CTX_PC]
 mov rdi,qword ptr [r12+rax]
 jmp SvmNativeDispatch
SVM_END SvmRunNative
.macro DISPATCH name,counted
SVM_FUNCTION \name
.if \counted
 add qword ptr [r12+SVM_X64_CTX_DISPATCHES-SVM_X64_CTX_STATE],1
.endif
 mov eax,dword ptr [r12+SVM_X64_CTX_PC-SVM_X64_CTX_STATE]
 mov qword ptr [r12+rax],rdi
 PUBLISH_FLAGS
 mov rsi,qword ptr [r12+SVM_X64_CTX_SLOTS-SVM_X64_CTX_STATE]
 mov rdx,qword ptr [r12+SVM_X64_CTX_MASK-SVM_X64_CTX_STATE]
 mov rax,rdi
 shr rax,SVM_DISPATCH_HASH_SHIFT
 xor rax,rdi
 and rax,rdx
.Lprobe\@:
 mov rcx,rax
 shl rcx,SVM_DISPATCH_SLOT_SHIFT
 add rcx,rsi
.if \counted
 add qword ptr [r12+SVM_X64_CTX_PROBES-SVM_X64_CTX_STATE],1
.endif
 cmp byte ptr [rcx+SVM_DISPATCH_OCCUPIED_OFFSET],0
 je .Lcold
 cmp qword ptr [rcx],rdi
 je .Lready\@
 add rax,1
 and rax,rdx
 jmp .Lprobe\@
.Lready\@:
.if \counted
 add qword ptr [r12+SVM_X64_CTX_HITS-SVM_X64_CTX_STATE],1
.endif
 mov r11,qword ptr [rcx+8]
 test r11,r11
 jz .Lcold
 mov rax,qword ptr [r12+SVM_X64_CTX_ACTIVE-SVM_X64_CTX_STATE]
 mov rcx,qword ptr [r12+SVM_X64_CTX_ANCHOR-SVM_X64_CTX_STATE]
 mov qword ptr [rax],rcx
 jmp r11
SVM_END \name
.endm
DISPATCH SvmNativeDispatch,0
DISPATCH SvmNativeDispatchCounted,1
.globl SvmNativeDispatchNZCV
.set SvmNativeDispatchNZCV,SvmNativeDispatch
.globl SvmNativeDispatchNZCVCounted
.set SvmNativeDispatchNZCVCounted,SvmNativeDispatchCounted
SVM_FUNCTION SvmNativePendingCounted
 sub qword ptr [r12+SVM_X64_CTX_HITS-SVM_X64_CTX_STATE],1
 jmp .Lcold
SVM_END SvmNativePendingCounted
SVM_FUNCTION SvmNativePending
 jmp .Lcold
SVM_END SvmNativePending
.Lcold:
 PUBLISH_FLAGS
 mov rax,qword ptr [r12+SVM_X64_CTX_ACTIVE-SVM_X64_CTX_STATE]
 mov qword ptr [rax],0
 ldmxcsr dword ptr [r12+SVM_X64_CTX_HOST_MXCSR-SVM_X64_CTX_STATE]
 lea rsi,[r12-SVM_X64_CTX_STATE]
 mov rdi,qword ptr [rsi]
 call qword ptr [rsi+8]
 mov r11,rax
 ldmxcsr dword ptr [r12+SVM_X64_CTX_GUEST_MXCSR-SVM_X64_CTX_STATE]
 mov eax,dword ptr [r12+SVM_X64_CTX_FLAGS-SVM_X64_CTX_STATE]
 cmp eax,-1
 je 1f
 mov rbx,qword ptr [r12+rax]
1:
 test r11,r11
 jz .Lfailed
 mov rax,qword ptr [r12+SVM_X64_CTX_ACTIVE-SVM_X64_CTX_STATE]
 mov rcx,qword ptr [r12+SVM_X64_CTX_ANCHOR-SVM_X64_CTX_STATE]
 mov qword ptr [rax],rcx
 jmp r11
.Lfailed:
 mov eax,SVM_NATIVE_EXIT_RESOLVE_FAILURE
 jmp SvmNativeExit
SVM_FUNCTION SvmNativeExit
 PUBLISH_FLAGS
 ldmxcsr dword ptr [rsp+8]
 add rsp,24
 pop r15
 pop r14
 pop r13
 pop r12
 pop rbp
 pop rbx
 ret
SVM_END SvmNativeExit
.section .note.GNU-stack,"",@progbits
#endif
