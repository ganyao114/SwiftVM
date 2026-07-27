// VEX.256 (AVX / AVX2) instruction handlers.
//
// Contract C1: a YMM register is NEVER a single IR value.  ARM64's V registers
// are 128 bits and RegAlloc maps one IR value onto one host register, so every
// 256-bit operation here expands into two independent V128 operations — one on
// bits 127:0 (ThreadContext64::xmms[i], reached through the existing SSE
// helpers) and one on bits 255:128 (ymm_high[i], reached through Agent A's
// YmmHigh* accessors).  That is why this file adds no IR opcode and no backend
// change at all: it reuses the existing V128 VecAdd / VecXor / ... emitters,
// calling each one twice.
//
// A 256-bit handler writes BOTH halves of its destination, so unlike every
// VEX.128 handler it must NOT call ZeroYmmHigh (C3) — the high half is a real
// result here, not a zeroed remnant.
//
// ---------------------------------------------------------------------------
// INTEGRATION (decoder.h / decoder.cc are Agent A's files)
// ---------------------------------------------------------------------------
// This file needs exactly one call site.  DecodeAvx() already parses the VEX
// prefix and rejects L=1; that rejection is the hook:
//
//     const auto vex = DecodeVex();
//     if (!IsVex128(vex)) {
//         return DecodeAvx256(insn, vex);   // L=1, or not a VEX prefix at all
//     }
//
// DecodeAvx256 re-checks vex.valid/vex.l itself, so a non-VEX encoding still
// returns false and the block still traps as FALLBACK.  Nothing on the L=0
// path changes.
//
// Two further things are needed from Agent A:
//
//   1) The declarations below, added to the private section of X64Decoder.
//      Every name is Avx256-prefixed so it cannot collide with the VEX.128
//      work:
//
//        bool DecodeAvx256(_DInst& insn, const VexInfo& vex);
//        VecHalves LoadAvx256Src(_DInst& insn, _Operand& op);
//        void StoreAvx256Dst(_DInst& insn, _Operand& op, ir::Value lo, ir::Value hi);
//        void WriteAvx256(u32 index, ir::Value lo, ir::Value hi);
//        void DecodeAvx256Mov(_DInst& insn);
//        void DecodeAvx256Bitwise(_DInst& insn, VecBitwiseOp op);
//        void DecodeAvx256Int(_DInst& insn, VecIntOp op, u32 lane_bits);
//        void DecodeAvx256MinMax(_DInst& insn, bool max, u32 lane_bits, bool is_signed);
//        void DecodeAvx256Pmovmskb(_DInst& insn);
//        void DecodeAvx256Pshufb(_DInst& insn);
//        void DecodeAvx256BroadcastSS(_DInst& insn);
//
//   2) Case labels in DecodeSwitch for the mnemonics this file adds on top of
//      the VEX.128 set.  Without them DecodeAvx is never reached and these
//      handlers are dead code:
//
//        I_VMOVNTDQ, I_VMOVNTPS, I_VMOVNTPD, I_VLDDQU,
//        I_VXORPS, I_VXORPD, I_VORPS, I_VORPD,
//        I_VANDPS, I_VANDPD, I_VANDNPS, I_VANDNPD,
//        I_VPMINUB, I_VPMINUD, I_VPMAXUB, I_VPMAXUD,
//        I_VPMOVMSKB, I_VPSHUFB, I_VBROADCASTSS
//
//      All of them route to DecodeAvx exactly like the existing entries.  The
//      VMOVNT*/VLDDQU/V*PS/V*PD group is also valid at VEX.128 and would need
//      matching 128-bit handling; until that exists, DecodeAvx's L=0 path
//      correctly declines them (FALLBACK) rather than mis-executing them.
//
// Register operand codes are NOT normalized on this path: DecodeAvx rewrites
// YMM codes to their XMM twins only after the IsVex128 check, so operands here
// may still be R_YMM*.  Everything below therefore goes through VecIndex(),
// which accepts either register file, and never hands a raw op.index to an SSE
// helper — a YMM code reaching XmmRead would silently address ymm_high.
//
// ---------------------------------------------------------------------------
// FAULT SEMANTICS DEVIATION (32-byte accesses) — known, unfixed
// ---------------------------------------------------------------------------
// On x86 a 256-bit load/store is ONE architectural memory operation: it either
// completes or faults, and no architectural state is updated when it faults.
// Splitting it into two 16-byte accesses breaks that indivisibility:
//   * `vmovdqu ymm,[m]` straddling a page boundary where only the second page
//     is unmapped writes the destination's low half before the fault is taken.
//     Real hardware leaves the whole YMM untouched.
//   * `vmovdqu [m],ymm` in the same situation commits the first 16 bytes and
//     then faults — a partial store hardware never produces.
//   * A fault in the upper half reports base+16 as the faulting address rather
//     than base.
// This is observable only for a guest that resumes after a SIGSEGV/SIGBUS
// (userland fault handlers, JIT guard pages, mmap-probing allocators).  It is
// not fixable in the frontend under C1: an exact version needs either a probe
// of both halves before either is committed, or a genuine 32-byte IR memory
// operation (which needs backend work, out of scope per the contract).
//
// The 32-byte alignment requirement of the aligned forms (VMOVDQA / VMOVAPS /
// VMOVAPD / VMOVNTDQ / VMOVNTPS / VMOVNTPD raise #GP on a misaligned operand)
// is likewise not enforced, matching how the existing SSE MOVDQA path already
// ignores the 16-byte rule.  CheckMemoryAlignment(addr, ir::Imm(31)) is the
// mechanism if it ever has to be exact.
//
// The non-temporal hint of the VMOVNT* forms is not observable in SwiftVM and
// degrades to a plain store, exactly as SSE MOVNTDQ already does.

