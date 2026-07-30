// Emits AESKEYGENASSIST(key, 1) verbatim to nail its four output dword lanes.
.intel_syntax noprefix
.global _start
.section .text
_start:
    movdqu xmm0, XMMWORD PTR [rip + key]
    aeskeygenassist xmm0, xmm0, 1
    movdqu XMMWORD PTR [rip + out], xmm0
    mov eax, 1
    mov edi, 1
    lea rsi, [rip + out]
    mov edx, 16
    syscall
    mov eax, 60
    xor edi, edi
    syscall
.section .rodata
.align 16
key: .byte 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
.section .data
.align 16
out: .zero 16
