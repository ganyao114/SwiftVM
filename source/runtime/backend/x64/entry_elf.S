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
.macro IMPORT_EFLAGS
 cmp qword ptr [r12+SVM_X64_CTX_NATIVE_EFLAGS-SVM_X64_CTX_STATE],0
 je 8f
 cmp qword ptr [r12+SVM_X64_CTX_NATIVE_EFLAGS-SVM_X64_CTX_STATE],2
 jne .Lraw_import\@
 mov rdx,rbx
 shr rdx,16
 and edx,0xc000
 mov rax,rbx
 shr rax,21
 and eax,0x100
 xor eax,0x100
 or edx,eax
 mov rax,rbx
 shr rax,28
 and eax,1
 or edx,eax
 mov qword ptr [r12+SVM_X64_CTX_EFLAGS-SVM_X64_CTX_STATE],rdx
 jmp 8f
.Lraw_import\@:
 pushfq
 pop rdx
 and rdx,-2242
 mov rax,rbx
 shr rax,24
 and eax,0xc0
 or rdx,rax
 mov rax,rbx
 shr rax,29
 and eax,1
 xor eax,1
 or rdx,rax
 mov rax,rbx
 shr rax,17
 and eax,0x800
 or rdx,rax
 mov qword ptr [r12+SVM_X64_CTX_EFLAGS-SVM_X64_CTX_STATE],rdx
8:
.endm
.macro PACK_EFLAGS
 cmp qword ptr [r12+SVM_X64_CTX_NATIVE_EFLAGS-SVM_X64_CTX_STATE],0
 je 8f
 mov rdx,qword ptr [r12+SVM_X64_CTX_EFLAGS-SVM_X64_CTX_STATE]
 cmp qword ptr [r12+SVM_X64_CTX_NATIVE_EFLAGS-SVM_X64_CTX_STATE],2
 jne .Lraw_pack\@
 mov eax,edx
 and eax,0xc000
 shl eax,16
 mov ecx,edx
 and ecx,1
 shl ecx,28
 or eax,ecx
 and edx,0x100
 shl edx,21
 xor edx,0x20000000
 or eax,edx
 jmp .Lpack_merge\@
.Lraw_pack\@:
 mov eax,edx
 and eax,0xc0
 shl eax,24
 mov ecx,edx
 and ecx,0x800
 shl ecx,17
 or eax,ecx
 and edx,1
 shl edx,29
 xor edx,0x20000000
 or eax,edx
.Lpack_merge\@:
 movabs rcx,0xffffffff0fffffff
 and rbx,rcx
 or rbx,rax
8:
.endm
.macro PUBLISH_FLAGS
 mov ecx,dword ptr [r12+SVM_X64_CTX_FLAGS-SVM_X64_CTX_STATE]
 cmp ecx,-1
 je 1f
 mov qword ptr [r12+rcx],rbx
1:
.endm
.macro PUBLISH_PINNED
 mov r10d,dword ptr [r12+SVM_X64_CTX_PIN_OFFSET-SVM_X64_CTX_STATE]
 cmp r10d,-1
 je 7f
 mov qword ptr [r12+r10],r13
 mov qword ptr [r12+r10+8],r14
 mov ecx,dword ptr [r12+SVM_X64_CTX_PIN_SLOTS-SVM_X64_CTX_STATE]
 cmp ecx,2
 je 7f
 mov qword ptr [r12+r10+16],r15
 mov qword ptr [r12+r10+24],rbp
7:
.endm
.macro RELOAD_PINNED
 mov r10d,dword ptr [r12+SVM_X64_CTX_PIN_OFFSET-SVM_X64_CTX_STATE]
 cmp r10d,-1
 je 9f
 mov r13,qword ptr [r12+r10]
 mov r14,qword ptr [r12+r10+8]
 mov ecx,dword ptr [r12+SVM_X64_CTX_PIN_SLOTS-SVM_X64_CTX_STATE]
 cmp ecx,2
 je 9f
 mov r15,qword ptr [r12+r10+16]
 mov rbp,qword ptr [r12+r10+24]
