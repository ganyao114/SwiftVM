// SSE4.2 string compare: PCMPISTRI / PCMPISTRM / PCMPESTRI / PCMPESTRM and
// their VEX twins VPCMPISTRI / VPCMPISTRM / VPCMPESTRI / VPCMPESTRM.
//
// WHY THIS FILE EXISTS
// --------------------
// These four (eight with the VEX forms) are the whole remaining content of
// SSE4.2 beyond POPCNT and CRC32, which are already implemented.  They are the
// path glibc's ifunc dispatcher picks for strlen / strcmp / strstr / strchr /
// strspn on any CPU that advertises SSE4.2, and pcmpistri alone is the single
// highest-frequency unimplemented mnemonic in the instruction census (302
// occurrences).  Until this file existed the family was left ENTIRELY
// unimplemented on purpose -- decoder_sse4.cc's header says why, and the
// reason was right: an undecodable instruction kills the guest loudly with
// IllegalCode, whereas a half-right pcmpistri hands strlen a wrong index and
// the guest silently reads the wrong memory.  So the family is done whole or
// not at all, which is what this file does.
//
// ---------------------------------------------------------------------------
// INTEGRATION -- decoder.h and decoder.cc belong to the main line
// ---------------------------------------------------------------------------
// (1) decoder.h, private section of X64Decoder (append; nothing is reordered):
//
//         // ---- SSE4.2 string compare (decoder_sse42str.cc) --------------
//         // Two entry points, one per encoding family.  Both DECLINE (return
//         // false) rather than approximate anything they do not model.
//         bool DecodeSse42Str(_DInst& insn);          // legacy 66 0F 3A 60..63
//         bool DecodeSse42StrVex(const VexInsn& v);   // VEX.128.66.0F3A 60..63
//         // Shared body: `reg1` is the xmm holding the first operand, `src2`
//         // the already-loaded second operand, `imm8` the control byte, and
//         // `wide` selects RAX/RDX over EAX/EDX for the explicit lengths.
//         void DecodeSse42StrBody(_RegisterType reg1, ir::Value src2, u8 imm8,
//                                 bool explicit_length, bool wide, bool mask_form,
//                                 bool vex);
//         ir::Value Sse42StrLength(ir::Value raw, u32 elements);
//         void Sse42StrFlags(ir::Value packed);
//
// (2) decoder.cc -- TWO one-line changes, both to existing fall-through
//     chains, so neither can conflict with another agent adding a `case`:
//
//     (2a) X64Decoder::DecodeSwitch, the switch's `default:` arm (which
//          decoder_sse4.cc already owns):
//
//         -        default:
//         -            return DecodeSse4(insn);
//         +        default:
//         +            return DecodeSse4(insn) || DecodeSse42Str(insn);
//
//     (2b) the VEX dispatch chain (decoder.cc, in Decode()'s VEX arm):
//
//         -                      DecodeAvxGather(vex) || DecodeAvxMisc(vex)))) {
//         +                      DecodeAvxGather(vex) || DecodeAvxMisc(vex) ||
//         +                      DecodeSse42StrVex(vex)))) {
//
// (3) source/runtime/frontend/x86/CMakeLists.txt: add `decoder_sse42str.cc`.
//     source/tests/CMakeLists.txt: add `fuzz/sse42str_test.cpp`.
//
// (4) NO new IR opcode.  See "WHY THIS FILE ADDS NO IR OPCODE" below.
//
// (5) CPUID.  This file deliberately does NOT turn the bit on -- that is the
//     main line's call.  It does export the gate the main line needs so the
//     advertisement can never outrun the decoder:
//     X64Decoder::Sse42StrEnabled(), declared next to Sse4Enabled().
//
//     decoder_misc.cc, X64Decoder::DecodeCpuid -- ONE added line:
//
//         const u32 leaf1_ecx = kLeaf1Ecx | (Sse4Enabled() ? kSse4Ecx : 0u) |
//     +                         (Sse42StrEnabled() ? (1u << 20) : 0u) |  // SSE4.2
//                               (XsaveEnabled() ? ((1u << 26) | (1u << 27)) : 0u) |
//                               (avx_reported ? ((1u << 28) | (1u << 12)) : 0u);
//
//     NOT folded into kSse4Ecx: that constant is gated on SVM_SSE4, and a
//     build with SVM_SSE4=1 SVM_SSE42STR=0 would then advertise SSE4.2 while
//     this file declines every one of its four instructions.
//
//     SSE4.2's advertised contents are POPCNT (implemented), CRC32
//     (implemented) and these four, so bit 20 is now fully backed.  It must go
//     in together with bits 0/9/19/23 (SSE3/SSSE3/SSE4.1/POPCNT), which
//     decoder_sse4.cc already delivers: a guest that sees SSE4.2 without
//     SSE4.1 sees a CPU that has never existed.
//
//     One test asserts the bit is currently OFF and must move with it:
//     source/tests/fuzz/x86_fuzz.cpp, the CPUID leaf-1 case --
//         -  REQUIRE((sig[2] & (1u << 20)) == 0);  // no SSE4.2 (pcmpXstrY missing)
//         +  REQUIRE(((sig[2] >> 20) & 1u) == (sse42str_on ? 1u : 0u));  // SSE4.2
//     with `sse42str_on` read from SVM_SSE42STR the way `sse4_on` is read
//     from SVM_SSE4 a few lines above.  That file belongs to another agent.
//
// ---------------------------------------------------------------------------
// THE SEMANTICS, AS THE SDM DEFINES THEM (Vol 2B, 4.1)
// ---------------------------------------------------------------------------
// Every one of the four computes the same thing and differs only in where the
// lengths come from (imm8-independent) and where the answer goes:
//
//   arr1 = the FIRST operand   (ModRM.reg, always an xmm register)
//   arr2 = the SECOND operand  (ModRM.rm, xmm or m128)
//   n    = 16 (bytes) or 8 (words), from imm8[0]
//   len1, len2  = number of VALID elements in arr1 / arr2:
//       implicit forms (PCMPISTR*): index of the first zero element, else n
//       explicit forms (PCMPESTR*): EAX/RAX and EDX/RDX read as SIGNED, then
//           ABSOLUTE VALUE, then saturated at n.  RAX = len1, RDX = len2.
//
//   BoolRes[i][j] over i in arr1, j in arr2:
//       imm8[3:2] == 01 (ranges): even i -> arr2[j] >= arr1[i]
//                                 odd  i -> arr2[j] <= arr1[i]
//       everything else:          arr1[i] == arr2[j]
//       comparisons are SIGNED when imm8[1] is set.
//
//   The validity override (SDM's "valid/invalid override of comparisons"),
//   applied to every (i, j) BEFORE aggregation.  This table is the part that
//   makes strcmp and strstr work and the part most likely to be wrong:
//
//       arr1[i]   arr2[j]   equal any   ranges   equal each   equal ordered
//       invalid   invalid   false       false    TRUE         TRUE
//       invalid   valid     false       false    false        TRUE
//       valid     invalid   false       false    false        false
//       valid     valid     compare     compare  compare      compare
//
//   The two middle rows are NOT symmetric and the asymmetry is the whole
//   point of "equal ordered": arr1 is the NEEDLE, so running off ITS end
//   (invalid arr1) means the needle matched to completion and the cell must
//   be TRUE, while running off the HAYSTACK's end (invalid arr2) means the
//   text ran out mid-needle and the cell must be FALSE.  Getting these two the
//   wrong way round was the first cut of this file; hardware caught it on the
//   `nul5_10` input (needle "ABCDE" at offset 0 of "ABCDEFGHIJ") where the
//   wrong table reports NO match and hardware reports one at index 0.
//
//   Aggregation, imm8[3:2], producing the n-bit IntRes1:
//       00 equal any     IntRes1[j] = OR  over all i of M[i][j]
//       01 ranges        IntRes1[j] = OR  over even i of (M[i][j] AND M[i+1][j])
//       10 equal each    IntRes1[j] = M[j][j]
//       11 equal ordered IntRes1[j] = AND over i in [0, n-1-j] of M[i][j+i]
//
//   Polarity, imm8[5:4], producing IntRes2:
//       00, 10  IntRes2 = IntRes1
//       01      IntRes2 = IntRes1 XOR (all n bits)
//       11      IntRes2 = IntRes1 XOR (the len2 VALID bits only)
//
//   Output:
//       index forms  ECX = imm8[6] ? most : least significant set bit of
//                          IntRes2, or n when IntRes2 == 0.  (ECX, so bits
//                          63:32 of RCX are zeroed, even under REX.W.)
//       mask forms   XMM0 = imm8[6] ? IntRes2 expanded to per-element
//                          all-ones/all-zeros : IntRes2 zero-extended to 128.
//
//   Flags -- all six are DEFINED, none is undefined:
//       CF = IntRes2 != 0      ZF = len2 < n      SF = len1 < n
//       OF = IntRes2[0]        AF = 0             PF = 0
//
// ---------------------------------------------------------------------------
// WHY THIS FILE ADDS NO IR OPCODE
// ---------------------------------------------------------------------------
// The aggregation is a 16x16 boolean matrix reduced by one of four different
// functions selected by an immediate, with a four-case validity override and
// six flag outputs.  That is not a vector primitive in any ISA -- it is a tiny
// interpreter, and an IR opcode for it would be an x86 opcode wearing an IR
// hat: no other front end would ever emit it and no back end could do anything
// with it but call the same helper.  So it goes through CallLambda, exactly as
// PHMINPOSUW does in decoder_sse4.cc and as the pshufb / haddps / psadbw class
// already does in decoder_sse.cc.
//
// Everything that IS naturally a vector operation stays in IR: the expansion
// of IntRes2 into the byte/word mask XMM0 wants is VecTableLookup8 + VecAnd +
// VecCmpEq, and the explicit-length absolute value and saturation is five
// scalar ops.  Both are visible in the IR rather than buried in host C.
//
// ---------------------------------------------------------------------------
// WHY TWO HOST CALLS AND NOT ONE
// ---------------------------------------------------------------------------
// CallLambda carries at most THREE Values (ir.inc: `INST(CallLambda, Value,
// Lambda, Value, Value, Value)`, and Inst::max_args caps the rest).  This
// operation needs five: two 64-bit halves of each 128-bit operand plus a
// control word.  So the first call hands over the first operand and the
// control word, and the second call -- which TAKES THE FIRST CALL'S RESULT AS
// AN ARGUMENT -- hands over the second operand and does the work.
//
// The dependency is not decoration.  It is a genuine def-use edge, so no pass
// and no back end can reorder the two or drop the first: DCE already refuses
// to remove CallLambda (instr.cpp: HasSideEffects lists it explicitly), and
// UniformEliminationPass already treats it as a full barrier, but relying on
// that would make correctness a property of today's pass list rather than of
// the IR.  Eval ignores the token's VALUE; it exists only to be an edge.
//
// The handover slot is `thread_local`, so two guest threads cannot collide.
// Within one thread the two calls are adjacent instructions of one guest
// instruction's IR, with no block boundary and no interrupt check between
// them, so nothing can run in the window.
//
// The alternative -- reading the operands out of ThreadContext64 through
// GetUniformAddress, the way the x87 and XSAVE helpers do -- was rejected: it
// works for the register form of the second operand but not the MEMORY form,
// which is the form strlen actually uses, because a helper that dereferences a
// guest address through a raw host pointer takes its page fault at a host PC
// AddressSpace::LookupFault does not recognize and kills the host process
// instead of raising PageFatal (the same reasoning decoder.h records for
// DecodeAvxGather).  Loading the operand with IR and passing the halves keeps
// the fault on the IR memory path where it belongs.
//
// ---------------------------------------------------------------------------
// COST, MEASURED -- AND WHAT IT MEANS FOR CPUID
// ---------------------------------------------------------------------------
// A JIT'd loop of one instruction plus `dec ebx; jnz`, 3M iterations, Apple
// M4 Max, Release, measured three times on a host busy with other builds so
// the spread is wide and these are ORDERS OF MAGNITUDE, not baselines:
//
//     empty loop                              1.4 - 3.1 ns/iteration
//     paddb xmm1, xmm2   (pure IR, reference) 1.4 - 2.4
//     pcmpeqb + pmovmskb (the SSE2 strlen pair)  1.4 - 12
//     pcmpistri xmm1, xmm2, 0x1a               27 - 116
//     pcmpistri xmm1, [rdi], 0x1a              71 - 170
//     pcmpistrm xmm1, xmm2, 0x4c              130 - 180
//
// The two host calls are NOT what costs: `paddb xmm1, [rdi]` measures the same
// as its register form, so the memory path is free, and a host call in this
// runtime is single-digit nanoseconds since 64a48d9.  What costs is the
// helper's O(n^2) matrix -- 256 cells through a lambda for the byte formats --
// which is why "equal ordered" and the wider imm8 values sit at the top of the
// range.  Anything worth optimizing is inside Sse42StrEval, not around it.
//
// THE CONSEQUENCE FOR CPUID BIT 20 IS NOT PURELY A WIN.  Two different guests
// are affected in opposite directions:
//
//   * A binary built for x86-64-v2 (`-msse4.2`, the default floor for a lot of
//     shipping software) emits pcmpistri UNCONDITIONALLY, without consulting
//     CPUID.  Today it dies with IllegalCode; with this file it runs.  That is
//     the whole reason the family had to be implemented, and it does not
//     depend on bit 20 at all.
//   * glibc's ifunc dispatcher DOES consult CPUID, and today picks its SSE2
//     strlen/strcmp, which are pcmpeqb + pmovmskb -- an order of magnitude
//     cheaper here than pcmpistri.  Turning bit 20 on moves those calls onto
//     the slower path.
//
// So bit 20 is now CORRECTNESS-backed but is a performance REGRESSION for
// glibc-linked guests until Sse42StrEval gets faster.  That trade is the main
// line's to make; this file states it rather than deciding it.
//
// ---------------------------------------------------------------------------
// LEGACY VS VEX
// ---------------------------------------------------------------------------
// The only semantic difference is contract C3: VPCMPISTRM/VPCMPESTRM ZERO bits
// 255:128 of YMM0, PCMPISTRM/PCMPESTRM leave them UNCHANGED.  The index forms
// write no vector register at all, so they zero nothing -- including the VEX
// ones.  sse42str_test.cpp poisons ymm0's high half on every row and requires
// the poison back from every legacy row and from every VEX index row, and
// zeros from every VEX mask row.
//
// ---------------------------------------------------------------------------
// KNOWN DEVIATIONS
// ---------------------------------------------------------------------------
//  * No #GP on a misaligned m128 operand, inherited: this front end models no
//    alignment check anywhere.
//  * The 66 prefix is required by the encoding and distorm enforces it; the
//    F2/F3-prefixed and no-prefix encodings do not exist and are not reachable
//    here.
//  * imm8[7] is reserved.  Real hardware ignores it and so does this file (the
//    reference data covers imm8 values with bit 7 set and hardware agreed).

