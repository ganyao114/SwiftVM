// BMI1 / BMI2 -- the VEX-encoded general-purpose bit-manipulation family, plus
// the two legacy-encoded counters TZCNT and LZCNT.
//
// WHY THIS FAMILY BLOCKS EVERYTHING ELSE
// ---------------------------------------------------------------------------
// glibc's ifunc resolvers will only select the __*_avx2 string routines when
// AVX2, BMI2 and Fast_Unaligned_Load are ALL advertised (strlen_ifunc tests
// `andl $0x128`).  Every one of those routines -- __strlen_avx2, __strcmp_avx2,
// __strchr_avx2, __strrchr_avx2, __memchr_avx2, __memcmp_avx2_movbe,
// __strncmp_avx2, __stpcpy/__strcpy_avx2, __wcslen/__wcsnlen/__wmemchr_avx2 --
// contains at least one BMI1/BMI2 instruction.  An unimplemented instruction
// here is not slow, it is fatal: DecodeSwitch's default path raises FALLBACK,
// which surfaces as ExitReason::IllegalCode and kills the guest process (there
// is no interpreter fallback).  So BMI2 must NOT be advertised in CPUID until
// this file is verified; enabling the bit is a separate, later main-line step.
//
// Measured frequency in one glibc build (shrxl 19, sarxl 18, blsmskl 13,
// blsmskq 10, shlxl 9, bzhil 8, bzhiq 7, blsrq 2 = 86 sites) is why the shift
// and mask forms get real IR while PDEP/PEXT/MULX -- zero sites -- go through
// host helpers.
//
// ---------------------------------------------------------------------------
// ORACLES
// ---------------------------------------------------------------------------
// Unicorn 2.1.4 DOES execute BMI and DOES honour VEX.vvvv on these GPR forms
// (both probed, not assumed), but it is wrong in four places, found by diffing
// it against Rosetta over 2640 cases and adjudicating each disagreement against
// the SDM (source/tests/fuzz/bmi_unicorn_check.c):
//   * BLSI's CF is inverted -- Unicorn uses the BLSR rule, but the SDM sets CF
//     when the source is NON-zero.  132 rows.
//   * BZHI reduces N modulo the operand size and uses CF = N >= width-1; the
//     SDM does neither (N is the whole SRC2[7:0], CF is N > width-1).  36 rows.
//   * BEXTR reduces LEN modulo the operand size, so LEN = 0xFF at W0 extracts
//     31 bits rather than 32.  3 rows.
//   * PDEP at W0 computes the 64-bit result.  6 rows.  PEXT at W0 is correct,
//     so that is two code paths, not one bug.
// Rosetta matched the SDM on all 2640 rows, so the committed reference table is
// Rosetta's.  Every CF rule below is the SDM's, not Unicorn's.
//
// ---------------------------------------------------------------------------
// INTEGRATION (all of these live in main-line-owned files)
// ---------------------------------------------------------------------------
// 1. decoder.h, private section of X64Decoder, next to the DecodeAvx* block:
//
//        // ---- BMI1 / BMI2 (decoder_bmi.cc) ---------------------------------
//        [[nodiscard]] static bool BmiEnabled();
//        bool DecodeBmi(const VexInsn& v);
//        ir::Value BmiSrc(const VexInsn& v, u32 width);
//        void BmiWriteFlagsNZ(ir::Value result, u32 width);
//        void DecodeBmiAndn(const VexInsn& v, u32 width);
//        void DecodeBmiBls(const VexInsn& v, u32 width, u32 kind);
//        void DecodeBmiBzhi(const VexInsn& v, u32 width);
//        void DecodeBmiBextr(const VexInsn& v, u32 width);
//        void DecodeBmiShiftX(const VexInsn& v, u32 width, u32 kind);
//        void DecodeBmiRorx(const VexInsn& v, u32 width);
//        void DecodeBmiMulx(const VexInsn& v, u32 width);
//        void DecodeBmiDepExt(const VexInsn& v, u32 width, bool deposit);
//        void DecodeTzcnt(_DInst& insn);
//        void DecodeLzcntBmi(_DInst& insn);
//
// 2. decoder.cc, the VEX dispatch in Decode().  Today:
//
//        if (AvxEnabled() && HasVexPrefix(code_ptr, kMaxInsnBytes)) {
//            const auto vex = DecodeVexInsn(code_ptr, kMaxInsnBytes);
//            if (vex.valid) {
//                const auto saved_pc = pc;
//                pc += vex.length;
//                if (DecodeAvxInt(vex) || DecodeAvxFp(vex)) {
//
//    becomes:
//
//        const bool avx_on = AvxEnabled();
//        const bool bmi_on = BmiEnabled();
//        if ((avx_on || bmi_on) && HasVexPrefix(code_ptr, kMaxInsnBytes)) {
//            const auto vex = DecodeVexInsn(code_ptr, kMaxInsnBytes);
//            if (vex.valid) {
//                const auto saved_pc = pc;
//                pc += vex.length;
//                if (DecodeBmi(vex) ||
//                    (avx_on && (DecodeAvxInt(vex) || DecodeAvxFp(vex)))) {
//
//    BMI is tried first only for clarity; the families share no (map, pp,
//    opcode) triple.  BMI lives on 0F38 F2/F3/F5/F6/F7 and 0F3A F0, and the AVX
//    handlers' 0xF2/0xF3/0xF5/0xF6 cases are all on the 0F map (verified
//    against the `case VexMap::` boundaries in decoder_avx_int.cc, lines
//    808/985/1113, and decoder_avx_fp.cc, 1306/1477/1534).  DecodeBmi applies
//    its own SVM_BMI gate and returns false before emitting any IR whenever it
//    declines, so a decline never leaves a half-built block behind.
//
// 3. decoder.cc, DecodeSwitch.  TZCNT is currently aliased to BSF and LZCNT to
//    a CF-less handler, both correct only while BMI1/LZCNT stay hidden in
//    CPUID.  They must become their own instructions before the bits are
//    advertised:
//
//        case I_BSF:                     case I_LZCNT:
//            DecodeBitScan(insn, false);     DecodeLzcntBmi(insn);
//            break;                          break;
//        case I_TZCNT:
//            DecodeTzcnt(insn);
//            break;
//
//    (I_TZCNT drops out of the I_BSF case; I_LZCNT stops calling DecodeLzcnt.)
//    Both new handlers are gated on BmiEnabled() internally and fall back to
//    the existing behaviour when the gate is off, so step 3 is safe to land
//    before the CPUID bits move.
//
// 4. source/runtime/frontend/x86/CMakeLists.txt: add decoder_bmi.cc.
//    source/tests/CMakeLists.txt: add fuzz/bmi_test.cpp.
//
// ---------------------------------------------------------------------------
// CONTRACTS
// ---------------------------------------------------------------------------
// B1  These are VEX-encoded but NOT vector instructions.  They operate on
//     GPRs, VEX.L must be 0 (the manuals write VEX.LZ) and an L=1 encoding is
//     #UD -- declined here, which traps the block rather than executing a
//     256-bit-looking form as 32-bit.
// B2  VEX.vvvv is a REAL operand for all of these except RORX, so
//     VexInsn::vvvv_valid must NOT be consulted: the "unused" marker 0b1111
//     un-inverts to register 0, which for BMI legitimately means rax/eax.
//     (Probed: `blsi eax, ecx` encodes vvvv as 1111 and Unicorn writes eax.)
// B3  The IR value that carries SaveFlags must be typed at the ARCHITECTURAL
//     width.  The arm64 JIT sizes its flag-setting op by the value's type
//     (W vs X register, so SF comes from bit 31 or bit 63) and the interpreter
//     reads `def->ReturnType()` for the same purpose.  A U64-typed value
//     standing in for a 32-bit result silently reports the wrong SF and, for a
//     result whose low 32 bits are zero, the wrong ZF.
// B4  Both backends MASK a variable shift amount by (width-1) -- arm64 in
//     hardware, the interpreter explicitly in RunLslValue/RunLsrValue.  Every
//     place where x86 wants a >= width count to produce something other than
//     the masked shift (BZHI, BEXTR) therefore needs an explicit Select, not a
//     shift the hardware will silently fold.
// B5  SARX/SHLX/SHRX/RORX/MULX/PDEP/PEXT do not touch ANY flag.  In this IR
//     that is expressed by emitting no SaveFlags/ClearFlags/SetCarry AND by
//     leaving `carry_` alone: flag state is only ever written by an explicit
//     flag op, and the carry-polarity tracker is decoder-local state that must
//     keep describing whatever the last flag-writing instruction stored.
//     Resetting carry_ here would not be conservative -- it would push later CF
//     consumers onto the runtime polarity byte, which is only authoritative
//     once some instruction has actually rewritten it (DecodeCmp, for one,
//     does not).  bmi_test.cpp's prologue variant 1 covers the first half of
//     this directly: a preceding CMP leaves ARM's not-borrow carry stored with
//     inverted polarity, so a no-flag instruction that disturbed the flag word
//     produces a complemented SETC.  The carry_ half is reasoned, not measured
//     -- a control that reset carry_ to Unknown here still passed, because the
//     recovery path happened to read a correct polarity byte in that block.
//
// ---------------------------------------------------------------------------
// KNOWN DEVIATIONS
// ---------------------------------------------------------------------------
//  * Flags the SDM calls undefined (AF everywhere, PF everywhere, SF for
//    BEXTR/TZCNT/LZCNT) are left at whatever the emitted sequence produces.
//    bmi_test.cpp compares only the defined ones, and bmi_model.h marks the
//    undefined SF with -1 so it is skipped rather than silently asserted.
//  * A segment override on the memory form is ignored -- VexInsn does not
//    carry the segment.  Same deviation decoder_avx_fp.cc documents for
//    VexAddress; an FS/GS-prefixed BMI instruction does not occur in practice.
//  * VEX.vvvv is not checked against 1111 for RORX, where a different value is
//    architecturally #UD.  Consistent with the rest of the front end, which
//    does not enforce reserved-field #UD.
//  * 16-bit TZCNT/LZCNT (66 F3 0F BC/BD) are decoded at their architectural
//    width, matching DecodeBitScan's 66-prefix recovery, but no 16-bit BMI1/2
//    VEX form exists so the VEX side is 32/64 only.

