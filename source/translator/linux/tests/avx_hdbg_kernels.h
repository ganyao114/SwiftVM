//
// avx_hdbg_kernels.h -- bisecting harness for the K0 `fdot` mismatch, and the
// standing regression test for what it found.
//
// WHAT IT FOUND
// -------------
// NOT the horizontal family.  vhaddps/vextractf128/vaddps/vextractps all agree
// with x86-64 hardware bit for bit here, including the dst==src1==src2 shape
// clang emits in k_fdot.  The wrong answer comes from ADDRESS GENERATION:
//
//     a SIB memory operand with an index and a scale but NO base register
//     (ModRM.mod=00, SIB.base=101, address in the disp32) is computed as
//         (index + disp) << log2(scale)
//     instead of
//         (index << log2(scale)) + disp
//
// on the legacy/distorm address path.  k_fdot is the only kernel in
// avx_real_x86_64 that uses that form -- 8 sites, all of them in its dot-product
// loop -- so its `vmovups (,%rax,4), %ymm1` loads read a wildly wrong address,
// return zeros, and the whole accumulator comes out +0.0.  Every later step then
// operates correctly on zeros, which is why the failure looked like a horizontal
// bug: 0638edb1822d2cd5 is exactly the k_fdot hash of an all-zero acc/t/sum.
//
// The VEX handlers' own VexAddress() (decoder_avx_fp.cc) computes the same form
// CORRECTLY, which is why `vaddps 32(,%rax,4), %ymm1, %ymm0` agrees while
// `vmovups 32(,%rax,4), %ymm0` does not -- decoder_avx_fp.cc deliberately
// declines vmovups/vmovupd at 0F 10/11, so those take the legacy path.
//
// LAYOUT
// ------
//   section 0  addressing-mode probes -- the defect, plus the controls that
//              localise it (base register present / scale 1 / VEX handler path)
//   section 1  the horizontal family on constants that distinguish every lane
//              permutation, in all three register-aliasing shapes
//   section 2  k_fdot's real accumulator and its reduction, step by step
//   section 3  the same reduction on data-independent constants
//
// Guest shim: avx_hdbg_x86_64.c   Oracle shim: avx_hdbg_host.c
// Driver:     build_avx_hdbg.sh   (diffs SwiftVM against real x86-64)
//
#ifndef SVM_AVX_HDBG_KERNELS_H
#define SVM_AVX_HDBG_KERNELS_H

typedef unsigned int u32;
typedef unsigned long long u64;

void svm_write(const char *buf, unsigned long len);
void svm_exit(int code);

static void emit_str(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    svm_write(s, n);
}

// name=xxxxxxxx xxxxxxxx ... (n 32-bit lanes, lane 0 first)
static void emit_lanes(const char *name, const u32 *v, int n) {
    char buf[256];
    unsigned long i = 0;
    while (name[i]) { buf[i] = name[i]; i++; }
    buf[i++] = '=';
    for (int k = 0; k < n; k++) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            buf[i++] = "0123456789abcdef"[(v[k] >> shift) & 0xF];
        }
        buf[i++] = (k + 1 == n) ? '\n' : ' ';
    }
    svm_write(buf, i);
}

// ---------------------------------------------------------------------------
// Primitives.  Inline asm, so the encoding is fixed here and identical in the
// guest and in the oracle.  AT&T order is `op src2, src1, dst`.
// ---------------------------------------------------------------------------

// vhaddps ymm2, ymm1, ymm0  -- three distinct registers.
static void hadd256_3reg(const u32 *a, const u32 *b, u32 *out) {
    __asm__ volatile("vmovups (%0), %%ymm0\n\t"
                     "vmovups (%1), %%ymm1\n\t"
                     "vhaddps %%ymm1, %%ymm0, %%ymm2\n\t"
                     "vmovups %%ymm2, (%2)\n\t"
                     "vzeroupper\n\t"
                     :
                     : "r"(a), "r"(b), "r"(out)
                     : "memory", "ymm0", "ymm1", "ymm2");
}

// vhaddps ymm0, ymm0, ymm0  -- exactly what clang emits in k_fdot.
static void hadd256_same(const u32 *a, u32 *out) {
    __asm__ volatile("vmovups (%0), %%ymm0\n\t"
                     "vhaddps %%ymm0, %%ymm0, %%ymm0\n\t"
                     "vmovups %%ymm0, (%1)\n\t"
                     "vzeroupper\n\t"
                     :
                     : "r"(a), "r"(out)
                     : "memory", "ymm0");
}

