//
// Created by 甘尧 on 2023/3/12.
//

#pragma once

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

#define L1_CODE_CACHE_BITS 18
#define L1_CODE_CACHE_HASH 0x3FFFF
#define L2_CODE_CACHE_BITS 23
#define L2_CODE_CACHE_HASH 0x7FFFFF

#define PAGE_BITS               12
#define PAGE_MASK               4095
#define VALID_PAGE_INDEX_BITS   20

// ctx offsets

#define OFF_L1_CACHE        0
#define OFF_L2_CACHE        8
#define OFF_PT              16
#define OFF_INTERFACE       24
#define OFF_EXCEPTION       32
#define OFF_RSB_PTR         72
#define OFF_LOC             80

// regs
#define state       x28
#define local       x27
#define pt          x26
#define cache       x25
#define rsb_ptr     x24
#define args        x23
#define arg         x22
#define handle      x21
#define loc         x20

#define ip          x11
#define ip0         x16
#define ip1         x17
#define ip2         x12
#define ip3         x13
#define ip4         x14
#define ip5         x15
#define ipw         w11
#define ipw0        w16
#define ipw1        w17
#define ipw2        w12
#define ipw3        w13
#define ipw4        w14
#define ipw5        w15
#define tmp0        w3
#define tmp1        w2
#define tmp2        w1
#define tmp3        w0
#define tmpx0       x3
#define tmpx1       x2
#define tmpx2       x1
#define tmpx3       x0
#define ips0        s11
#define ips1        s12
#define ips2        s13
#define ips3        s14
#define ipd0        d11
#define ipd1        d12
#define ipd2        d13
#define ipd3        d14
#define ipv0        v11
#define ipv1        v12
#define ipv2        v13
#define ipv3        v14
#define ipb0        b11
#define iph0        h11
#define iph1        h12
#define iph2        h13
#define iph3        h14
#define ipq0        q11
#define ipq1        q12

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