#include <cstdlib>
#include <cstring>
#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/vex_decoder.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

// VexInsn register numbers are architectural (0..15); distorm's GPR enum is in
// the same order, so the 64-bit and 32-bit views are one addition apart.
_RegisterType BmiGprOf(u32 index, bool wide) {
    return static_cast<_RegisterType>((wide ? R_RAX : R_EAX) + index);
}

// High half of an unsigned 64x64 product (MULX.W1).  No IR opcode produces it
// and MULX has zero call sites in glibc, so a host helper is the right trade:
// adding one would mean touching runtime/backend, which this work must not do.
u64 BmiMulHi64(u64 a, u64 b) {
    return static_cast<u64>((static_cast<unsigned __int128>(a) * b) >> 64);
}

// PDEP: take the low popcount(mask) bits of `src` and scatter them, in order,
// into the set bit positions of `mask`.  PEXT is the inverse gather.  Neither
// is expressible with the existing IR at any reasonable cost, and both have
// zero call sites in the surveyed glibc, so they are host helpers.
u64 BmiPdep64(u64 src, u64 mask) {
    u64 result = 0;
    u64 bit = 1;
    while (mask) {
        const u64 low = mask & (~mask + 1);  // lowest set bit of mask
        if (src & bit) {
            result |= low;
        }
        mask ^= low;
        bit <<= 1;
    }
    return result;
}