// vhaddps ymm1, ymm0, ymm0  (dst != src, src1 == src2) -- the other k_fdot form.
static void hadd256_srcsame_dstdiff(const u32 *a, u32 *out) {
    __asm__ volatile("vmovups (%0), %%ymm0\n\t"
                     "vhaddps %%ymm0, %%ymm0, %%ymm1\n\t"
                     "vmovups %%ymm1, (%1)\n\t"
                     "vzeroupper\n\t"
                     :
                     : "r"(a), "r"(out)
                     : "memory", "ymm0", "ymm1");
}

// vhaddps xmm2, xmm1, xmm0 -- VEX.128 control for the same data.
static void hadd128_3reg(const u32 *a, const u32 *b, u32 *out) {
    __asm__ volatile("vmovups (%0), %%xmm0\n\t"
                     "vmovups (%1), %%xmm1\n\t"
                     "vhaddps %%xmm1, %%xmm0, %%xmm2\n\t"
                     "vmovups %%xmm2, (%1)\n\t"
                     "vmovups %%xmm2, (%2)\n\t"
                     :
                     : "r"(a), "r"(b), "r"(out)
                     : "memory", "xmm0", "xmm1", "xmm2");
}

// vextractf128 $1, ymm0, xmm1
static void extractf128_hi(const u32 *a, u32 *out) {
    __asm__ volatile("vmovups (%0), %%ymm0\n\t"
                     "vextractf128 $1, %%ymm0, %%xmm1\n\t"
                     "vmovups %%xmm1, (%1)\n\t"
                     "vzeroupper\n\t"
                     :
                     : "r"(a), "r"(out)
                     : "memory", "ymm0", "xmm1");
}

// vaddps xmm2, xmm1, xmm0 then vextractps $0, xmm2, m32 -- k_fdot's tail.
static void addps_extractps(const u32 *a, const u32 *b, u32 *out) {
    __asm__ volatile("vmovups (%0), %%xmm1\n\t"
                     "vmovups (%1), %%xmm0\n\t"
                     "vaddps %%xmm0, %%xmm1, %%xmm2\n\t"
                     "vmovups %%xmm2, (%2)\n\t"
                     "vextractps $0, %%xmm2, 16(%2)\n\t"
                     "vextractps $2, %%xmm2, 20(%2)\n\t"
                     :
                     : "r"(a), "r"(b), "r"(out)
                     : "memory", "xmm0", "xmm1", "xmm2");
}

// The whole k_fdot tail, expressed exactly as clang lowered it, so the four
// steps can be watched together on one input.
static void fdot_tail(const u32 *acc, u32 *out) {
    __asm__ volatile("vmovups (%0), %%ymm0\n\t"
                     "vhaddps %%ymm0, %%ymm0, %%ymm0\n\t"      // t1
                     "vmovups %%ymm0, 0(%1)\n\t"
                     "vhaddps %%ymm0, %%ymm0, %%ymm1\n\t"      // t2
                     "vmovups %%ymm1, 32(%1)\n\t"
                     "vextractf128 $1, %%ymm1, %%xmm0\n\t"     // hi
                     "vmovups %%xmm0, 64(%1)\n\t"
                     "vaddps %%xmm0, %%xmm1, %%xmm2\n\t"       // sum
                     "vmovups %%xmm2, 80(%1)\n\t"
                     "vextractps $0, %%xmm2, 96(%1)\n\t"
                     "vzeroupper\n\t"
                     :
                     : "r"(acc), "r"(out)
                     : "memory", "ymm0", "ymm1", "xmm2");
}

// ---------------------------------------------------------------------------
// The real acc: byte-for-byte the accumulation half of k_fdot.
// ---------------------------------------------------------------------------
#define NDOT 4096
static float hdbg_a[NDOT], hdbg_b[NDOT];
static u64 rngs;
static u64 rng_next(void) {
    rngs ^= rngs << 13;
    rngs ^= rngs >> 7;
    rngs ^= rngs << 17;
    return rngs;
}

