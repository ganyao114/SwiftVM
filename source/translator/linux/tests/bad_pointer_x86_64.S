// Minimal statically-linked x86_64 Linux bad-pointer test for SwiftVM.
// Loads from an unmapped guest address; the host signal handler (Phase 3)
// must convert the resulting host SIGSEGV into a guest PageFatal halt
// instead of crashing the host process. If the load ever "succeeds", the
// program exits 42 (a test failure signal).
    .global _start
    .text
_start:
    movabs  $0x414141414141, %rax  // ~71TB: unmapped in any host layout
    mov     (%rax), %rax        // wild load -> guest page fault
    mov     $42, %edi           // exit status (must not be reached)
    mov     $231, %eax          // __NR_exit_group
    syscall