u64 BmiPext64(u64 src, u64 mask) {
    u64 result = 0;
    u64 bit = 1;
    while (mask) {
        const u64 low = mask & (~mask + 1);
        if (src & low) {
            result |= bit;
        }
        mask ^= low;
        bit <<= 1;
    }
    return result;
}

// TZCNT: the count of trailing zeros, or the operand width for a zero source.
// This is exactly where TZCNT differs from BSF, which leaves the destination
// UNCHANGED for a zero source.
u64 BmiTzcnt64(u64 v, u64 width) {
    return v ? static_cast<u64>(__builtin_ctzll(v)) : width;
}

// LZCNT within the architectural width (the same shape as decoder_alu.cc's
// Lzcnt64, duplicated because that one is file-static).
u64 BmiLzcnt64(u64 v, u64 width) {
    if (!v) {
        return width;
    }
    const u32 leading = static_cast<u32>(__builtin_clzll(v));
    return leading - (64 - width);
}

}  // namespace

// SVM_BMI gates every handler in this file.  With the gate off a BMI encoding
// falls through to the old behaviour (FALLBACK for the VEX forms, BSF for
// TZCNT), which is what the main line runs until this family is signed off.
// No CPUID bit is advertised either way, so a well-behaved guest never emits
// BMI at all.
bool X64Decoder::BmiEnabled() {
    static const bool enabled = [] {
        const char* env = swift::runtime::PerfGetenv("SVM_BMI");
        return env && std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

// The r/m operand, typed at the architectural width (contract B3).
ir::Value X64Decoder::BmiSrc(const VexInsn& v, u32 width) {
    if (v.RmIsRegister()) {
        return R(BmiGprOf(v.rm, width == 64));
    }
    return MemLoad(ir::Operand{VexAddress(v)},
                   GetSize(width),
                   GetTsoMode() == runtime::TsoMode::AcqRel);
}

// SF/ZF from `result` (which must already be typed at the guest width, B3),
// with OF and AF cleared.  CF is left to the caller: every instruction in this
// family defines it differently, and two of them (BLSI, BZHI) define it from
// something other than the result.
void X64Decoder::BmiWriteFlagsNZ(ir::Value result, u32 width) {
    (void)width;
    __ ClearFlags(ir::Flags::Overflow | ir::Flags::AuxiliaryCarry);
    __ SaveFlags(result, ir::Flags::Negate | ir::Flags::Zero | ir::Flags::Parity);
}

// ANDN r32a/r64a, r32b/r64b, r/m -- VEX.LZ.0F38.W0/W1 F2 /r.
//   DEST = (NOT SRC1) AND SRC2, SRC1 = VEX.vvvv, SRC2 = r/m.
// SF/ZF from the result, CF = OF = 0, AF/PF undefined.
void X64Decoder::DecodeBmiAndn(const VexInsn& v, u32 width) {
    const bool wide = width == 64;
    auto src1 = R(BmiGprOf(v.vvvv, wide));
    auto src2 = BmiSrc(v, width);
    // AndNot(left, right) is left AND NOT right, so the x86 operand order is
    // reversed here: the INVERTED source is the second argument.
    auto result = __ AndNot(src2, ir::Operand{src1}).SetType(GetSize(width));
    __ ClearFlags(ir::Flags::Carry);
    BmiWriteFlagsNZ(result, width);
    // CF is a hard zero, which reads the same under either stored polarity.
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
    R(BmiGprOf(v.reg, wide), result);
}

// BLSR / BLSMSK / BLSI -- VEX.LZ.0F38.W0/W1 F3 /1, /2, /3.
//   The DESTINATION is VEX.vvvv and the source is r/m (ModRM.reg is the opcode
//   extension), which is the reverse of every other form in this file.
//   kind 1 BLSR   = (SRC-1) AND SRC     CF = (SRC == 0)
//   kind 2 BLSMSK = (SRC-1) XOR SRC     CF = (SRC == 0)   ZF is always 0
//   kind 3 BLSI   = (-SRC)  AND SRC     CF = (SRC != 0)
// SF/ZF from the result, OF = 0, AF/PF undefined.
void X64Decoder::DecodeBmiBls(const VexInsn& v, u32 width, u32 kind) {
    const bool wide = width == 64;
    const auto type = GetSize(width);
    auto src = BmiSrc(v, width);
    auto decremented = __ Sub(src, ir::Operand{ir::Imm(u64(1))}).SetType(type);
    ir::Value result;
    if (kind == 2) {
        result = __ Xor(decremented, ir::Operand{src}).SetType(type);
    } else {
        auto cleared = __ And(decremented, ir::Operand{src}).SetType(type);
        // BLSI is the lowest set bit, i.e. SRC minus BLSR(SRC).  Spelling it
        // this way keeps every operand at the guest width without needing a
        // width-typed zero constant to negate against.
        result = kind == 3 ? __ Sub(src, ir::Operand{cleared}).SetType(type) : cleared;
    }
    BmiWriteFlagsNZ(result, width);
    // BLSI's CF is the complement of the other two: it is 1 for a NON-zero
    // source.  Getting this backwards is invisible in the result value.
    auto cf = kind == 3 ? ir::Value{__ TestNotZero(src)} : ir::Value{__ TestZero(src)};
    __ SetCarry(cf);
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
    R(BmiGprOf(v.vvvv, wide), result);
}

// BZHI r32a/r64a, r/m, r32b/r64b -- VEX.LZ.0F38.W0/W1 F5 /r.
//   N = SRC2[7:0] (VEX.vvvv); DEST = SRC1 with bits [width-1:N] zeroed.
//   N >= width leaves SRC1 untouched and sets CF; otherwise CF = 0.
// SF/ZF from the result, OF = 0, AF/PF undefined.
void X64Decoder::DecodeBmiBzhi(const VexInsn& v, u32 width) {
    const bool wide = width == 64;
    const auto type = GetSize(width);
    auto src = BmiSrc(v, width);
    auto index = R(BmiGprOf(v.vvvv, wide));
    // Only the low byte of the index register participates, so N is 0..255 and
    // "N >= width" is exactly "N has a bit set at or above log2(width)".
    auto n = __ And(index, ir::Operand{ir::Imm(u64(0xFF))}).SetType(type);
    const u64 over = width == 32 ? 0xE0 : 0xC0;
    auto too_big = __ TestNotZero(__ And(n, ir::Operand{ir::Imm(over)}).SetType(type));
    // (1 << N) - 1.  Only evaluated for its own sake when N < width; for larger
    // N both backends fold the shift modulo the width and the Select below
    // discards the value (contract B4).
    auto one = __ LoadImm(ir::Imm(u64(1))).SetType(type);
    auto mask = __ Sub(__ LslValue(one, n).SetType(type), ir::Operand{ir::Imm(u64(1))})
                        .SetType(type);
    auto kept = __ And(src, ir::Operand{mask}).SetType(type);
    auto result = __ Select(too_big, src, kept).SetType(type);
    // Select carries no flags, so the architectural SF/ZF come from a separate
    // flag-only op over the finished result.
    auto flagged = __ Or(result, ir::Operand{ir::Imm(u64(0))}).SetType(type);
    BmiWriteFlagsNZ(flagged, width);
    __ SetCarry(too_big);
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
    R(BmiGprOf(v.reg, wide), result);
}

// BEXTR r32a/r64a, r/m, r32b/r64b -- VEX.LZ.0F38.W0/W1 F7 /r.
//   START = SRC2[7:0], LEN = SRC2[15:8] (SRC2 = VEX.vvvv).
//   DEST = zero_extend(SRC1[START+LEN-1 : START]); bits past the operand width
//   read as zero, so START >= width gives 0 and LEN >= width extracts to the
//   top.  LEN == 0 gives 0.
// ZF from the result, CF = OF = 0, AF/SF/PF undefined.
void X64Decoder::DecodeBmiBextr(const VexInsn& v, u32 width) {
    const bool wide = width == 64;
    const auto type = GetSize(width);
    auto src = BmiSrc(v, width);
    auto control = R(BmiGprOf(v.vvvv, wide));
    auto start = __ And(control, ir::Operand{ir::Imm(u64(0xFF))}).SetType(type);
    auto len = __ And(__ LsrImm(control, ir::Imm(8u)).SetType(type),
                      ir::Operand{ir::Imm(u64(0xFF))})
                       .SetType(type);
    const u64 over = width == 32 ? 0xE0 : 0xC0;
    // Both START and LEN are 0..255 while the backends mask a shift by
    // width-1, so each needs an explicit out-of-range branch (contract B4).
    auto start_over = __ TestNotZero(__ And(start, ir::Operand{ir::Imm(over)}).SetType(type));
    auto zero = __ LoadImm(ir::Imm(u64(0))).SetType(type);
    auto shifted =
            __ Select(start_over, zero, __ LsrValue(src, start).SetType(type)).SetType(type);
    auto len_over = __ TestNotZero(__ And(len, ir::Operand{ir::Imm(over)}).SetType(type));
    auto one = __ LoadImm(ir::Imm(u64(1))).SetType(type);
    auto len_mask = __ Sub(__ LslValue(one, len).SetType(type), ir::Operand{ir::Imm(u64(1))})
                            .SetType(type);
    auto all_ones = __ LoadImm(ir::Imm(width == 32 ? u64(0xFFFFFFFF) : ~u64(0))).SetType(type);
    auto mask = __ Select(len_over, all_ones, len_mask).SetType(type);
    auto result = __ And(shifted, ir::Operand{mask}).SetType(type);
    __ ClearFlags(ir::Flags::Carry | ir::Flags::Overflow | ir::Flags::AuxiliaryCarry);
    __ SaveFlags(result, ir::Flags::Zero);
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
    R(BmiGprOf(v.reg, wide), result);
}

// SHLX / SHRX / SARX -- VEX.LZ.66(F2)(F3).0F38.W0/W1 F7 /r.
//   DEST = SRC1 shifted by SRC2[4:0] (or [5:0] at W1); SRC1 = r/m, SRC2 =
//   VEX.vvvv.  kind 0 = shl, 1 = shr, 2 = sar.
// NO FLAGS ARE AFFECTED -- unlike the legacy SHL/SHR/SAR, which is the whole
// reason these encodings exist (contract B5: emit no flag op, and do not touch
// carry_).
void X64Decoder::DecodeBmiShiftX(const VexInsn& v, u32 width, u32 kind) {
    const bool wide = width == 64;
    const auto type = GetSize(width);
    auto src = BmiSrc(v, width);
    auto count = __ And(R(BmiGprOf(v.vvvv, wide)), ir::Operand{ir::Imm(u64(width - 1))})
                         .SetType(type);
    ir::Value result;
    switch (kind) {
        case 0:
            result = __ LslValue(src, count).SetType(type);
            break;
        case 1:
            result = __ LsrValue(src, count).SetType(type);
            break;
        default:
            // AsrValue sign-extends from the SOURCE value's type and shifts in
            // a register of the RESULT's width, so both must be the guest
            // width or a 32-bit SARX would sign-extend from bit 63.
            result = __ AsrValue(src, count).SetType(type);
            break;
    }
    R(BmiGprOf(v.reg, wide), result);
}

// RORX r32/r64, r/m, imm8 -- VEX.LZ.F2.0F3A.W0/W1 F0 /r ib.
// NO FLAGS AFFECTED (contract B5).
void X64Decoder::DecodeBmiRorx(const VexInsn& v, u32 width) {
    const bool wide = width == 64;
    const auto type = GetSize(width);
    auto src = BmiSrc(v, width);
    const u32 rotate = v.imm8 & (width - 1);
    // A zero rotate is the identity.  Spelled as an explicit copy rather than
    // Ror #0 so neither backend has to accept a zero-width rotate.
    auto result = rotate == 0 ? __ Or(src, ir::Operand{ir::Imm(u64(0))}).SetType(type)
                              : __ RorImm(src, ir::Imm(rotate)).SetType(type);
    R(BmiGprOf(v.reg, wide), result);
}

// MULX r32a/r64a, r32b/r64b, r/m -- VEX.LZ.F2.0F38.W0/W1 F6 /r.
//   Unsigned (r/m * EDX/RDX).  ModRM.reg takes the HIGH half, VEX.vvvv the LOW
//   half; RDX is implicit and is NOT written.  When the two destinations name
//   the same register the high half must win, which is why the low half is
//   stored first.
// NO FLAGS AFFECTED (contract B5).
void X64Decoder::DecodeBmiMulx(const VexInsn& v, u32 width) {
    const bool wide = width == 64;
    auto multiplicand = R(BmiGprOf(2 /* rdx */, wide));
    auto multiplier = BmiSrc(v, width);
    ir::Value low, high;
    if (!wide) {
        // 32x32 fits in one 64-bit multiply, so no helper is needed.  Both
        // halves stay U64-typed: the backend picks W or X registers from an
        // instruction's OWN type, so a U32-typed shift of a U64 product would
        // emit `lsr w, x, #32` -- a register-width mismatch that vixl turns
        // into an undefined instruction (it faulted at run time, it did not
        // merely compute the wrong value).  The 32-bit truncation and the
        // upper-half zeroing both come from R()'s 32-bit-write path instead.
        auto a = __ ZeroExtend64(multiplicand);
        auto b = __ ZeroExtend64(multiplier);
        auto product = __ Mul(a, ir::Operand{b}).SetType(ir::ValueType::U64);
        low = __ And(product, ir::Operand{ir::Imm(u64(0xFFFFFFFF))}).SetType(ir::ValueType::U64);
        high = __ LsrImm(product, ir::Imm(32u)).SetType(ir::ValueType::U64);
    } else {
        low = __ Mul(multiplicand, ir::Operand{multiplier}).SetType(ir::ValueType::U64);
        high = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&BmiMulHi64)}},
                             multiplicand,
                             multiplier)
                       .SetType(ir::ValueType::U64);
    }
    R(BmiGprOf(v.vvvv, wide), low);
    R(BmiGprOf(v.reg, wide), high);
}

