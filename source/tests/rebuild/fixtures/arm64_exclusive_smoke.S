.arch armv8-a
.text
.global _start
.type _start,%function
_start:
    // A private anonymous page gives the execution owner an exclusive domain.
    mov x0, #0
    mov x1, #4096
    mov x2, #3
    mov x3, #0x22
    mov x4, #-1
    mov x5, #0
    mov x8, #222
    svc #0
    tbnz x0, #63, failure
    mov x19, x0
    mov x1, #36
    str x1, [x19]
    mov x1, #100
    str x1, [x19, #8]

    // Warm the store entry without a reservation. Mixed mode subsequently
    // enters this native entry with a reservation created by the direct path.
    clrex
    mov x6, #99
    bl store_scalar
    cmp w8, #1
    b.ne failure
    ldr x1, [x19]
    cmp x1, #36
    b.ne failure
    mov x9, #4
    mov x10, #1000
scalar_retry:
    ldxr x5, [x19]
    add x6, x5, #1
    bl store_scalar
    cbz w8, scalar_committed
    subs x10, x10, #1
    b.ne scalar_retry
    b failure
scalar_committed:
    subs x9, x9, #1
    b.ne scalar_retry
    ldr x1, [x19]
    cmp x1, #40
    b.ne failure

    mov x10, #1000
pair64_retry:
    ldaxp x1, x2, [x19]
    add x3, x1, #1
    add x4, x2, #1
    stlxp w8, x3, x4, [x19]
    cbz w8, pair64_committed
    subs x10, x10, #1
    b.ne pair64_retry
    b failure
pair64_committed:
    ldp x1, x2, [x19]
    cmp x1, #41
    b.ne failure
    cmp x2, #101
    b.ne failure

    add x20, x19, #16
    mov x3, #1
    movk x3, #2, lsl #32
    str x3, [x20]
    mov x10, #1000
pair32_retry:
    ldxp w1, w2, [x20]
    cmp x1, #1
    b.ne failure
    cmp x2, #2
    b.ne failure
    mov w3, #3
    mov w4, #4
    stxp w8, w3, w4, [x20]
    cbz w8, pair32_committed
    subs x10, x10, #1
    b.ne pair32_retry
    b failure
pair32_committed:
    ldp w1, w2, [x20]
    cmp x1, #3
    b.ne failure
    cmp x2, #4
    b.ne failure

    mov x10, #1000
byte_retry:
    mov x1, #-1
    ldaxrb w1, [x19]
    cmp x1, #41
    b.ne failure
    add w2, w1, #1
    stlxrb w8, w2, [x19]
    cbz w8, byte_committed
    subs x10, x10, #1
    b.ne byte_retry
    b failure
byte_committed:
    mov x10, #1000
half_retry:
    ldxrh w1, [x19]
    cmp x1, #42
    b.ne failure
    add w2, w1, #1
    stxrh w8, w2, [x19]
    cbz w8, half_committed
    subs x10, x10, #1
    b.ne half_retry
    b failure
half_committed:
    mov x10, #1000
word_retry:
    ldaxr w1, [x19]
    cmp x1, #43
    b.ne failure
    add w2, w1, #1
    stlxr w8, w2, [x19]
    cbz w8, word_committed
    subs x10, x10, #1
    b.ne word_retry
    b failure
word_committed:
    // CLREX must make the following store fail, even if memory is unchanged.
    ldxr x1, [x19]
    clrex
    mov x2, #999
    stxr w8, x2, [x19]
    cmp w8, #1
    b.ne failure
    ldr x1, [x19]
    cmp x1, #44
    b.ne failure

    mrs x1, cntfrq_el0
    cbz x1, failure
    mrs x2, cntvct_el0
    mrs x3, cntvct_el0
    cmp x3, x2
    b.cc failure
    mrs x4, ctr_el0
    tbz x4, #31, failure
    adr x0, _start
    dc cvau, x0
    dc cvac, x0
    dc civac, x0
    dsb ish
    ic ivau, x0
    dsb ish
    isb

    mov x0, #1
    adr x1, message
    mov x2, #(message_end-message)
    mov x8, #64
    svc #0
    mov x0, #42
    mov x8, #93
    svc #0
failure:
    mov x0, #125
    mov x8, #93
    svc #0
.size _start,.-_start
.type store_scalar,%function
store_scalar:
    stxr w8, x6, [x19]
    ret
.size store_scalar,.-store_scalar
.section .rodata
message:
    .ascii "arm64 exclusive ok\n"
message_end:
.section .note.GNU-stack,"",%progbits