#include "runtime/frontend/x86/decoder_internal.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

X64Decoder::VecHalves X64Decoder::LoadAvx256Src(_DInst& insn, _Operand& op) {
    if (op.type == O_REG) {
        const auto index = VecIndex(static_cast<_RegisterType>(op.index));
        return {XmmRead(XmmOf(index)), YmmHighRead(index)};
    }
    // Fold the address once: FlatAddress emits IR, and both halves must share
    // the same base even in the RIP-relative and scaled-index forms.  op.size
    // is deliberately ignored — for the AVX2 packed-integer opcodes this
    // distorm snapshot reports a 128-bit memory operand even when VEX.L=1, so
    // the prefix's L bit (already checked by the caller) is the only truth.
    auto addr = FlatAddress(insn, op);
    auto lo = __ LoadMemory(ir::Operand{addr}).SetType(ir::ValueType::V128);
    auto hi = __ LoadMemory(ir::Operand{__ Add(addr, ir::Operand{ir::Imm(u64(16))})})
                      .SetType(ir::ValueType::V128);
    return {lo, hi};
}

void X64Decoder::StoreAvx256Dst(_DInst& insn, _Operand& op, ir::Value lo, ir::Value hi) {
    auto addr = FlatAddress(insn, op);
    // Plain (non-TSO) accesses, matching the existing 128-bit DecodeMovVec:
    // vector memory never participates in the lock/atomic paths, and ordering
    // the two halves individually would not restore the 32-byte indivisibility
    // the split already gave up.
    __ StoreMemory(ir::Operand{addr}, lo);
    __ StoreMemory(ir::Operand{__ Add(addr, ir::Operand{ir::Imm(u64(16))})}, hi);
}

void X64Decoder::WriteAvx256(u32 index, ir::Value lo, ir::Value hi) {
    XmmWrite(XmmOf(index), lo.SetType(ir::ValueType::V128));
    YmmHighWrite(index, hi.SetType(ir::ValueType::V128));
}

// vmovdqu / vmovdqa / vmovups / vmovaps / vmovupd / vmovapd / vmovntdq /
// vmovntps / vmovntpd / vlddqu, 256-bit forms.
void X64Decoder::DecodeAvx256Mov(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    if (op0.type == O_REG) {
        auto halves = LoadAvx256Src(insn, op1);
        WriteAvx256(VecIndex(static_cast<_RegisterType>(op0.index)), halves.lo, halves.hi);
    } else {
        // Named locals, not nested calls: argument evaluation order is
        // unspecified, and these two loads must land in the IR in a fixed
        // order for the emitted block to be reproducible.
        const auto src = VecIndex(static_cast<_RegisterType>(op1.index));
        auto lo = XmmRead(XmmOf(src));
        auto hi = YmmHighRead(src);
        StoreAvx256Dst(insn, op0, lo, hi);
    }
}