// PDEP / PEXT r32a/r64a, r32b/r64b, r/m -- VEX.LZ.F2(F3).0F38.W0/W1 F5 /r.
//   SRC1 (the data) is VEX.vvvv, SRC2 (the mask) is r/m.
// NO FLAGS AFFECTED (contract B5).
void X64Decoder::DecodeBmiDepExt(const VexInsn& v, u32 width, bool deposit) {
    const bool wide = width == 64;
    auto data = __ ZeroExtend64(R(BmiGprOf(v.vvvv, wide)));
    auto mask = __ ZeroExtend64(BmiSrc(v, width));
    auto result = __ CallLambda(
                          ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(deposit ? &BmiPdep64
                                                                             : &BmiPext64)}},
                          data,
                          mask)
                          .SetType(ir::ValueType::U64);
    R(BmiGprOf(v.reg, wide), result);
}

bool X64Decoder::DecodeBmi(const VexInsn& v) {
    if (!BmiEnabled()) {
        return false;
    }
    // Contract B1: VEX.LZ.  An L=1 encoding is architecturally #UD, and running
    // it through a 32-bit path would be a silent wrong answer.
    if (v.l) {
        return false;
    }
    // VEX.W selects the operand size; W=1 outside 64-bit mode is #UD.
    if (v.w && !is_64bit) {
        return false;
    }
    const u32 width = v.w ? 64u : 32u;
    switch (v.map) {
        case VexMap::Map0F38:
            switch (v.opcode) {
                case 0xF2:
                    if (v.pp != VexPP::None) {
                        return false;
                    }
                    DecodeBmiAndn(v, width);
                    return true;
                case 0xF3:
                    if (v.pp != VexPP::None) {
                        return false;
                    }
                    switch (v.reg & 7) {
                        case 1:
                        case 2:
                        case 3:
                            DecodeBmiBls(v, width, v.reg & 7);
                            return true;
                        default:
                            // /0 and /4../7 on this opcode are not BMI.
                            return false;
                    }
                case 0xF5:
                    switch (v.pp) {
                        case VexPP::None:
                            DecodeBmiBzhi(v, width);
                            return true;
                        case VexPP::PF3:
                            DecodeBmiDepExt(v, width, false);  // pext
                            return true;
                        case VexPP::PF2:
                            DecodeBmiDepExt(v, width, true);  // pdep
                            return true;
                        default:
                            return false;
                    }
                case 0xF6:
                    if (v.pp != VexPP::PF2) {
                        return false;
                    }
                    DecodeBmiMulx(v, width);
                    return true;
                case 0xF7:
                    switch (v.pp) {
                        case VexPP::None:
                            DecodeBmiBextr(v, width);
                            return true;
                        case VexPP::P66:
                            DecodeBmiShiftX(v, width, 0);  // shlx
                            return true;
                        case VexPP::PF2:
                            DecodeBmiShiftX(v, width, 1);  // shrx
                            return true;
                        case VexPP::PF3:
                            DecodeBmiShiftX(v, width, 2);  // sarx
                            return true;
                    }
                    return false;
                default:
                    return false;
            }
        case VexMap::Map0F3A:
            if (v.opcode == 0xF0 && v.pp == VexPP::PF2) {
                DecodeBmiRorx(v, width);
                return true;
            }
            return false;
        case VexMap::Map0F:
        case VexMap::Invalid:
        default:
            return false;
    }
}

