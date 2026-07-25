// Minimal raw x86_64 Linux x87 throughput benchmark.
//
// The loop deliberately combines register arithmetic, FSQRT, and FXCH.  It
// has no libc dependency and exits through the Linux syscall ABI, so it can
// be timed directly through svm_translator_linux.
    .global _start
    .text
_start:
    fninit
    fld1
    fldl    factor(%rip)
    fxch    %st(1)              // ST0=accumulator, ST1=factor
    mov     $2000000, %ecx
1:
    fadd    %st(1), %st(0)
    fmul    %st(1), %st(0)
    fdiv    %st(1), %st(0)
    fxch    %st(1)
    fxch    %st(1)
    fld1
    fsqrt
    fstp    %st(0)
    dec     %ecx
    jne     1b
    fstp    %st(0)
    fstp    %st(0)
    xor     %edi, %edi
    mov     $231, %eax          // __NR_exit_group
    syscall

    .p2align 3
factor:
    .quad   0x3ff0000000000001  // 1.0 + one binary64 ULP
