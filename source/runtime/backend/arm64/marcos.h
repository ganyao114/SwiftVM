//
// Created by 甘尧 on 2023/3/12.
//

#pragma once

#include "defines.h"

#if defined(__APPLE__)
#define cdecl(s) _##s
#else
#define cdecl(s) s
#endif

#define ifin(thing, ...) _ifin(thing, __COUNTER__, __VA_ARGS__)
#define _ifin(thing, line, ...) __ifin(thing, line, __VA_ARGS__)
#define __ifin(thing, line, ...) irp da_op##line, __VA_ARGS__ N .ifc thing,\da_op##line
#define endifin endif N .endr

.macro .def name
    .global cdecl(\name)
    .align 4
    cdecl(\name):
.endm

// common operations
#define load_next       ldp arg, handle, [args], #16

.macro goto_next
    br handle
.endm

.macro go_link
    blr handle
.endm

.macro next_instr
    load_next
    goto_next
.endm

.macro save_caller_regs
    stp x29, x30, [sp, #-16]!
.endm

.macro restore_caller_regs
	ldp x29, x30, [sp], #16
.endm

#define scale_imm4(x)  ubfx x, arg, 16, 4
#define extract(x, p, s)  ubfx x, arg, p, s
#define extract64(x, p, s)  ubfx x, arg_x, p, s

#define load_b_arg(x)   ldrb x, [args], #1
#define load_h_arg(x)   ldrh x, [args], #2
#define load_w_arg(x)   ldr x, [args], #4
#define load_x_arg(x)   ldr x, [args], #8
#define push_x_arg(x)   str x, [args, #-8]!

#define load_guest_sp(x) ldr x, [ctx, #256]

#define COND_LIST eq, ne, cs, cc, mi, pl, vs, vc, hi, ls, ge, lt, gt, le, nv, al
#define MEM_OPS b, h, w, x, sb, sh, sw
#define LDR_OPS b, h, w, x, sb, sh, sw
#define STR_OPS b, h, w, x
#define VMEM_OPS b, h, s, d, q
#define ADDR_MODES no_offset, offset, post_index, pre_index
#define BOOL_LIST true, false