#include <cstdlib>
#include <cstring>

#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/vex_decoder.h"

namespace swift::x86 {

using namespace swift::runtime;

#define __ assembler->

namespace {

constexpr auto kV128 = ir::ValueType::V128;
constexpr auto kU64 = ir::ValueType::U64;

// ---------------------------------------------------------------------------
// The control word handed to the helpers
// ---------------------------------------------------------------------------
// Everything the helper needs that is not one of the four operand halves.
// The two lengths are already absolute-valued and saturated (that happens in
// IR, see Sse42StrLength) so five bits each is exact.
constexpr u32 kCtlImmShift = 0;    // bits  7:0   the raw imm8
constexpr u32 kCtlExplicitBit = 8; // bit   8     explicit-length form
constexpr u32 kCtlLen1Shift = 9;   // bits 13:9   saturated len1 (0..16)
constexpr u32 kCtlLen2Shift = 14;  // bits 18:14  saturated len2 (0..16)

// The packed result the second helper returns.
constexpr u32 kResMaskShift = 0;   // bits 15:0   IntRes2
constexpr u32 kResIndexShift = 16; // bits 23:16  the index result (0..16)
constexpr u32 kResZfBit = 24;
constexpr u32 kResSfBit = 25;
constexpr u32 kResCfBit = 26;
constexpr u32 kResOfBit = 27;

struct Sse42StrStaged {
    u64 lo, hi;
    u64 ctl;
};

// See "WHY TWO HOST CALLS AND NOT ONE".  thread_local so two guest threads
// running string compares cannot see each other's operand.
thread_local Sse42StrStaged g_sse42_str_src1;

u64 Sse42StrStage(u64 lo, u64 hi, u64 ctl) {
    g_sse42_str_src1.lo = lo;
    g_sse42_str_src1.hi = hi;
    g_sse42_str_src1.ctl = ctl;
    // The return value is the def-use edge to Sse42StrEval and nothing else.
    return 0;
}

// Split a 128-bit operand into n elements, sign- or zero-extended to s32 so a
// single comparison covers all four data formats.
void Sse42StrUnpack(u64 lo, u64 hi, bool words, bool is_signed, s32* out) {
    if (words) {
        for (u32 i = 0; i < 8; ++i) {
            const u32 raw = u32(((i < 4 ? lo : hi) >> ((i % 4) * 16)) & 0xFFFFu);
            out[i] = is_signed ? s32(s16(raw)) : s32(raw);
        }
        return;
    }
    for (u32 i = 0; i < 16; ++i) {
        const u32 raw = u32(((i < 8 ? lo : hi) >> ((i % 8) * 8)) & 0xFFu);
        out[i] = is_signed ? s32(s8(raw)) : s32(raw);
    }
}

// The number of leading non-zero elements, which is what the implicit-length
// forms mean by "length".  A zero ELEMENT terminates, so for the word formats
// it is a zero WORD and not a zero byte.
u32 Sse42StrImplicitLength(const s32* elements, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        if (elements[i] == 0) {
            return i;
        }
    }
    return n;
}

u64 Sse42StrEval(u64 b_lo, u64 b_hi, u64 token) {
    // `token` is Sse42StrStage's return: an ordering edge, never read.
    (void)token;
    const Sse42StrStaged staged = g_sse42_str_src1;
    const u32 ctl = u32(staged.ctl);
    const u32 imm = (ctl >> kCtlImmShift) & 0xFFu;

    const bool words = (imm & 0x01u) != 0;
    const bool is_signed = (imm & 0x02u) != 0;
    const u32 aggregation = (imm >> 2) & 3u;
    const u32 polarity = (imm >> 4) & 3u;
    const bool most_significant = (imm & 0x40u) != 0;
    const u32 n = words ? 8u : 16u;

    s32 arr1[16];
    s32 arr2[16];
    Sse42StrUnpack(staged.lo, staged.hi, words, is_signed, arr1);
    Sse42StrUnpack(b_lo, b_hi, words, is_signed, arr2);

    u32 len1, len2;
    if ((ctl >> kCtlExplicitBit) & 1u) {
        len1 = (ctl >> kCtlLen1Shift) & 0x1Fu;
        len2 = (ctl >> kCtlLen2Shift) & 0x1Fu;
        // Sse42StrLength already saturated, but the helper must not be able to
        // index out of arr1/arr2 if it is ever called with something else.
        if (len1 > n) len1 = n;
        if (len2 > n) len2 = n;
    } else {
        len1 = Sse42StrImplicitLength(arr1, n);
        len2 = Sse42StrImplicitLength(arr2, n);
    }

    // BoolRes[i][j] with the SDM's validity override folded in.
    const auto compare = [&](u32 i, u32 j) -> bool {
        const bool valid1 = i < len1;
        const bool valid2 = j < len2;
        if (!valid1 && !valid2) {
            return aggregation >= 2;  // equal each / equal ordered force TRUE
        }
        if (!valid1) {
            // The needle ran out: equal ordered has matched to completion.
            return aggregation == 3;
        }
        if (!valid2) {
            // The text ran out mid-needle: no aggregation forces TRUE here.
            return false;
        }
        if (aggregation == 1) {
            return (i & 1u) == 0 ? arr2[j] >= arr1[i] : arr2[j] <= arr1[i];
        }
        return arr1[i] == arr2[j];
    };

    u32 res1 = 0;
    for (u32 j = 0; j < n; ++j) {
        bool bit = false;
        switch (aggregation) {
            case 0:  // equal any
                for (u32 i = 0; i < n && !bit; ++i) {
                    bit = compare(i, j);
                }
                break;
            case 1:  // ranges
                for (u32 i = 0; i + 1 < n && !bit; i += 2) {
                    bit = compare(i, j) && compare(i + 1, j);
                }
                break;
            case 2:  // equal each
                bit = compare(j, j);
                break;
            default:  // equal ordered
                bit = true;
                for (u32 i = 0; i + j < n && bit; ++i) {
                    bit = compare(i, j + i);
                }
                break;
        }
        if (bit) {
            res1 |= 1u << j;
        }
    }

    const u32 all = n == 16 ? 0xFFFFu : 0xFFu;
    u32 res2;
    switch (polarity) {
        case 1:
            res2 = (~res1) & all;
            break;
        case 3:
            // Invert only the bits belonging to VALID elements of the second
            // operand.  len2 can be n, so build the mask in 32 bits.
            res2 = res1 ^ u32((1u << len2) - 1u);
            break;
        default:
            res2 = res1;
            break;
    }
    res2 &= all;

    u32 index = n;
    if (res2 != 0) {
        if (most_significant) {
            index = 31u - u32(__builtin_clz(res2));
        } else {
            index = u32(__builtin_ctz(res2));
        }
    }

    u64 packed = u64(res2) << kResMaskShift;
    packed |= u64(index) << kResIndexShift;
    packed |= u64(len2 < n ? 1 : 0) << kResZfBit;
    packed |= u64(len1 < n ? 1 : 0) << kResSfBit;
    packed |= u64(res2 != 0 ? 1 : 0) << kResCfBit;
    packed |= u64(res2 & 1u) << kResOfBit;
    return packed;
}

// Materialize an arbitrary 128-bit constant.  Byte-identical to VecConst in
// decoder_sse4.cc / decoder_avx_int.cc (they should all become one helper).
ir::Value VecConst(ir::Assembler* as, u64 lo, u64 hi) {
    auto low = as->VecDup64(as->LoadImm(ir::Imm(lo)).SetType(kU64)).SetType(kV128);
    if (lo == hi) {
        return low;
    }
    auto high = as->VecDup64(as->LoadImm(ir::Imm(hi)).SetType(kU64)).SetType(kV128);
    return as->VecZip(low, high, ir::Imm(64u), ir::Imm(0u)).SetType(kV128);
}

}  // namespace

