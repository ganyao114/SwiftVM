// Self-modifying code (SMC) test for SwiftVM (Phase 4).
//
// Executes getval (JIT-translates it with imm 42), then patches the imm32
// of its `mov $42, %eax` twice — 42 -> 99 -> 7 — with byte stores into a
// page holding translated code. Each store must be detected (write-protect
// fault), the stale translation invalidated and re-translated from the
// patched bytes, so the three calls return 42, 99 and 7. The second patch
// also proves the page was re-protected when the replacement translation
// was registered.
//
// getval lives in its own 16KB-aligned page so the patching store and the
// patched code sit in different host pages (macOS host page = 16KB),
// exercising page-granular tracking without invalidating the patcher.
//
// Exit codes: 99 = success (42, then 99, then 7)
//              1 = first call wrong (baseline broken)
//              2 = second call wrong (first patch not detected)
//              3 = third call wrong (second patch not detected /
//                 re-protection after retranslation broken)
    .global _start
    .text
_start:
    lea     back1(%rip), %rcx
    jmp     getval
back1:
    mov     %eax, %ebx          // save first result (expect 42)
    lea     getval(%rip), %rax
    movb    $0x63, 1(%rax)      // patch imm32 byte: 42 (0x2a) -> 99 (0x63)
    lea     back2(%rip), %rcx
    jmp     getval
back2:
    cmp     $42, %ebx
    jne     fail1
    cmp     $99, %eax
    jne     fail2
    lea     getval(%rip), %rax
    movb    $0x07, 1(%rax)      // patch again: 99 (0x63) -> 7 (0x07)
    lea     back3(%rip), %rcx
    jmp     getval
back3:
    cmp     $7, %eax
    jne     fail3
    mov     $99, %edi           // success
    mov     $231, %eax          // __NR_exit_group
    syscall
fail1:
    mov     $1, %edi
    mov     $231, %eax
    syscall
fail2:
    mov     $2, %edi
    mov     $231, %eax
    syscall
fail3:
    mov     $3, %edi
    mov     $231, %eax
    syscall

    .p2align 14                 // getval on its own 16KB page
getval:
    mov     $42, %eax           // B8 2A 00 00 00 -- imm32 at offset +1
    jmp     *%rcx
