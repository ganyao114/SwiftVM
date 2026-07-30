// Same FIPS-197 AES-128 vector, but derives each key with Intel's canonical
// AESKEYGENASSIST + PSHUFD + three-byte-shift expansion sequence.
.intel_syntax noprefix
.macro NEXT_KEY rcon
    aeskeygenassist xmm2, xmm1, \rcon
    pshufd xmm2, xmm2, 0xff
    movdqa xmm3, xmm1
    pslldq xmm3, 4
    pxor xmm1, xmm3
    pslldq xmm3, 4
    pxor xmm1, xmm3
    pslldq xmm3, 4
    pxor xmm1, xmm3
    pxor xmm1, xmm2
.endm
.global _start
.section .text
_start:
    movdqu xmm0, XMMWORD PTR [rip + pt]
    movdqu xmm1, XMMWORD PTR [rip + key]
    pxor xmm0, xmm1
    NEXT_KEY 0x01
    aesenc xmm0, xmm1
    NEXT_KEY 0x02
    aesenc xmm0, xmm1
    NEXT_KEY 0x04
    aesenc xmm0, xmm1
    NEXT_KEY 0x08
    aesenc xmm0, xmm1
    NEXT_KEY 0x10
    aesenc xmm0, xmm1
    NEXT_KEY 0x20
    aesenc xmm0, xmm1
    NEXT_KEY 0x40
    aesenc xmm0, xmm1
    NEXT_KEY 0x80
    aesenc xmm0, xmm1
    NEXT_KEY 0x1b
    aesenc xmm0, xmm1
    NEXT_KEY 0x36
    aesenclast xmm0, xmm1
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
pt:  .byte 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
key: .byte 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
.section .data
.align 16
out: .zero 16