// ---------------------------------------------------------------------------
// Explicit lengths: |signed| saturated at n
// ---------------------------------------------------------------------------
// This is the single most error-prone line in the family.  `raw` is the
// architectural length already widened to 64 bits WITH SIGN, so:
//
//   * -3 must become 3, not 0xFFFFFFFFFFFFFFFD and not 0;
//   * INT64_MIN must become n, because its true absolute value is 2^63 and
//     saturates -- negating it in two's complement gives INT64_MIN back, whose
//     UNSIGNED value is huge, which is exactly why the saturation below is an
//     unsigned test and not a signed compare;
//   * anything at or above n must become n, and only values strictly below n
//     pass through.
//
// The saturation is spelled as "are any bits above log2(n) set" rather than as
// a compare because n is 8 or 16, both powers of two, so `abs & (n-1)` is
// already the answer whenever the high bits are clear.
ir::Value X64Decoder::Sse42StrLength(ir::Value raw, u32 elements) {
    auto zero = __ LoadImm(ir::Imm(u64(0))).SetType(kU64);
    auto negated = __ Sub(zero, ir::Operand{raw}).SetType(kU64);
    auto absolute = __ Select(__ TestBit(raw, ir::Imm(63u)), negated, raw).SetType(kU64);
    const u32 shift = elements == 16 ? 4u : 3u;
    auto high = __ LsrImm(absolute, ir::Imm(shift)).SetType(kU64);
    auto low = __ And(absolute, ir::Operand{ir::Imm(u64(elements - 1))}).SetType(kU64);
    auto saturated = __ LoadImm(ir::Imm(u64(elements))).SetType(kU64);
    return __ Select(__ TestNotZero(high), saturated, low).SetType(kU64);
}