// vpand / vpandn / vpor / vpxor and the vandps/vandpd/... FP-typed aliases.
void X64Decoder::DecodeAvx256Bitwise(_DInst& insn, VecBitwiseOp op) {
    const auto dst = VecIndex(static_cast<_RegisterType>(insn.ops[0].index));
    // VEX is non-destructive: src1 comes from ops[1] (VEX.vvvv), not from dst.
    auto a = LoadAvx256Src(insn, insn.ops[1]);
    auto b = LoadAvx256Src(insn, insn.ops[2]);
    ir::Value lo, hi;
    switch (op) {
        case VecBitwiseOp::Xor:
            lo = __ VecXor(a.lo, b.lo);
            hi = __ VecXor(a.hi, b.hi);
            break;
        case VecBitwiseOp::Or:
            lo = __ VecOr(a.lo, b.lo);
            hi = __ VecOr(a.hi, b.hi);
            break;
        case VecBitwiseOp::And:
            lo = __ VecAnd(a.lo, b.lo);
            hi = __ VecAnd(a.hi, b.hi);
            break;
        case VecBitwiseOp::AndNot:
            // VPANDN: dst = (NOT src1) AND src2; VecAndNot(x, y) = x AND NOT y.
            lo = __ VecAndNot(b.lo, a.lo);
            hi = __ VecAndNot(b.hi, a.hi);
            break;
    }
    WriteAvx256(dst, lo, hi);
}

// vpadd* / vpsub* / vpcmpeq* / vpcmpgt*.  Every one of these is lane-wise, so
// the two 128-bit halves are genuinely independent: there is no cross-lane
// carry or comparison for the split to lose.
void X64Decoder::DecodeAvx256Int(_DInst& insn, VecIntOp op, u32 lane_bits) {
    const auto dst = VecIndex(static_cast<_RegisterType>(insn.ops[0].index));
    auto a = LoadAvx256Src(insn, insn.ops[1]);
    auto b = LoadAvx256Src(insn, insn.ops[2]);
    const auto lanes = ir::Imm(lane_bits);
    ir::Value lo, hi;
    switch (op) {
        case VecIntOp::Add:
            lo = __ VecAdd(a.lo, b.lo, lanes);
            hi = __ VecAdd(a.hi, b.hi, lanes);
            break;
        case VecIntOp::Sub:
            lo = __ VecSub(a.lo, b.lo, lanes);
            hi = __ VecSub(a.hi, b.hi, lanes);
            break;
        case VecIntOp::CmpEq:
            lo = __ VecCmpEq(a.lo, b.lo, lanes);
            hi = __ VecCmpEq(a.hi, b.hi, lanes);
            break;
        case VecIntOp::CmpGt:
            lo = __ VecCmpGt(a.lo, b.lo, lanes);
            hi = __ VecCmpGt(a.hi, b.hi, lanes);
            break;
    }
    WriteAvx256(dst, lo, hi);
}

// vpminub / vpminud / vpmaxub / vpmaxud.
void X64Decoder::DecodeAvx256MinMax(_DInst& insn, bool max, u32 lane_bits, bool is_signed) {
    const auto dst = VecIndex(static_cast<_RegisterType>(insn.ops[0].index));
    auto a = LoadAvx256Src(insn, insn.ops[1]);
    auto b = LoadAvx256Src(insn, insn.ops[2]);
    const auto lanes = ir::Imm(lane_bits);
    const auto sign = ir::Imm(is_signed ? 1u : 0u);
    auto lo = max ? __ VecMax(a.lo, b.lo, lanes, sign) : __ VecMin(a.lo, b.lo, lanes, sign);
    auto hi = max ? __ VecMax(a.hi, b.hi, lanes, sign) : __ VecMin(a.hi, b.hi, lanes, sign);
    WriteAvx256(dst, lo, hi);
}