// TZCNT r16/r32/r64, r/m -- F3 0F BC /r.
//
// This is NOT BSF.  For a zero source BSF leaves the destination unchanged and
// sets ZF; TZCNT writes the operand width and CLEARS ZF while setting CF.  The
// two agree for every non-zero source, which is exactly why the alias is safe
// while BMI1 is hidden and becomes a silent wrong answer the moment it is not.
void X64Decoder::DecodeTzcnt(_DInst& insn) {
    if (!BmiEnabled()) {
        DecodeBitScan(insn, false);  // the pre-BMI1 alias
        return;
    }
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    // distorm reports 32-bit operands for the 66-prefixed (16-bit) form; the
    // width has to come from the encoding, exactly as DecodeBitScan does it
    // (pc is already advanced past the instruction here).
    u32 width = op0.size;
    if (width != 64 && insn.size > 2) {
        auto* bytes = reinterpret_cast<u8*>(
                memory->GetPointer(reinterpret_cast<void*>(pc - insn.size)));
        if (bytes) {
            for (u32 i = 0; i + 1 < insn.size && bytes[i] != 0x0F; ++i) {
                if (bytes[i] == 0x66) {
                    width = 16;
                    break;
                }
            }
        }
    }
    const u64 wmask = width == 64 ? UINT64_MAX : ((u64(1) << width) - 1);
    auto src = __ And(ToValue(Src(insn, op1)), ir::Operand{ir::Imm(wmask)})
                       .SetType(GetSize(width));
    auto src64 = __ ZeroExtend64(src);
    auto result = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&BmiTzcnt64)}},
                                src64,
                                __ LoadImm(ir::Imm(u64(width))))
                          .SetType(ir::ValueType::U64);
    // ZF is the result being zero (i.e. bit 0 of the source was set), NOT the
    // source being zero; CF is the source being zero.  SF/OF/PF/AF undefined.
    __ SaveFlags(__ Or(result, ir::Operand{ir::Imm(u64(0))}).SetType(ir::ValueType::U64),
                 ir::Flags::Zero);
    __ SetCarry(__ TestZero(src64));
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
    if (width == 16) {
        // NarrowTo, not And(...).SetType(U16): the And's left operand is a U64
        // value, and a U16 return type would make the backend read it through
        // a W register while writing an X one.
        auto& info = x86_regs_table[op0.index];
        auto off = ToReg(info).GetOffset();
        __ StoreUniform(ir::Uniform{off, ir::ValueType::U16},
                        NarrowTo(result, ir::ValueType::U16));
        return;
    }
    Dst(insn, op0, result);
}