// ---------------------------------------------------------------------------
// The six flags
// ---------------------------------------------------------------------------
// Same recipe as DecodePopf, which is the only other place in this front end
// that has to write all six from computed 0/1 values:
//   PF  a value with ODD low-byte parity spells PF = 0
//   ZF  SaveFlags(v, Zero) sets ZF from v == 0, so the bit is inverted first
//   SF  SaveFlags(v, Negate) reads v's sign bit, so the bit is shifted to 63
//   AF  0xF + 0 carries out of bit 3 exactly when the added bit is 1
//   CF / OF  SetCarry / SetOverflow write the flag bit directly
void X64Decoder::Sse42StrFlags(ir::Value packed) {
    const auto bit = [&](u32 position) {
        return __ And(__ LsrImm(packed, ir::Imm(position)), ir::Operand{ir::Imm(u64(1))})
                .SetType(kU64);
    };
    auto one = __ LoadImm(ir::Imm(u64(1))).SetType(kU64);
    auto zero = __ LoadImm(ir::Imm(u64(0))).SetType(kU64);
    // PF and AF are architecturally 0 for all four instructions.
    __ SaveFlags(__ Or(one, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Parity);
    auto zf = __ Select(__ TestNotZero(bit(kResZfBit)), zero, one).SetType(kU64);
    __ SaveFlags(__ Or(zf, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Zero);
    auto sf = __ LslImm(bit(kResSfBit), ir::Imm(63u)).SetType(kU64);
    __ SaveFlags(__ Or(sf, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Negate);
    __ SaveFlags(__ Add(__ LoadImm(ir::Imm(u64(0xF))), ir::Operand{ir::Imm(u64(0))}),
                 ir::Flags::AuxiliaryCarry);
    __ SetCarry(bit(kResCfBit));
    __ SetOverflow(bit(kResOfBit));
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
}

// ---------------------------------------------------------------------------
// The shared body
// ---------------------------------------------------------------------------
void X64Decoder::DecodeSse42StrBody(_RegisterType reg1,
                                    ir::Value src2,
                                    u8 imm8,
                                    bool explicit_length,
                                    bool wide,
                                    bool mask_form,
                                    bool vex) {
    const u32 elements = (imm8 & 1u) ? 8u : 16u;

    // The control word.  For the implicit forms it is a pure constant.
    ir::Value ctl;
    if (explicit_length) {
        // EAX/RAX is the first operand's length, EDX/RDX the second's.  Read
        // with SIGN extension: the architectural value is signed even in the
        // 32-bit form, where bits 63:32 of RAX are architecturally ignored.
        auto raw1 = wide ? R(R_RAX) : Extend(R(R_EAX), kU64, true);
        auto raw2 = wide ? R(R_RDX) : Extend(R(R_EDX), kU64, true);
        auto len1 = Sse42StrLength(raw1, elements);
        auto len2 = Sse42StrLength(raw2, elements);
        auto base = __ LoadImm(ir::Imm(u64(imm8) | (u64(1) << kCtlExplicitBit))).SetType(kU64);
        auto packed_lengths = __ Or(__ LslImm(len1, ir::Imm(kCtlLen1Shift)).SetType(kU64),
                                    ir::Operand{__ LslImm(len2, ir::Imm(kCtlLen2Shift))
                                                        .SetType(kU64)})
                                      .SetType(kU64);
        ctl = __ Or(base, ir::Operand{packed_lengths}).SetType(kU64);
    } else {
        ctl = __ LoadImm(ir::Imm(u64(imm8))).SetType(kU64);
    }

    // The first operand is always a register, so its halves are two uniform
    // reads; the second has already been loaded as a V128 by the caller (the
    // memory form goes through the IR memory path so a fault is a guest fault).
    auto token = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&Sse42StrStage)}},
                               XmmLo(reg1),
                               XmmHi(reg1),
                               ctl);
    auto packed = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&Sse42StrEval)}},
                                __ VecExtract64(src2, ir::Imm(0u)).SetType(kU64),
                                __ VecExtract64(src2, ir::Imm(1u)).SetType(kU64),
                                token)
                          .SetType(kU64);

    if (mask_form) {
        // The destination is the IMPLICIT XMM0.  distorm reports no operand for
        // it (ops[2] is the immediate), unlike the BLENDV family where it does,
        // so it is named architecturally here.
        auto intres2 = __ And(packed, ir::Operand{ir::Imm(u64(0xFFFF))}).SetType(kU64);
        if ((imm8 & 0x40u) == 0) {
            // Bit mask: IntRes2 zero-extended to 128 bits.
            XmmLo(R_XMM0, intres2);
            XmmHi(R_XMM0, __ LoadImm(ir::Imm(u64(0))));
        } else {
            // Element mask: element j is all-ones exactly when IntRes2[j] is
            // set.  VecTableLookup8 broadcasts the mask's bytes into the lanes
            // that need them (index 0x80 selects nothing and yields a zero
            // byte), the AND isolates one bit per lane and the compare turns a
            // surviving bit into all-ones.
            const bool words = (imm8 & 1u) != 0;
            auto spread_control =
                    words ? VecConst(assembler, 0x8000800080008000ull, 0x8000800080008000ull)
                          : VecConst(assembler, 0x0000000000000000ull, 0x0101010101010101ull);
            auto bits = words ? VecConst(assembler, 0x0008000400020001ull, 0x0080004000200010ull)
                              : VecConst(assembler, 0x8040201008040201ull, 0x8040201008040201ull);
            auto spread = __ VecTableLookup8(__ VecDup64(intres2).SetType(kV128), spread_control)
                                  .SetType(kV128);
            auto selected = __ VecAnd(spread, bits).SetType(kV128);
            auto mask = __ VecCmpEq(selected, bits, ir::Imm(words ? 16u : 8u)).SetType(kV128);
            XmmWrite(R_XMM0, mask);
        }
        // C3: a VEX write zeroes bits 255:128, a legacy write preserves them.
        if (vex) {
            ZeroYmmHigh(0);
        }
    } else {
        // ECX, so bits 63:32 of RCX are zeroed -- including under REX.W, where
        // only the LENGTHS are 64-bit.  The index forms write no vector
        // register, so nothing is zeroed even in the VEX encoding.
        R(R_ECX, __ And(__ LsrImm(packed, ir::Imm(kResIndexShift)),
                        ir::Operand{ir::Imm(u64(0xFF))})
                         .SetType(kU64));
    }

    Sse42StrFlags(packed);
}