// vpmovmskb r32, ymm: 32 sign bits, the low half in [15:0] and the high half
// in [31:16].  This is the one operation here whose halves are not
// independent — the two 16-bit masks have to be recombined.
void X64Decoder::DecodeAvx256Pmovmskb(_DInst& insn) {
    // The source comes from the raw ModRM.rm, NOT insn.ops[1]: this distorm
    // snapshot's VEX VPMOVMSKB table entry has no operand descriptors, so it
    // reports ModRM.reg for both operands (see VexRmRegister). Reading ops[1]
    // returns another register's mask whenever the destination GPR number
    // differs from the source vector register number — verified against
    // Rosetta, which decodes `vpmovmskb ecx, ymm5` correctly while distorm
    // renders it as `VPMOVMSKB RCX, XMM1`.
    const u32 rm = VexRmRegister();
    const auto src = rm != UINT32_MAX
                             ? rm
                             : VecIndex(static_cast<_RegisterType>(insn.ops[1].index));
    auto lo = __ VecMovMask(XmmRead(XmmOf(src)), ir::Imm(8u)).SetType(ir::ValueType::U32);
    auto hi = __ VecMovMask(YmmHighRead(src), ir::Imm(8u)).SetType(ir::ValueType::U32);
    auto shifted = __ LslImm(hi, ir::Imm(u64(16))).SetType(ir::ValueType::U32);
    Dst(insn, insn.ops[0], __ Or(lo, ir::Operand{shifted}).SetType(ir::ValueType::U32));
}

// vpshufb ymm1, ymm2, ymm3/m256.  AVX2 shuffles WITHIN each 128-bit lane (the
// index is bits [3:0] of the control byte and bit 7 zeroes the result byte),
// so the 128-bit VecTableLookup8 is exact per half; there is no cross-lane
// behaviour to emulate.
void X64Decoder::DecodeAvx256Pshufb(_DInst& insn) {
    const auto dst = VecIndex(static_cast<_RegisterType>(insn.ops[0].index));
    auto a = LoadAvx256Src(insn, insn.ops[1]);
    auto b = LoadAvx256Src(insn, insn.ops[2]);
    auto lo = __ VecTableLookup8(a.lo, b.lo);
    auto hi = __ VecTableLookup8(a.hi, b.hi);
    WriteAvx256(dst, lo, hi);
}

// vbroadcastss ymm, xmm/m32: the source dword fills all eight lanes, so both
// halves receive the identical V128 value.
void X64Decoder::DecodeAvx256BroadcastSS(_DInst& insn) {
    const auto dst = VecIndex(static_cast<_RegisterType>(insn.ops[0].index));
    auto& op1 = insn.ops[1];
    ir::Value dword;
    if (op1.type == O_REG) {
        // AVX2 register form: the low dword of the source's LOW half.
        dword = __ And(XmmLo(XmmOf(VecIndex(static_cast<_RegisterType>(op1.index)))),
                       ir::Operand{ir::Imm(0xFFFFFFFFull)});
    } else {
        dword = __ ZeroExtend64(
                __ LoadMemory(ir::Operand{FlatAddress(insn, op1)}).SetType(ir::ValueType::U32));
    }
    // Build the 64-bit lane pair first: VecDup64 only replicates qwords.
    auto pair = __ Or(dword, ir::Operand{__ LslImm(dword, ir::Imm(u64(32)))});
    auto value = __ VecDup64(pair).SetType(ir::ValueType::V128);
    WriteAvx256(dst, value, value);
}