// Written with plain scalar C so the accumulate step cannot itself be the
// suspect: it is compiled to vmulps/vaddps by -mavx2 -O2 in both builds, and
// K2/K3 already prove vmulps/vaddps agree.
static void build_acc(u32 *acc_out) {
    rngs = 0x243f6a8885a308d3ULL;
    for (int i = 0; i < NDOT; i++) {
        hdbg_a[i] = (float)(int)(rng_next() % 2001 - 1000) * 0.125f;
        hdbg_b[i] = (float)(int)(rng_next() % 2001 - 1000) * 0.03125f;
    }
    // Read the freshly-filled arrays back through a volatile pointer: that
    // forces a plain base-register load, so a difference between THIS and the
    // vectorised loop below is a load-addressing difference, not a store one.
    {
        static u32 probe[8];
        volatile float *pa = hdbg_a, *pb = hdbg_b;
        union { float f; u32 u; } c;
        for (int k = 0; k < 4; k++) { c.f = pa[k]; probe[k] = c.u; }
        for (int k = 0; k < 4; k++) { c.f = pb[k]; probe[4 + k] = c.u; }
        emit_lanes("probe_a0_3_b0_3", probe, 8);
        for (int k = 0; k < 4; k++) { c.f = pa[4088 + k]; probe[k] = c.u; }
        for (int k = 0; k < 4; k++) { c.f = pb[4088 + k]; probe[4 + k] = c.u; }
        emit_lanes("probe_tail", probe, 8);
        u32 rl[2] = {(u32)rngs, (u32)(rngs >> 32)};
        emit_lanes("rng_final", rl, 2);
    }
    float acc[8];
    for (int k = 0; k < 8; k++) acc[k] = 0.0f;
    for (int i = 0; i < NDOT; i += 8) {
        for (int k = 0; k < 8; k++) acc[k] = acc[k] + hdbg_a[i + k] * hdbg_b[i + k];
    }
    for (int k = 0; k < 8; k++) {
        union { float f; u32 u; } c;
        c.f = acc[k];
        acc_out[k] = c.u;
    }
}

// ---------------------------------------------------------------------------
// Addressing-mode probes -- the actual defect this harness found.
//
// clang's -fno-pic lowering of `arr[i]` with a 64-bit index is a SIB byte with
// an index and scale but NO BASE, the array address living in the disp32
// (ModRM.mod=00, SIB.base=101).  k_fdot's inner loop is the only place in
// avx_real_x86_64 that uses it -- 8 sites, all in k_fdot.
//
// A fixed absolute disp32 cannot be used on macOS (everything is PIC and
// MAP_FIXED in the low 4 GiB is refused), so the probes below put a SMALL
// number in the disp32 and carry the address in the INDEX register instead:
//
//     movl 4(,%rax,4), %edx      with rax = p/4 - 1
//
// architecturally reads *p, because EA = index*scale + disp = (p-4) + 4.  Any
// implementation that instead computes (index + disp) * scale reads
// (p/4 - 1 + 4) * 4 = p + 12.  Both addresses are inside the caller's array, so
// the probe is safe and -- unlike an absolute disp32 -- runs on the oracle too.
// ---------------------------------------------------------------------------

// movl 4(,%rax,4), %edx        -- no base, scale 4, disp32 = 4
static void sib_nobase_mov32(const u32 *p, u32 *out) {
    long idx = (long)((unsigned long)p / 4) - 1;
    __asm__ volatile("movl 4(,%[i],4), %%edx\n\t"
                     "movl %%edx, (%[o])\n\t"
                     :
                     : [i] "r"(idx), [o] "r"(out)
                     : "memory", "rdx");
}

// movl 4(%rbx,%rax,4), %edx    -- same EA, but WITH a base register (rbx = 0)
static void sib_base_mov32(const u32 *p, u32 *out) {
    long idx = (long)((unsigned long)p / 4) - 1;
    __asm__ volatile("xor %%rbx, %%rbx\n\t"
                     "movl 4(%%rbx,%[i],4), %%edx\n\t"
                     "movl %%edx, (%[o])\n\t"
                     :
                     : [i] "r"(idx), [o] "r"(out)
                     : "memory", "rdx", "rbx");
}

// movl 4(,%rax,1), %edx        -- scale 1 control: no shift, so no bug
static void sib_nobase_mov32_s1(const u32 *p, u32 *out) {
    long idx = (long)((unsigned long)p) - 4;
    __asm__ volatile("movl 4(,%[i],1), %%edx\n\t"
                     "movl %%edx, (%[o])\n\t"
                     :
                     : [i] "r"(idx), [o] "r"(out)
                     : "memory", "rdx");
}

// vmovups 32(,%rax,4), %ymm0   -- the exact form k_fdot's loads use.
// Goes through the LEGACY distorm address path (decoder_avx_fp.cc explicitly
// declines vmovups/vmovupd at 0F 10/11).
static void sib_nobase_vmovups(const u32 *p, u32 *out) {
    long idx = (long)(((unsigned long)p - 32) / 4);
    __asm__ volatile("vmovups 32(,%[i],4), %%ymm0\n\t"
                     "vmovups %%ymm0, (%[o])\n\t"
                     "vzeroupper\n\t"
                     :
                     : [i] "r"(idx), [o] "r"(out)
                     : "memory", "ymm0");
}

