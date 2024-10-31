//
// Created by 甘尧 on 2024/4/10.
//
#pragma once

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
#define cache       x27
#define flags       x26
#define rsb_ptr     x25
#define local       x25
#define loc         x24
#define pt          x24
#define args        x23
#define arg         x22
#define handle      x21

#define ip          x11
#define ip0         x16
#define ip1         x17
#define ip2         x14
#define ip3         x15
#define ip4         x11
#define ip5         x12
#define ip6         x9
#define ip7         x10
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

