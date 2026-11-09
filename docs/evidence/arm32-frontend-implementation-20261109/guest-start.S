.syntax unified
.arch armv7-a
.arm
.global _start
.type _start,%function
_start:
    mov fp, #0
    bl guest_main
    mov r7, #1
    svc 0
    udf 0
