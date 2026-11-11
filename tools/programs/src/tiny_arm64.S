.global _start
.text
_start:
    mov x0, #1
    adr x1, msg
    mov x2, #16
    mov x8, #64
    svc #0
    mov x0, #42
    mov x8, #93
    svc #0
.data
msg: .ascii "tiny-aarch64-ok\n"