// vaddps 32(,%rax,4), %ymm1, %ymm0 with ymm1 = 0 -- same addressing form, but
// this one goes through the VEX handler's own VexAddress (decoder_avx_fp.cc),
// which computes the address correctly.  The pair is the differential that
// says WHICH address path is at fault.
static void sib_nobase_vaddps(const u32 *p, u32 *out) {
    long idx = (long)(((unsigned long)p - 32) / 4);
    __asm__ volatile("vxorps %%ymm1, %%ymm1, %%ymm1\n\t"
                     "vaddps 32(,%[i],4), %%ymm1, %%ymm0\n\t"
                     "vmovups %%ymm0, (%[o])\n\t"
                     "vzeroupper\n\t"
                     :
                     : [i] "r"(idx), [o] "r"(out)
                     : "memory", "ymm0", "ymm1");
}

// movups 32(,%rax,4), %xmm0    -- legacy SSE, same form
static void sib_nobase_movups128(const u32 *p, u32 *out) {
    long idx = (long)(((unsigned long)p - 32) / 4);
    __asm__ volatile("movups 32(,%[i],4), %%xmm0\n\t"
                     "movups %%xmm0, (%[o])\n\t"
                     :
                     : [i] "r"(idx), [o] "r"(out)
                     : "memory", "xmm0");
}

static u32 sibsrc[64] __attribute__((aligned(32)));

static u32 obuf[64];

static int svm_hdbg_run(void) {
    // ---- 0. addressing-mode probes -----------------------------------------
    for (int i = 0; i < 64; i++) sibsrc[i] = 0xa0000000u + (u32)i;
    sib_nobase_mov32(sibsrc, obuf);
    emit_lanes("sib_nobase_mov32", obuf, 1);
    sib_base_mov32(sibsrc, obuf);
    emit_lanes("sib_base_mov32", obuf, 1);
    sib_nobase_mov32_s1(sibsrc, obuf);
    emit_lanes("sib_nobase_mov32_s1", obuf, 1);
    sib_nobase_vmovups(sibsrc, obuf);
    emit_lanes("sib_nobase_vmovups", obuf, 8);
    sib_nobase_vaddps(sibsrc, obuf);
    emit_lanes("sib_nobase_vaddps", obuf, 8);
    sib_nobase_movups128(sibsrc, obuf);
    emit_lanes("sib_nobase_movups128", obuf, 4);

    // ---- 1. synthetic, distinguishes every possible lane permutation -------
    // a = 1,2,4,...,128 ; b = 256,512,...,32768 -- as floats.
    static u32 pa[8], pb[8];
    for (int i = 0; i < 8; i++) {
        union { float f; u32 u; } ca, cb;
        ca.f = (float)(1 << i);
        cb.f = (float)(256 << i);
        pa[i] = ca.u;
        pb[i] = cb.u;
    }
    hadd256_3reg(pa, pb, obuf);
    emit_lanes("syn_h256_3reg", obuf, 8);
    hadd256_same(pa, obuf);
    emit_lanes("syn_h256_same", obuf, 8);
    hadd256_srcsame_dstdiff(pa, obuf);
    emit_lanes("syn_h256_srcsame", obuf, 8);
    hadd128_3reg(pa, pb, obuf);
    emit_lanes("syn_h128_3reg", obuf, 4);
    extractf128_hi(pa, obuf);
    emit_lanes("syn_ext128", obuf, 4);
    addps_extractps(pa, pb, obuf);
    emit_lanes("syn_addps", obuf, 6);

    // ---- 2. the real acc, then the real tail, step by step -----------------
    static u32 acc[8];
    build_acc(acc);
    emit_lanes("acc", acc, 8);

    fdot_tail(acc, obuf);
    emit_lanes("t1", obuf + 0, 8);
    emit_lanes("t2", obuf + 8, 8);
    emit_lanes("hi", obuf + 16, 4);
    emit_lanes("sum", obuf + 20, 4);
    emit_lanes("scalar", obuf + 24, 1);

    // ---- 3. the same tail on the synthetic vector (data-independent) -------
    fdot_tail(pa, obuf);
    emit_lanes("syn_t1", obuf + 0, 8);
    emit_lanes("syn_t2", obuf + 8, 8);
    emit_lanes("syn_hi", obuf + 16, 4);
    emit_lanes("syn_sum", obuf + 20, 4);
    emit_lanes("syn_scalar", obuf + 24, 1);
    return 0;
}

#endif  // SVM_AVX_HDBG_KERNELS_H