bool X64Decoder::DecodeAvx256(_DInst& insn, const VexInfo& vex) {
    // Called from DecodeAvx's !IsVex128 branch, which is also taken when the
    // instruction carries no VEX prefix at all; re-check rather than assume.
    // Returning false traps the block as FALLBACK, the correct conservative
    // answer for a 256-bit form with no handler.
    if (!AvxEnabled() || !vex.valid || !vex.l) {
        return false;
    }
    switch (insn.opcode) {
        // ---- two-operand forms ------------------------------------------
        // 256-bit moves (vmovdqu 1136 / vmovdqa 576 / vmovups 346 /
        // vmovntdq 200 / vmovaps 8 in the test-fixture census).
        case I_VMOVDQU:
        case I_VMOVDQA:
        case I_VMOVUPS:
        case I_VMOVAPS:
        case I_VMOVUPD:
        case I_VMOVAPD:
        case I_VMOVNTDQ:
        case I_VMOVNTPS:
        case I_VMOVNTPD:
        case I_VLDDQU:
            // One side must be a register; two memory operands would mean
            // distorm produced a shape this handler cannot read.
            if (insn.ops[0].type != O_REG && insn.ops[1].type != O_REG) {
                return false;
            }
            DecodeAvx256Mov(insn);
            return true;
        case I_VPMOVMSKB:  // 984
            if (insn.ops[0].type != O_REG || insn.ops[1].type != O_REG) {
                return false;
            }
            DecodeAvx256Pmovmskb(insn);
            return true;
        case I_VBROADCASTSS:  // 2
            if (insn.ops[0].type != O_REG) {
                return false;
            }
            DecodeAvx256BroadcastSS(insn);
            return true;
        default:
            break;
    }
    // Everything remaining is a 3-operand non-destructive form.  Verify the
    // shape distorm produced rather than trusting it: a 2-operand result here
    // would make ops[1] the r/m operand and ops[2] garbage.  Same check as the
    // VEX.128 path, and it matters more here because this snapshot has no
    // 256-bit table entry for the packed-integer opcodes.
    if (insn.ops[0].type != O_REG || insn.ops[1].type != O_REG || insn.ops[2].type == O_NONE) {
        return false;
    }
    switch (insn.opcode) {
        // ---- bitwise (vpandn 388 / vpand 108 / vpor 88 / vpxor 82) -------
        case I_VPXOR:
        case I_VXORPS:
        case I_VXORPD:
            DecodeAvx256Bitwise(insn, VecBitwiseOp::Xor);
            return true;
        case I_VPOR:
        case I_VORPS:
        case I_VORPD:
            DecodeAvx256Bitwise(insn, VecBitwiseOp::Or);
            return true;
        case I_VPAND:
        case I_VANDPS:
        case I_VANDPD:
            DecodeAvx256Bitwise(insn, VecBitwiseOp::And);
            return true;
        case I_VPANDN:
        case I_VANDNPS:
        case I_VANDNPD:
            DecodeAvx256Bitwise(insn, VecBitwiseOp::AndNot);
            return true;
        // ---- lane-wise integer add/sub (vpaddb 460 / vpsubb 92) ----------
        case I_VPADDB:
            DecodeAvx256Int(insn, VecIntOp::Add, 8);
            return true;
        case I_VPADDW:
            DecodeAvx256Int(insn, VecIntOp::Add, 16);
            return true;
        case I_VPADDD:
            DecodeAvx256Int(insn, VecIntOp::Add, 32);
            return true;
        case I_VPADDQ:
            DecodeAvx256Int(insn, VecIntOp::Add, 64);
            return true;
        case I_VPSUBB:
            DecodeAvx256Int(insn, VecIntOp::Sub, 8);
            return true;
        case I_VPSUBW:
            DecodeAvx256Int(insn, VecIntOp::Sub, 16);
            return true;
        case I_VPSUBD:
            DecodeAvx256Int(insn, VecIntOp::Sub, 32);
            return true;
        case I_VPSUBQ:
            DecodeAvx256Int(insn, VecIntOp::Sub, 64);
            return true;
        // ---- compares (vpcmpeqb 1372 / vpcmpeqd 248 / vpcmpgtb 184) ------
        case I_VPCMPEQB:
            DecodeAvx256Int(insn, VecIntOp::CmpEq, 8);
            return true;
        case I_VPCMPEQW:
            DecodeAvx256Int(insn, VecIntOp::CmpEq, 16);
            return true;
        case I_VPCMPEQD:
            DecodeAvx256Int(insn, VecIntOp::CmpEq, 32);
            return true;
        case I_VPCMPGTB:
            DecodeAvx256Int(insn, VecIntOp::CmpGt, 8);
            return true;
        case I_VPCMPGTW:
            DecodeAvx256Int(insn, VecIntOp::CmpGt, 16);
            return true;
        case I_VPCMPGTD:
            DecodeAvx256Int(insn, VecIntOp::CmpGt, 32);
            return true;
        // ---- unsigned min/max (vpminub 274 / vpminud 34) ----------------
        case I_VPMINUB:
            DecodeAvx256MinMax(insn, false, 8, false);
            return true;
        case I_VPMINUD:
            DecodeAvx256MinMax(insn, false, 32, false);
            return true;
        case I_VPMAXUB:
            DecodeAvx256MinMax(insn, true, 8, false);
            return true;
        case I_VPMAXUD:
            DecodeAvx256MinMax(insn, true, 32, false);
            return true;
        case I_VPSHUFB:  // 2
            DecodeAvx256Pshufb(insn);
            return true;
        default:
            // Any other 256-bit form is unimplemented: decline so the caller
            // traps the block instead of letting a VEX.128 path produce a
            // half-width result.
            return false;
    }
}

#undef __

}  // namespace swift::x86