// ---------------------------------------------------------------------------
// Dispatch -- legacy 66 0F 3A 60..63
// ---------------------------------------------------------------------------
bool X64Decoder::DecodeSse42Str(_DInst& insn) {
    if (!Sse42StrEnabled()) {
        return false;
    }
    bool mask_form;
    bool explicit_length;
    switch (insn.opcode) {
        case I_PCMPESTRM:
            mask_form = true;
            explicit_length = true;
            break;
        case I_PCMPESTRI:
            mask_form = false;
            explicit_length = true;
            break;
        case I_PCMPISTRM:
            mask_form = true;
            explicit_length = false;
            break;
        case I_PCMPISTRI:
            mask_form = false;
            explicit_length = false;
            break;
        default:
            return false;
    }
    // ops[0] is the xmm register operand, ops[1] the xmm/m128 one, ops[2] the
    // immediate.  Anything else is a distorm shape this file has not seen, and
    // guessing would be worse than declining.
    if (insn.ops[0].type != O_REG || insn.ops[2].type != O_IMM) {
        return false;
    }
    const auto reg1 = static_cast<_RegisterType>(insn.ops[0].index);
    if (reg1 < R_XMM0 || reg1 > R_XMM15) {
        return false;
    }
    if (insn.ops[1].type == O_REG) {
        const auto reg2 = static_cast<_RegisterType>(insn.ops[1].index);
        if (reg2 < R_XMM0 || reg2 > R_XMM15) {
            return false;
        }
    }
    const bool wide = FLAG_GET_OPSIZE(insn.flags) == Decode64Bits;
    DecodeSse42StrBody(reg1, LoadSrcVec(insn, insn.ops[1]), u8(insn.imm.byte), explicit_length,
                       wide, mask_form, false);
    return true;
}