// LZCNT r16/r32/r64, r/m -- F3 0F BD /r.  Same shape as TZCNT (and likewise
// not BSR).  DecodeLzcnt in decoder_alu.cc already produces the right value and
// ZF but never writes CF, which is only invisible while LZCNT stays out of
// CPUID; this handler adds it.
void X64Decoder::DecodeLzcntBmi(_DInst& insn) {
    if (!BmiEnabled()) {
        DecodeLzcnt(insn);
        return;
    }
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    u32 width = op0.size;
    if (width != 64 && insn.size > 2) {
        auto* bytes = reinterpret_cast<u8*>(
                memory->GetPointer(reinterpret_cast<void*>(pc - insn.size)));
        if (bytes) {
            for (u32 i = 0; i + 1 < insn.size && bytes[i] != 0x0F; ++i) {
                if (bytes[i] == 0x66) {
                    width = 16;
                    break;
                }
            }
        }
    }
    const u64 wmask = width == 64 ? UINT64_MAX : ((u64(1) << width) - 1);
    auto src = __ And(ToValue(Src(insn, op1)), ir::Operand{ir::Imm(wmask)})
                       .SetType(GetSize(width));
    auto src64 = __ ZeroExtend64(src);
    auto result = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&BmiLzcnt64)}},
                                src64,
                                __ LoadImm(ir::Imm(u64(width))))
                          .SetType(ir::ValueType::U64);
    __ SaveFlags(__ Or(result, ir::Operand{ir::Imm(u64(0))}).SetType(ir::ValueType::U64),
                 ir::Flags::Zero);
    __ SetCarry(__ TestZero(src64));
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
    if (width == 16) {
        // NarrowTo, not And(...).SetType(U16): the And's left operand is a U64
        // value, and a U16 return type would make the backend read it through
        // a W register while writing an X one.
        auto& info = x86_regs_table[op0.index];
        auto off = ToReg(info).GetOffset();
        __ StoreUniform(ir::Uniform{off, ir::ValueType::U16},
                        NarrowTo(result, ir::ValueType::U16));
        return;
    }
    Dst(insn, op0, result);
}

#undef __

}  // namespace swift::x86