9:
.endm
SVM_FUNCTION SvmRunNative
 mov qword ptr [rdi+SVM_X64_CTX_PHASE],SVM_NATIVE_PHASE_ENTERING
 push rbx
 push rbp
 push r12
 push r13
 push r14
 push r15
 sub rsp,24
 mov qword ptr [rsp],rdi
 mov qword ptr [rdi+SVM_X64_CTX_ROOT_SP],rsp
 stmxcsr dword ptr [rsp+8]
 stmxcsr dword ptr [rdi+SVM_X64_CTX_HOST_MXCSR]
 lea r12,[rdi+SVM_X64_CTX_STATE]
 mov eax,dword ptr [rdi+SVM_X64_CTX_FLAGS]
 xor ebx,ebx
 cmp eax,-1
 je 1f
 mov rbx,qword ptr [r12+rax]
1:
 IMPORT_EFLAGS
 ldmxcsr dword ptr [rdi+SVM_X64_CTX_GUEST_MXCSR]
 RELOAD_PINNED
 mov eax,dword ptr [rdi+SVM_X64_CTX_PC]
 mov rdi,qword ptr [r12+rax]
 cmp qword ptr [r12+SVM_X64_CTX_NATIVE_EFLAGS-SVM_X64_CTX_STATE],0
 jne SvmNativeDispatchNZCV
 jmp SvmNativeDispatch
SVM_END SvmRunNative
.macro DISPATCH name,counted,native
SVM_FUNCTION \name
.if \counted
 add qword ptr [r12+SVM_X64_CTX_DISPATCHES-SVM_X64_CTX_STATE],1
.endif
 mov eax,dword ptr [r12+SVM_X64_CTX_PC-SVM_X64_CTX_STATE]
 mov qword ptr [r12+rax],rdi
.if !\native
 PUBLISH_FLAGS
.endif
 // R8 is only a scratch borrow of this thread's selected L1 slot. A null
 // base retains the bare NativeRunContext callback ABI; VM entries have L1.
 mov r8,qword ptr [r12+SVM_X64_CTX_L1-SVM_X64_CTX_STATE]
 test r8,r8
 jz .Ll2\@
 mov eax,edi
 and eax,(1<<SVM_DISPATCH_L1_BITS)-1
 shl rax,SVM_DISPATCH_L1_ENTRY_SHIFT
 add r8,rax
 .globl \name\()L1Probe
 .hidden \name\()L1Probe
\name\()L1Probe:
 cmp qword ptr [r8],rdi
 jne .Ll2\@
 mov r11,qword ptr [r8+8]
.if \counted
 add qword ptr [r12+SVM_X64_CTX_HITS-SVM_X64_CTX_STATE],1
.endif
 jmp .Lenter\@
.Ll2\@:
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
 mov r11,qword ptr [rcx+8]
 test r11,r11
 jz .Lcold
.if \counted
 add qword ptr [r12+SVM_X64_CTX_HITS-SVM_X64_CTX_STATE],1
.endif
 // PendingCounted cancels the counted tag match. The uncounted root
 // must go cold directly: it has no hit increment for that thunk to cancel.
 // Only ready entries enter L1; reuse R8 rather than selecting/hash again.
 cmp dword ptr [rcx+SVM_DISPATCH_CODE_ID_OFFSET],-1
.if \counted
 je .Lenter\@
.else
 je .Lcold
.endif
 test r8,r8
 jz .Lenter\@
 mov qword ptr [r8+8],r11
 mov qword ptr [r8],rdi
.Lenter\@:
 mov rax,qword ptr [r12+SVM_X64_CTX_ACTIVE-SVM_X64_CTX_STATE]
 mov rcx,qword ptr [r12+SVM_X64_CTX_ANCHOR-SVM_X64_CTX_STATE]
 mov qword ptr [rax],rcx
 mov qword ptr [r12+SVM_X64_CTX_PHASE-SVM_X64_CTX_STATE],SVM_NATIVE_PHASE_GUEST
 jmp r11