// ---------------------------------------------------------------------------
// Dispatch -- VEX.128.66.0F3A 60..63
// ---------------------------------------------------------------------------
bool X64Decoder::DecodeSse42StrVex(const VexInsn& v) {
    if (!Sse42StrEnabled() || !v.valid || v.map != VexMap::Map0F3A || v.pp != VexPP::P66) {
        return false;
    }
    // VEX.L must be 0 (the 256-bit encoding is #UD) and VEX.vvvv must be the
    // "no operand" 1111b.  Declining rather than executing keeps a malformed
    // encoding a decode failure instead of a wrong answer against xmm0.
    if (v.l || v.vvvv_valid || !v.has_imm8) {
        return false;
    }
    bool mask_form;
    bool explicit_length;
    switch (v.opcode) {
        case 0x60:
            mask_form = true;
            explicit_length = true;
            break;
        case 0x61:
            mask_form = false;
            explicit_length = true;
            break;
        case 0x62:
            mask_form = true;
            explicit_length = false;
            break;
        case 0x63:
            mask_form = false;
            explicit_length = false;
            break;
        default:
            return false;
    }
    DecodeSse42StrBody(XmmOf(v.reg), VexLoadVec(v), v.imm8, explicit_length, v.w, mask_form, true);
    return true;
}

// Escape hatch, default ON: without these handlers the guest dies with
// IllegalCode, so a bisectable off switch is worth one getenv.  Read once --
// the dispatchers consult it per instruction and getenv is neither cheap nor
// thread-safe against setenv.  DecodeCpuid consults it too, so CPUID can never
// promise SSE4.2 to a build where the family is switched off (see decoder.h).
bool X64Decoder::Sse42StrEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SVM_SSE42STR");
        return !env || std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

#undef __

}  // namespace swift::x86
