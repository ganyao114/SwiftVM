#include "marcos.h"

save_host_callee:
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x28, [sp, #-16]!

    stp q8, q9,   [sp, #-32]!
    stp q10, q11, [sp, #-32]!
    stp q12, q13, [sp, #-32]!
    stp q14, q15, [sp, #-32]!
    ret

restore_host_callee:
    ldp q14, q15, [sp], #32
    ldp q12, q13, [sp], #32
    ldp q10, q11, [sp], #32
    ldp q8, q9,   [sp], #32

    ldp x27, x28, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ret

.def swift_runtime_entry
    stp x29, x30, [sp, #-16]!
    bl save_host_callee

    mov state, x0
    mov args, x1
    // load cache_ptr
    ldr cache, [state, #OFF_L2_CACHE]
    // load pt
    ldr pt, [state, #OFF_PT]
    // load rsb
    ldr rsb_ptr, [state, #OFF_RSB_PTR]
    cbnz args, go_guest

.def swift_code_dispatcher
code_dispatcher:

#define loc_index ip0
#define l1_cache  ip1
#define l1_index  ip2
#define l1_start  ip3
#define l2_cache  cache
#define l2_index  ip4
#define l2_start  ip5

    // align loc
    ldr loc, [state, #OFF_LOC]
    lsr loc_index, loc, 2

    // query l1 cache
    ldr l1_cache, [state, #OFF_L1_CACHE]
    eor l1_index, loc_index, loc_index, lsr #L1_CODE_CACHE_BITS
    and l1_index, l1_index, #L1_CODE_CACHE_HASH
    add l1_start, l1_cache, l1_index, lsl #4

1:  // l1 cache looper
    ldr l1_index, [l1_start], #0x10
    cbz l1_index, 2f
    sub l1_index, l1_index, loc
    cbnz l1_index, 1b
    ldr args, [l1_start, #-0x8]
    tbnz arg, 63, go_interp
    blr args

2:  // query l2 cache
    eor l2_index, loc_index, loc_index, lsr #L2_CODE_CACHE_BITS
    and l2_index, l2_index, #L2_CODE_CACHE_HASH
    add l2_start, l2_cache, l2_index, lsl #4

3:  // l2 looper
    ldr l2_index, [l2_start], #0x10
    cbz l2_index, return_host
    sub l2_index, l2_index, loc
    cbnz l2_index, 3b
    ldr args, [l2_start, #-0x8]
    cbz args, return_host

    // write to l1 cache
    ldr l2_index, [l1_start, #-0x8]
    add l2_index, l2_index, #1
    cbz l2_index, go_guest // check if l1 cache is full
    stp loc, args, [l1_start, #-0x10]

#undef loc_index
#undef l1_cache
#undef l1_index
#undef l1_start
#undef l2_cache
#undef l2_index
#undef l2_start

go_guest:
    tbnz arg, 63, go_interp
go_interp:
    bfc args, 63, 1
jump_guest:
    blr args

.def swift_return_host
return_host:
    // write back rsb
    str rsb_ptr, [state, #OFF_RSB_PTR]
    // load excption
    ldr w0, [state, #OFF_EXCEPTION]
    str wzr, [state, #OFF_EXCEPTION]
    bl restore_host_callee
    ldp x29, x30, [sp], #16
    ret

.def swift_check_halt
    ldr w0, [state, #OFF_EXCEPTION]
    cbnz w0, return_host
    next_instr