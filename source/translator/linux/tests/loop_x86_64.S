// x86_64 guest exercising loops, conditional branches, div and memory:
// sums 1..100 (= 5050), prints it as decimal via repeated div by 10,
// then exit_group(sum & 0xff).
    .global _start
    .text
_start:
    xor     %eax, %eax          // sum = 0
    mov     $1, %ecx            // i = 1
1:
    add     %ecx, %eax
    inc     %ecx
    cmp     $101, %ecx
    jne     1b
    mov     %eax, %ebx          // save sum for the exit code

    // Convert eax to decimal, digits written backwards into buf.
    lea     buf+16(%rip), %rsi
    mov     $10, %r8d
2:
    xor     %edx, %edx
    div     %r8d                // eax = quot, edx = rem
    add     $'0', %dl
    dec     %rsi
    mov     %dl, (%rsi)
    test    %eax, %eax
    jne     2b
    movb    $'\n', buf+16(%rip)

    // write(1, rsi, buf+17 - rsi)
    lea     buf+17(%rip), %rdx
    sub     %rsi, %rdx
    mov     $1, %edi
    mov     $1, %eax            // __NR_write
    syscall

    mov     %ebx, %edi          // exit status = sum & 0xff
    mov     $231, %eax          // __NR_exit_group
    syscall

buf:
    .space 17
