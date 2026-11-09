// Linux AArch32 kuser ABI. Bodies are ordinary guest code, decoded through
// the same frontend; the runtime only installs this read/execute page.
.syntax unified
.arch armv7-a
.arm
.text
.global svm_arm32_kuser_start
svm_arm32_kuser_start:
.org 0x100
.type cmpxchg,%function
cmpxchg:
    dmb ish
1:  ldrex r3, [r2]
    subs r3, r3, r0
    bne 2f
    strex ip, r1, [r2]
    cmp ip, #0
    bne 1b
2:  clrex
    dmb ish
    rsbs r0, r3, #0
    bx lr
.size cmpxchg,.-cmpxchg
.org 0x180
.type cmpxchg64,%function
cmpxchg64:
    push {r4-r7, lr}
    ldrd r4, r5, [r0]
    ldrd r6, r7, [r1]
    dmb ish
1:  ldrexd r0, r1, [r2]
    cmp r0, r4
    cmpeq r1, r5
    bne 2f
    strexd r3, r6, r7, [r2]
    cmp r3, #0
    bne 1b
    mov r0, #0
    b 3f
2:  clrex
    mov r0, #1
3:  dmb ish
    rsbs r0, r0, #0
    pop {r4-r7, pc}
.size cmpxchg64,.-cmpxchg64
.org 0xf60
.type entry_cmpxchg64,%function
entry_cmpxchg64:
    b cmpxchg64
.size entry_cmpxchg64,.-entry_cmpxchg64
.org 0xfa0
.type entry_barrier,%function
entry_barrier:
    dmb ish
    bx lr
.size entry_barrier,.-entry_barrier
.org 0xfc0
.type entry_cmpxchg,%function
entry_cmpxchg:
    b cmpxchg
.size entry_cmpxchg,.-entry_cmpxchg
.org 0xfe0
.type entry_tls,%function
entry_tls:
    mrc p15, 0, r0, c13, c0, 3
    bx lr
.size entry_tls,.-entry_tls
.org 0xffc
.type helper_version,%object
helper_version:
    .word 5

.size helper_version,.-helper_version