SVM_END \name
.endm
DISPATCH SvmNativeDispatch,0,0
DISPATCH SvmNativeDispatchCounted,1,0
DISPATCH SvmNativeDispatchNZCV,0,1
DISPATCH SvmNativeDispatchNZCVCounted,1,1
SVM_FUNCTION SvmNativePendingCounted
 sub qword ptr [r12+SVM_X64_CTX_HITS-SVM_X64_CTX_STATE],1
 jmp .Lcold
SVM_END SvmNativePendingCounted
SVM_FUNCTION SvmNativePending
 jmp .Lcold
SVM_END SvmNativePending
.Lcold:
 PACK_EFLAGS
 PUBLISH_PINNED
 PUBLISH_FLAGS
 mov rax,qword ptr [r12+SVM_X64_CTX_ACTIVE-SVM_X64_CTX_STATE]
 mov qword ptr [rax],0
 stmxcsr dword ptr [r12+SVM_X64_CTX_GUEST_MXCSR-SVM_X64_CTX_STATE]
 ldmxcsr dword ptr [r12+SVM_X64_CTX_HOST_MXCSR-SVM_X64_CTX_STATE]
 mov qword ptr [r12+SVM_X64_CTX_PHASE-SVM_X64_CTX_STATE],SVM_NATIVE_PHASE_RUNTIME
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
 IMPORT_EFLAGS
 RELOAD_PINNED
 test r11,r11
 jz .Lfailed
 mov rax,qword ptr [r12+SVM_X64_CTX_ACTIVE-SVM_X64_CTX_STATE]
 mov rcx,qword ptr [r12+SVM_X64_CTX_ANCHOR-SVM_X64_CTX_STATE]
 mov qword ptr [rax],rcx
 mov qword ptr [r12+SVM_X64_CTX_PHASE-SVM_X64_CTX_STATE],SVM_NATIVE_PHASE_GUEST
 jmp r11
.Lfailed:
 mov eax,SVM_NATIVE_EXIT_RESOLVE_FAILURE
 jmp SvmNativeExit
SVM_FUNCTION SvmNativeExit
 mov qword ptr [r12+SVM_X64_CTX_PHASE-SVM_X64_CTX_STATE],SVM_NATIVE_PHASE_LEAVING
 PUBLISH_PINNED
 PUBLISH_FLAGS
 stmxcsr dword ptr [r12+SVM_X64_CTX_GUEST_MXCSR-SVM_X64_CTX_STATE]
 jmp .Lrestore_host_root
SVM_END SvmNativeExit
SVM_FUNCTION SvmNativeEventExit
 lea r12,[rdi+SVM_X64_CTX_STATE]
 mov qword ptr [r12+SVM_X64_CTX_PHASE-SVM_X64_CTX_STATE],SVM_NATIVE_PHASE_LEAVING
 PUBLISH_PINNED
 mov eax,SVM_NATIVE_EXIT_GUEST_EVENTS
 jmp .Lrestore_host_root
SVM_END SvmNativeEventExit
SVM_FUNCTION SvmNativeExceptionExit
 lea r12,[rdi+SVM_X64_CTX_STATE]
 mov qword ptr [r12+SVM_X64_CTX_PHASE-SVM_X64_CTX_STATE],SVM_NATIVE_PHASE_LEAVING
 mov eax,SVM_NATIVE_EXIT_GUEST_EXCEPTION
.Lrestore_host_root:
 mov rsp,qword ptr [r12+SVM_X64_CTX_ROOT_SP-SVM_X64_CTX_STATE]
 mov rcx,qword ptr [r12+SVM_X64_CTX_ACTIVE-SVM_X64_CTX_STATE]
 mov qword ptr [rcx],0
 ldmxcsr dword ptr [rsp+8]
 mov qword ptr [r12+SVM_X64_CTX_ROOT_SP-SVM_X64_CTX_STATE],0
 mov qword ptr [r12+SVM_X64_CTX_PHASE-SVM_X64_CTX_STATE],SVM_NATIVE_PHASE_RUNTIME
 add rsp,24
 pop r15
 pop r14
 pop r13
 pop r12
 pop rbp
 pop rbx
 ret
SVM_END SvmNativeExceptionExit
.section .note.GNU-stack,"",@progbits
#endif
