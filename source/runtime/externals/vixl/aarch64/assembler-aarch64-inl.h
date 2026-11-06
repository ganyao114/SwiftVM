// Copyright 2015, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


#ifndef VIXL_AARCH64_ASSEMBLER_AARCH64_INL_H_
#define VIXL_AARCH64_ASSEMBLER_AARCH64_INL_H_

#include "assembler-aarch64.h"

namespace vixl {
namespace aarch64 {
// One exact implementation, moved from assembler-aarch64.cc so the actual
// operand form/opcode/width folds into the single borrowed-word write.

VIXL_ALWAYS_INLINE void Assembler::br(const Register& xn) {
  VIXL_ASSERT(xn.Is64Bits());
  Emit(BR | Rn(xn));
}

VIXL_ALWAYS_INLINE void Assembler::blr(const Register& xn) {
  VIXL_ASSERT(xn.Is64Bits());
  Emit(BLR | Rn(xn));
}

VIXL_ALWAYS_INLINE void Assembler::ret(const Register& xn) {
  VIXL_ASSERT(xn.Is64Bits());
  Emit(RET | Rn(xn));
}

VIXL_ALWAYS_INLINE void Assembler::b(int64_t imm26) { Emit(B | ImmUncondBranch(imm26)); }

VIXL_ALWAYS_INLINE void Assembler::b(int64_t imm19, Condition cond) {
  Emit(B_cond | ImmCondBranch(imm19) | cond);
}

VIXL_ALWAYS_INLINE void Assembler::cbz(const Register& rt, int64_t imm19) {
  Emit(SF(rt) | CBZ | ImmCmpBranch(imm19) | Rt(rt));
}

VIXL_ALWAYS_INLINE void Assembler::cbnz(const Register& rt, int64_t imm19) {
  Emit(SF(rt) | CBNZ | ImmCmpBranch(imm19) | Rt(rt));
}

VIXL_ALWAYS_INLINE void Assembler::tbz(const Register& rt, unsigned bit_pos, int64_t imm14) {
  VIXL_ASSERT(rt.Is64Bits() || (rt.Is32Bits() && (bit_pos < kWRegSize)));
  Emit(TBZ | ImmTestBranchBit(bit_pos) | ImmTestBranch(imm14) | Rt(rt));
}

VIXL_ALWAYS_INLINE void Assembler::tbnz(const Register& rt, unsigned bit_pos, int64_t imm14) {
  VIXL_ASSERT(rt.Is64Bits() || (rt.Is32Bits() && (bit_pos < kWRegSize)));
  Emit(TBNZ | ImmTestBranchBit(bit_pos) | ImmTestBranch(imm14) | Rt(rt));
}

VIXL_ALWAYS_INLINE void Assembler::adr(const Register& xd, int64_t imm21) {
  VIXL_ASSERT(xd.Is64Bits());
  Emit(ADR | ImmPCRelAddress(imm21) | Rd(xd));
}

VIXL_ALWAYS_INLINE void Assembler::add(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  AddSub(rd, rn, operand, LeaveFlags, ADD);
}

VIXL_ALWAYS_INLINE void Assembler::adds(const Register& rd,
                     const Register& rn,
                     const Operand& operand) {
  AddSub(rd, rn, operand, SetFlags, ADD);
}

VIXL_ALWAYS_INLINE void Assembler::cmn(const Register& rn, const Operand& operand) {
  Register zr = AppropriateZeroRegFor(rn);
  adds(zr, rn, operand);
}

VIXL_ALWAYS_INLINE void Assembler::sub(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  AddSub(rd, rn, operand, LeaveFlags, SUB);
}

VIXL_ALWAYS_INLINE void Assembler::subs(const Register& rd,
                     const Register& rn,
                     const Operand& operand) {
  AddSub(rd, rn, operand, SetFlags, SUB);
}

VIXL_ALWAYS_INLINE void Assembler::cmp(const Register& rn, const Operand& operand) {
  Register zr = AppropriateZeroRegFor(rn);
  subs(zr, rn, operand);
}

VIXL_ALWAYS_INLINE void Assembler::neg(const Register& rd, const Operand& operand) {
  Register zr = AppropriateZeroRegFor(rd);
  sub(rd, zr, operand);
}

VIXL_ALWAYS_INLINE void Assembler::negs(const Register& rd, const Operand& operand) {
  Register zr = AppropriateZeroRegFor(rd);
  subs(rd, zr, operand);
}

VIXL_ALWAYS_INLINE void Assembler::adc(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  AddSubWithCarry(rd, rn, operand, LeaveFlags, ADC);
}

VIXL_ALWAYS_INLINE void Assembler::adcs(const Register& rd,
                     const Register& rn,
                     const Operand& operand) {
  AddSubWithCarry(rd, rn, operand, SetFlags, ADC);
}

VIXL_ALWAYS_INLINE void Assembler::sbc(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  AddSubWithCarry(rd, rn, operand, LeaveFlags, SBC);
}

VIXL_ALWAYS_INLINE void Assembler::sbcs(const Register& rd,
                     const Register& rn,
                     const Operand& operand) {
  AddSubWithCarry(rd, rn, operand, SetFlags, SBC);
}

VIXL_ALWAYS_INLINE void Assembler::rmif(const Register& xn, unsigned rotation, StatusFlags flags) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kFlagM));
  VIXL_ASSERT(xn.Is64Bits());
  Emit(RMIF | Rn(xn) | ImmRMIFRotation(rotation) | Nzcv(flags));
}

VIXL_ALWAYS_INLINE void Assembler::setf8(const Register& rn) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kFlagM));
  Emit(SETF8 | Rn(rn));
}

VIXL_ALWAYS_INLINE void Assembler::setf16(const Register& rn) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kFlagM));
  Emit(SETF16 | Rn(rn));
}

VIXL_ALWAYS_INLINE void Assembler::and_(const Register& rd,
                     const Register& rn,
                     const Operand& operand) {
  Logical(rd, rn, operand, AND);
}

VIXL_ALWAYS_INLINE void Assembler::ands(const Register& rd,
                     const Register& rn,
                     const Operand& operand) {
  Logical(rd, rn, operand, ANDS);
}

VIXL_ALWAYS_INLINE void Assembler::tst(const Register& rn, const Operand& operand) {
  ands(AppropriateZeroRegFor(rn), rn, operand);
}

VIXL_ALWAYS_INLINE void Assembler::bic(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  Logical(rd, rn, operand, BIC);
}

VIXL_ALWAYS_INLINE void Assembler::orr(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  Logical(rd, rn, operand, ORR);
}

VIXL_ALWAYS_INLINE void Assembler::orn(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  Logical(rd, rn, operand, ORN);
}

VIXL_ALWAYS_INLINE void Assembler::eor(const Register& rd,
                    const Register& rn,
                    const Operand& operand) {
  Logical(rd, rn, operand, EOR);
}

VIXL_ALWAYS_INLINE void Assembler::lslv(const Register& rd,
                     const Register& rn,
                     const Register& rm) {
  VIXL_ASSERT(rd.GetSizeInBits() == rn.GetSizeInBits());
  VIXL_ASSERT(rd.GetSizeInBits() == rm.GetSizeInBits());
  Emit(SF(rd) | LSLV | Rm(rm) | Rn(rn) | Rd(rd));
}

VIXL_ALWAYS_INLINE void Assembler::lsrv(const Register& rd,
                     const Register& rn,
                     const Register& rm) {
  VIXL_ASSERT(rd.GetSizeInBits() == rn.GetSizeInBits());
  VIXL_ASSERT(rd.GetSizeInBits() == rm.GetSizeInBits());
  Emit(SF(rd) | LSRV | Rm(rm) | Rn(rn) | Rd(rd));
}

VIXL_ALWAYS_INLINE void Assembler::asrv(const Register& rd,
                     const Register& rn,
                     const Register& rm) {
  VIXL_ASSERT(rd.GetSizeInBits() == rn.GetSizeInBits());
  VIXL_ASSERT(rd.GetSizeInBits() == rm.GetSizeInBits());
  Emit(SF(rd) | ASRV | Rm(rm) | Rn(rn) | Rd(rd));
}

VIXL_ALWAYS_INLINE void Assembler::rorv(const Register& rd,
                     const Register& rn,
                     const Register& rm) {
  VIXL_ASSERT(rd.GetSizeInBits() == rn.GetSizeInBits());
  VIXL_ASSERT(rd.GetSizeInBits() == rm.GetSizeInBits());
  Emit(SF(rd) | RORV | Rm(rm) | Rn(rn) | Rd(rd));
}

VIXL_ALWAYS_INLINE void Assembler::bfm(const Register& rd,
                    const Register& rn,
                    unsigned immr,
                    unsigned imms) {
  VIXL_ASSERT(rd.GetSizeInBits() == rn.GetSizeInBits());
  Instr N = SF(rd) >> (kSFOffset - kBitfieldNOffset);
  Emit(SF(rd) | BFM | N | ImmR(immr, rd.GetSizeInBits()) |
       ImmS(imms, rn.GetSizeInBits()) | Rn(rn) | Rd(rd));
}

VIXL_ALWAYS_INLINE void Assembler::sbfm(const Register& rd,
                     const Register& rn,
                     unsigned immr,
                     unsigned imms) {
  VIXL_ASSERT(rd.Is64Bits() || rn.Is32Bits());
  Instr N = SF(rd) >> (kSFOffset - kBitfieldNOffset);
  Emit(SF(rd) | SBFM | N | ImmR(immr, rd.GetSizeInBits()) |
       ImmS(imms, rn.GetSizeInBits()) | Rn(rn) | Rd(rd));
}

VIXL_ALWAYS_INLINE void Assembler::ubfm(const Register& rd,
                     const Register& rn,
                     unsigned immr,
                     unsigned imms) {
  VIXL_ASSERT(rd.GetSizeInBits() == rn.GetSizeInBits());
  Instr N = SF(rd) >> (kSFOffset - kBitfieldNOffset);
  Emit(SF(rd) | UBFM | N | ImmR(immr, rd.GetSizeInBits()) |
       ImmS(imms, rn.GetSizeInBits()) | Rn(rn) | Rd(rd));
}

VIXL_ALWAYS_INLINE void Assembler::extr(const Register& rd,
                     const Register& rn,
                     const Register& rm,
                     unsigned lsb) {
  VIXL_ASSERT(rd.GetSizeInBits() == rn.GetSizeInBits());
  VIXL_ASSERT(rd.GetSizeInBits() == rm.GetSizeInBits());
  Instr N = SF(rd) >> (kSFOffset - kBitfieldNOffset);
  Emit(SF(rd) | EXTR | N | Rm(rm) | ImmS(lsb, rn.GetSizeInBits()) | Rn(rn) |
       Rd(rd));
}

VIXL_ALWAYS_INLINE void Assembler::csel(const Register& rd,
                     const Register& rn,
                     const Register& rm,
                     Condition cond) {
  ConditionalSelect(rd, rn, rm, cond, CSEL);
}

VIXL_ALWAYS_INLINE void Assembler::csinc(const Register& rd,
                      const Register& rn,
                      const Register& rm,
                      Condition cond) {
  ConditionalSelect(rd, rn, rm, cond, CSINC);
}

VIXL_ALWAYS_INLINE void Assembler::csinv(const Register& rd,
                      const Register& rn,
                      const Register& rm,
                      Condition cond) {
  ConditionalSelect(rd, rn, rm, cond, CSINV);
}

VIXL_ALWAYS_INLINE void Assembler::csneg(const Register& rd,
                      const Register& rn,
                      const Register& rm,
                      Condition cond) {
  ConditionalSelect(rd, rn, rm, cond, CSNEG);
}

VIXL_ALWAYS_INLINE void Assembler::cset(const Register& rd, Condition cond) {
  VIXL_ASSERT((cond != al) && (cond != nv));
  Register zr = AppropriateZeroRegFor(rd);
  csinc(rd, zr, zr, InvertCondition(cond));
}

VIXL_ALWAYS_INLINE void Assembler::ConditionalSelect(const Register& rd,
                                  const Register& rn,
                                  const Register& rm,
                                  Condition cond,
                                  ConditionalSelectOp op) {
  VIXL_ASSERT(rd.GetSizeInBits() == rn.GetSizeInBits());
  VIXL_ASSERT(rd.GetSizeInBits() == rm.GetSizeInBits());
  Emit(SF(rd) | op | Rm(rm) | Cond(cond) | Rn(rn) | Rd(rd));
}

VIXL_ALWAYS_INLINE void Assembler::DataProcessing3Source(const Register& rd,
                                      const Register& rn,
                                      const Register& rm,
                                      const Register& ra,
                                      DataProcessing3SourceOp op) {
  Emit(SF(rd) | op | Rm(rm) | Ra(ra) | Rn(rn) | Rd(rd));
}

VIXL_ALWAYS_INLINE void Assembler::mul(const Register& rd,
                    const Register& rn,
                    const Register& rm) {
  VIXL_ASSERT(AreSameSizeAndType(rd, rn, rm));
  DataProcessing3Source(rd, rn, rm, AppropriateZeroRegFor(rd), MADD);
}

VIXL_ALWAYS_INLINE void Assembler::madd(const Register& rd,
                     const Register& rn,
                     const Register& rm,
                     const Register& ra) {
  DataProcessing3Source(rd, rn, rm, ra, MADD);
}

VIXL_ALWAYS_INLINE void Assembler::smulh(const Register& xd,
                      const Register& xn,
                      const Register& xm) {
  VIXL_ASSERT(xd.Is64Bits() && xn.Is64Bits() && xm.Is64Bits());
  DataProcessing3Source(xd, xn, xm, xzr, SMULH_x);
}

VIXL_ALWAYS_INLINE void Assembler::umulh(const Register& xd,
                      const Register& xn,
                      const Register& xm) {
  VIXL_ASSERT(xd.Is64Bits() && xn.Is64Bits() && xm.Is64Bits());
  DataProcessing3Source(xd, xn, xm, xzr, UMULH_x);
}

VIXL_ALWAYS_INLINE void Assembler::rbit(const Register& rd, const Register& rn) {
  DataProcessing1Source(rd, rn, RBIT);
}

VIXL_ALWAYS_INLINE void Assembler::rev16(const Register& rd, const Register& rn) {
  DataProcessing1Source(rd, rn, REV16);
}

VIXL_ALWAYS_INLINE void Assembler::rev(const Register& rd, const Register& rn) {
  DataProcessing1Source(rd, rn, rd.Is64Bits() ? REV_x : REV_w);
}

VIXL_ALWAYS_INLINE void Assembler::clz(const Register& rd, const Register& rn) {
  DataProcessing1Source(rd, rn, CLZ);
}

VIXL_ALWAYS_INLINE void Assembler::ldp(const CPURegister& rt,
                    const CPURegister& rt2,
                    const MemOperand& src) {
  LoadStorePair(rt, rt2, src, LoadPairOpFor(rt, rt2));
}

VIXL_ALWAYS_INLINE void Assembler::stp(const CPURegister& rt,
                    const CPURegister& rt2,
                    const MemOperand& dst) {
  LoadStorePair(rt, rt2, dst, StorePairOpFor(rt, rt2));
}

VIXL_ALWAYS_INLINE void Assembler::LoadStorePair(const CPURegister& rt,
                              const CPURegister& rt2,
                              const MemOperand& addr,
                              LoadStorePairOp op) {
  VIXL_ASSERT(CPUHas(rt, rt2));

  // 'rt' and 'rt2' can only be aliased for stores.
  VIXL_ASSERT(((op & LoadStorePairLBit) == 0) || !rt.Is(rt2));
  VIXL_ASSERT(AreSameSizeAndType(rt, rt2));
  VIXL_ASSERT(IsImmLSPair(addr.GetOffset(), CalcLSPairDataSize(op)));

  int offset = static_cast<int>(addr.GetOffset());
  Instr memop = op | Rt(rt) | Rt2(rt2) | RnSP(addr.GetBaseRegister()) |
                ImmLSPair(offset, CalcLSPairDataSize(op));

  Instr addrmodeop;
  if (addr.IsImmediateOffset()) {
    addrmodeop = LoadStorePairOffsetFixed;
  } else {
    if (addr.IsPreIndex()) {
      addrmodeop = LoadStorePairPreIndexFixed;
    } else {
      VIXL_ASSERT(addr.IsPostIndex());
      addrmodeop = LoadStorePairPostIndexFixed;
    }
  }
  Emit(addrmodeop | memop);
}

VIXL_ALWAYS_INLINE void Assembler::ldrb(const Register& rt,
                     const MemOperand& src,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, src, LDRB_w, option);
}

VIXL_ALWAYS_INLINE void Assembler::strb(const Register& rt,
                     const MemOperand& dst,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, dst, STRB_w, option);
}

VIXL_ALWAYS_INLINE void Assembler::ldrsb(const Register& rt,
                      const MemOperand& src,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, src, rt.Is64Bits() ? LDRSB_x : LDRSB_w, option);
}

VIXL_ALWAYS_INLINE void Assembler::ldrh(const Register& rt,
                     const MemOperand& src,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, src, LDRH_w, option);
}

VIXL_ALWAYS_INLINE void Assembler::strh(const Register& rt,
                     const MemOperand& dst,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, dst, STRH_w, option);
}

VIXL_ALWAYS_INLINE void Assembler::ldrsh(const Register& rt,
                      const MemOperand& src,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, src, rt.Is64Bits() ? LDRSH_x : LDRSH_w, option);
}

VIXL_ALWAYS_INLINE void Assembler::ldr(const CPURegister& rt,
                    const MemOperand& src,
                    LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, src, LoadOpFor(rt), option);
}

VIXL_ALWAYS_INLINE void Assembler::str(const CPURegister& rt,
                    const MemOperand& dst,
                    LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(rt, dst, StoreOpFor(rt), option);
}

VIXL_ALWAYS_INLINE void Assembler::ldrsw(const Register& xt,
                      const MemOperand& src,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(xt.Is64Bits());
  VIXL_ASSERT(option != RequireUnscaledOffset);
  VIXL_ASSERT(option != PreferUnscaledOffset);
  LoadStore(xt, src, LDRSW_x, option);
}

VIXL_ALWAYS_INLINE void Assembler::ldurb(const Register& rt,
                      const MemOperand& src,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, src, LDRB_w, option);
}

VIXL_ALWAYS_INLINE void Assembler::sturb(const Register& rt,
                      const MemOperand& dst,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, dst, STRB_w, option);
}

VIXL_ALWAYS_INLINE void Assembler::ldursb(const Register& rt,
                       const MemOperand& src,
                       LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, src, rt.Is64Bits() ? LDRSB_x : LDRSB_w, option);
}

VIXL_ALWAYS_INLINE void Assembler::ldurh(const Register& rt,
                      const MemOperand& src,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, src, LDRH_w, option);
}

VIXL_ALWAYS_INLINE void Assembler::sturh(const Register& rt,
                      const MemOperand& dst,
                      LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, dst, STRH_w, option);
}

VIXL_ALWAYS_INLINE void Assembler::ldursh(const Register& rt,
                       const MemOperand& src,
                       LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, src, rt.Is64Bits() ? LDRSH_x : LDRSH_w, option);
}

VIXL_ALWAYS_INLINE void Assembler::ldur(const CPURegister& rt,
                     const MemOperand& src,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, src, LoadOpFor(rt), option);
}

VIXL_ALWAYS_INLINE void Assembler::stur(const CPURegister& rt,
                     const MemOperand& dst,
                     LoadStoreScalingOption option) {
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(rt, dst, StoreOpFor(rt), option);
}

VIXL_ALWAYS_INLINE void Assembler::ldursw(const Register& xt,
                       const MemOperand& src,
                       LoadStoreScalingOption option) {
  VIXL_ASSERT(xt.Is64Bits());
  VIXL_ASSERT(option != RequireScaledOffset);
  VIXL_ASSERT(option != PreferScaledOffset);
  LoadStore(xt, src, LDRSW_x, option);
}

VIXL_ALWAYS_INLINE void Assembler::ldrsw(const Register& rt, int64_t imm19) {
  Emit(LDRSW_x_lit | ImmLLiteral(imm19) | Rt(rt));
}

VIXL_ALWAYS_INLINE void Assembler::ldr(const CPURegister& rt, int64_t imm19) {
  VIXL_ASSERT(CPUHas(rt));
  LoadLiteralOp op = LoadLiteralOpFor(rt);
  Emit(op | ImmLLiteral(imm19) | Rt(rt));
}

VIXL_ALWAYS_INLINE void Assembler::stlrb(const Register& rt, const MemOperand& dst) {
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.GetOffset() == 0));
  Emit(STLRB_w | Rs_mask | Rt(rt) | Rt2_mask | RnSP(dst.GetBaseRegister()));
}

VIXL_ALWAYS_INLINE void Assembler::stlurb(const Register& rt, const MemOperand& dst) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kRCpc, CPUFeatures::kRCpcImm));
  VIXL_ASSERT(dst.IsImmediateOffset() && IsImmLSUnscaled(dst.GetOffset()));

  Instr base = RnSP(dst.GetBaseRegister());
  int64_t offset = dst.GetOffset();
  Emit(STLURB | Rt(rt) | base | ImmLS(static_cast<int>(offset)));
}

VIXL_ALWAYS_INLINE void Assembler::stlrh(const Register& rt, const MemOperand& dst) {
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.GetOffset() == 0));
  Emit(STLRH_w | Rs_mask | Rt(rt) | Rt2_mask | RnSP(dst.GetBaseRegister()));
}

VIXL_ALWAYS_INLINE void Assembler::stlurh(const Register& rt, const MemOperand& dst) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kRCpc, CPUFeatures::kRCpcImm));
  VIXL_ASSERT(dst.IsImmediateOffset() && IsImmLSUnscaled(dst.GetOffset()));

  Instr base = RnSP(dst.GetBaseRegister());
  int64_t offset = dst.GetOffset();
  Emit(STLURH | Rt(rt) | base | ImmLS(static_cast<int>(offset)));
}

VIXL_ALWAYS_INLINE void Assembler::stlr(const Register& rt, const MemOperand& dst) {
  VIXL_ASSERT(dst.IsImmediateOffset() && (dst.GetOffset() == 0));
  LoadStoreExclusive op = rt.Is64Bits() ? STLR_x : STLR_w;
  Emit(op | Rs_mask | Rt(rt) | Rt2_mask | RnSP(dst.GetBaseRegister()));
}

VIXL_ALWAYS_INLINE void Assembler::stlur(const Register& rt, const MemOperand& dst) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kRCpc, CPUFeatures::kRCpcImm));
  VIXL_ASSERT(dst.IsImmediateOffset() && IsImmLSUnscaled(dst.GetOffset()));

  Instr base = RnSP(dst.GetBaseRegister());
  int64_t offset = dst.GetOffset();
  Instr op = rt.Is64Bits() ? STLUR_x : STLUR_w;
  Emit(op | Rt(rt) | base | ImmLS(static_cast<int>(offset)));
}

VIXL_ALWAYS_INLINE void Assembler::ldarb(const Register& rt, const MemOperand& src) {
  VIXL_ASSERT(src.IsImmediateOffset() && (src.GetOffset() == 0));
  Emit(LDARB_w | Rs_mask | Rt(rt) | Rt2_mask | RnSP(src.GetBaseRegister()));
}

VIXL_ALWAYS_INLINE void Assembler::ldarh(const Register& rt, const MemOperand& src) {
  VIXL_ASSERT(src.IsImmediateOffset() && (src.GetOffset() == 0));
  Emit(LDARH_w | Rs_mask | Rt(rt) | Rt2_mask | RnSP(src.GetBaseRegister()));
}

VIXL_ALWAYS_INLINE void Assembler::ldar(const Register& rt, const MemOperand& src) {
  VIXL_ASSERT(src.IsImmediateOffset() && (src.GetOffset() == 0));
  LoadStoreExclusive op = rt.Is64Bits() ? LDAR_x : LDAR_w;
  Emit(op | Rs_mask | Rt(rt) | Rt2_mask | RnSP(src.GetBaseRegister()));
}

VIXL_ALWAYS_INLINE void Assembler::ldaprb(const Register& rt, const MemOperand& src) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kRCpc));
  VIXL_ASSERT(src.IsImmediateOffset() && (src.GetOffset() == 0));
  AtomicMemoryOp op = LDAPRB;
  Emit(op | Rs(xzr) | Rt(rt) | RnSP(src.GetBaseRegister()));
}

VIXL_ALWAYS_INLINE void Assembler::ldapurb(const Register& rt, const MemOperand& src) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kRCpc, CPUFeatures::kRCpcImm));
  VIXL_ASSERT(src.IsImmediateOffset() && IsImmLSUnscaled(src.GetOffset()));

  Instr base = RnSP(src.GetBaseRegister());
  int64_t offset = src.GetOffset();
  Emit(LDAPURB | Rt(rt) | base | ImmLS(static_cast<int>(offset)));
}

VIXL_ALWAYS_INLINE void Assembler::ldaprh(const Register& rt, const MemOperand& src) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kRCpc));
  VIXL_ASSERT(src.IsImmediateOffset() && (src.GetOffset() == 0));
  AtomicMemoryOp op = LDAPRH;
  Emit(op | Rs(xzr) | Rt(rt) | RnSP(src.GetBaseRegister()));
}

VIXL_ALWAYS_INLINE void Assembler::ldapurh(const Register& rt, const MemOperand& src) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kRCpc, CPUFeatures::kRCpcImm));
  VIXL_ASSERT(src.IsImmediateOffset() && IsImmLSUnscaled(src.GetOffset()));

  Instr base = RnSP(src.GetBaseRegister());
  int64_t offset = src.GetOffset();
  Emit(LDAPURH | Rt(rt) | base | ImmLS(static_cast<int>(offset)));
}

VIXL_ALWAYS_INLINE void Assembler::ldapursh(const Register& rt, const MemOperand& src) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kRCpc, CPUFeatures::kRCpcImm));
  VIXL_ASSERT(src.IsImmediateOffset() && IsImmLSUnscaled(src.GetOffset()));

  Instr base = RnSP(src.GetBaseRegister());
  int64_t offset = src.GetOffset();
  LoadStoreRCpcUnscaledOffsetOp op = rt.Is64Bits() ? LDAPURSH_x : LDAPURSH_w;
  Emit(op | Rt(rt) | base | ImmLS(static_cast<int>(offset)));
}

VIXL_ALWAYS_INLINE void Assembler::ldapr(const Register& rt, const MemOperand& src) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kRCpc));
  VIXL_ASSERT(src.IsImmediateOffset() && (src.GetOffset() == 0));
  AtomicMemoryOp op = rt.Is64Bits() ? LDAPR_x : LDAPR_w;
  Emit(op | Rs(xzr) | Rt(rt) | RnSP(src.GetBaseRegister()));
}

VIXL_ALWAYS_INLINE void Assembler::ldapur(const Register& rt, const MemOperand& src) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kRCpc, CPUFeatures::kRCpcImm));
  VIXL_ASSERT(src.IsImmediateOffset() && IsImmLSUnscaled(src.GetOffset()));

  Instr base = RnSP(src.GetBaseRegister());
  int64_t offset = src.GetOffset();
  LoadStoreRCpcUnscaledOffsetOp op = rt.Is64Bits() ? LDAPUR_x : LDAPUR_w;
  Emit(op | Rt(rt) | base | ImmLS(static_cast<int>(offset)));
}

VIXL_ALWAYS_INLINE void Assembler::ldapursw(const Register& rt, const MemOperand& src) {
  VIXL_ASSERT(CPUHas(CPUFeatures::kRCpc, CPUFeatures::kRCpcImm));
  VIXL_ASSERT(rt.Is64Bits());
  VIXL_ASSERT(src.IsImmediateOffset() && IsImmLSUnscaled(src.GetOffset()));

  Instr base = RnSP(src.GetBaseRegister());
  int64_t offset = src.GetOffset();
  Emit(LDAPURSW | Rt(rt) | base | ImmLS(static_cast<int>(offset)));
}

VIXL_ALWAYS_INLINE void Assembler::hint(SystemHint code) { hint(static_cast<int>(code)); }

VIXL_ALWAYS_INLINE void Assembler::hint(int imm7) {
  VIXL_ASSERT(IsUint7(imm7));
  Emit(HINT | ImmHint(imm7) | Rt(xzr));
}

VIXL_ALWAYS_INLINE void Assembler::mov(const Register& rd, const Register& rm) {
  // Moves involving the stack pointer are encoded as add immediate with
  // second operand of zero. Otherwise, orr with first operand zr is
  // used.
  if (rd.IsSP() || rm.IsSP()) {
    add(rd, rm, 0);
  } else {
    orr(rd, AppropriateZeroRegFor(rd), rm);
  }
}

VIXL_ALWAYS_INLINE void Assembler::mvn(const Register& rd, const Operand& operand) {
  orn(rd, AppropriateZeroRegFor(rd), operand);
}

VIXL_ALWAYS_INLINE void Assembler::mrs(const Register& xt, SystemRegister sysreg) {
  VIXL_ASSERT(xt.Is64Bits());
  VIXL_ASSERT(CPUHas(sysreg));
  Emit(MRS | ImmSystemRegister(sysreg) | Rt(xt));
}

VIXL_ALWAYS_INLINE void Assembler::msr(SystemRegister sysreg, const Register& xt) {
  VIXL_ASSERT(xt.Is64Bits());
  VIXL_ASSERT(CPUHas(sysreg));
  Emit(MSR | Rt(xt) | ImmSystemRegister(sysreg));
}

VIXL_ALWAYS_INLINE void Assembler::cfinv() {
  VIXL_ASSERT(CPUHas(CPUFeatures::kFlagM));
  Emit(CFINV);
}

VIXL_ALWAYS_INLINE void Assembler::dmb(BarrierDomain domain, BarrierType type) {
  Emit(DMB | ImmBarrierDomain(domain) | ImmBarrierType(type));
}

VIXL_ALWAYS_INLINE void Assembler::isb() {
  Emit(ISB | ImmBarrierDomain(FullSystem) | ImmBarrierType(BarrierAll));
}

VIXL_ALWAYS_INLINE void Assembler::MoveWide(const Register& rd,
                         uint64_t imm,
                         int shift,
                         MoveWideImmediateOp mov_op) {
  // Ignore the top 32 bits of an immediate if we're moving to a W register.
  if (rd.Is32Bits()) {
    // Check that the top 32 bits are zero (a positive 32-bit number) or top
    // 33 bits are one (a negative 32-bit number, sign extended to 64 bits).
    VIXL_ASSERT(((imm >> kWRegSize) == 0) ||
                ((imm >> (kWRegSize - 1)) == 0x1ffffffff));
    imm &= kWRegMask;
  }

  if (shift >= 0) {
    // Explicit shift specified.
    VIXL_ASSERT((shift == 0) || (shift == 16) || (shift == 32) ||
                (shift == 48));
    VIXL_ASSERT(rd.Is64Bits() || (shift == 0) || (shift == 16));
    shift /= 16;
  } else {
    // Calculate a new immediate and shift combination to encode the immediate
    // argument.
    shift = 0;
    if ((imm & 0xffffffffffff0000) == 0) {
      // Nothing to do.
    } else if ((imm & 0xffffffff0000ffff) == 0) {
      imm >>= 16;
      shift = 1;
    } else if ((imm & 0xffff0000ffffffff) == 0) {
      VIXL_ASSERT(rd.Is64Bits());
      imm >>= 32;
      shift = 2;
    } else if ((imm & 0x0000ffffffffffff) == 0) {
      VIXL_ASSERT(rd.Is64Bits());
      imm >>= 48;
      shift = 3;
    }
  }

  VIXL_ASSERT(IsUint16(imm));

  Emit(SF(rd) | MoveWideImmediateFixed | mov_op | Rd(rd) | ImmMoveWide(imm) |
       ShiftMoveWide(shift));
}

VIXL_ALWAYS_INLINE void Assembler::AddSub(const Register& rd,
                       const Register& rn,
                       const Operand& operand,
                       FlagsUpdate S,
                       AddSubOp op) {
  VIXL_ASSERT(rd.GetSizeInBits() == rn.GetSizeInBits());
  if (operand.IsImmediate()) {
    int64_t immediate = operand.GetImmediate();
    VIXL_ASSERT(IsImmAddSub(immediate));
    Instr dest_reg = (S == SetFlags) ? Rd(rd) : RdSP(rd);
    Emit(SF(rd) | AddSubImmediateFixed | op | Flags(S) |
         ImmAddSub(static_cast<int>(immediate)) | dest_reg | RnSP(rn));
  } else if (operand.IsShiftedRegister()) {
    VIXL_ASSERT(operand.GetRegister().GetSizeInBits() == rd.GetSizeInBits());
    VIXL_ASSERT(operand.GetShift() != ROR);

    // For instructions of the form:
    //   add/sub   wsp, <Wn>, <Wm> [, LSL #0-3 ]
    //   add/sub   <Wd>, wsp, <Wm> [, LSL #0-3 ]
    //   add/sub   wsp, wsp, <Wm> [, LSL #0-3 ]
    //   adds/subs <Wd>, wsp, <Wm> [, LSL #0-3 ]
    // or their 64-bit register equivalents, convert the operand from shifted to
    // extended register mode, and emit an add/sub extended instruction.
    if (rn.IsSP() || rd.IsSP()) {
      VIXL_ASSERT(!(rd.IsSP() && (S == SetFlags)));
      DataProcExtendedRegister(rd,
                               rn,
                               operand.ToExtendedRegister(),
                               S,
                               AddSubExtendedFixed | op);
    } else {
      DataProcShiftedRegister(rd, rn, operand, S, AddSubShiftedFixed | op);
    }
  } else {
    VIXL_ASSERT(operand.IsExtendedRegister());
    DataProcExtendedRegister(rd, rn, operand, S, AddSubExtendedFixed | op);
  }
}

VIXL_ALWAYS_INLINE void Assembler::AddSubWithCarry(const Register& rd,
                                const Register& rn,
                                const Operand& operand,
                                FlagsUpdate S,
                                AddSubWithCarryOp op) {
  VIXL_ASSERT(rd.GetSizeInBits() == rn.GetSizeInBits());
  VIXL_ASSERT(rd.GetSizeInBits() == operand.GetRegister().GetSizeInBits());
  VIXL_ASSERT(operand.IsShiftedRegister() && (operand.GetShiftAmount() == 0));
  Emit(SF(rd) | op | Flags(S) | Rm(operand.GetRegister()) | Rn(rn) | Rd(rd));
}

VIXL_ALWAYS_INLINE void Assembler::brk(int code) {
  VIXL_ASSERT(IsUint16(code));
  Emit(BRK | ImmException(code));
}

VIXL_ALWAYS_INLINE void Assembler::Logical(const Register& rd,
                        const Register& rn,
                        const Operand operand,
                        LogicalOp op) {
  VIXL_ASSERT(rd.GetSizeInBits() == rn.GetSizeInBits());
  if (operand.IsImmediate()) {
    int64_t immediate = operand.GetImmediate();
    unsigned reg_size = rd.GetSizeInBits();

    VIXL_ASSERT(immediate != 0);
    VIXL_ASSERT(immediate != -1);
    VIXL_ASSERT(rd.Is64Bits() || IsUint32(immediate));

    // If the operation is NOT, invert the operation and immediate.
    if ((op & NOT) == NOT) {
      op = static_cast<LogicalOp>(op & ~NOT);
      immediate = rd.Is64Bits() ? ~immediate : (~immediate & kWRegMask);
    }

    unsigned n, imm_s, imm_r;
    if (IsImmLogical(immediate, reg_size, &n, &imm_s, &imm_r)) {
      // Immediate can be encoded in the instruction.
      LogicalImmediate(rd, rn, n, imm_s, imm_r, op);
    } else {
      // This case is handled in the macro assembler.
      VIXL_UNREACHABLE();
    }
  } else {
    VIXL_ASSERT(operand.IsShiftedRegister());
    VIXL_ASSERT(operand.GetRegister().GetSizeInBits() == rd.GetSizeInBits());
    Instr dp_op = static_cast<Instr>(op | LogicalShiftedFixed);
    DataProcShiftedRegister(rd, rn, operand, LeaveFlags, dp_op);
  }
}

VIXL_ALWAYS_INLINE void Assembler::LogicalImmediate(const Register& rd,
                                 const Register& rn,
                                 unsigned n,
                                 unsigned imm_s,
                                 unsigned imm_r,
                                 LogicalOp op) {
  unsigned reg_size = rd.GetSizeInBits();
  Instr dest_reg = (op == ANDS) ? Rd(rd) : RdSP(rd);
  Emit(SF(rd) | LogicalImmediateFixed | op | BitN(n, reg_size) |
       ImmSetBits(imm_s, reg_size) | ImmRotate(imm_r, reg_size) | dest_reg |
       Rn(rn));
}

VIXL_ALWAYS_INLINE void Assembler::DataProcShiftedRegister(const Register& rd,
                                        const Register& rn,
                                        const Operand& operand,
                                        FlagsUpdate S,
                                        Instr op) {
  VIXL_ASSERT(operand.IsShiftedRegister());
  VIXL_ASSERT(rn.Is64Bits() ||
              (rn.Is32Bits() && IsUint5(operand.GetShiftAmount())));
  Emit(SF(rd) | op | Flags(S) | ShiftDP(operand.GetShift()) |
       ImmDPShift(operand.GetShiftAmount()) | Rm(operand.GetRegister()) |
       Rn(rn) | Rd(rd));
}

VIXL_ALWAYS_INLINE void Assembler::DataProcExtendedRegister(const Register& rd,
                                         const Register& rn,
                                         const Operand& operand,
                                         FlagsUpdate S,
                                         Instr op) {
  Instr dest_reg = (S == SetFlags) ? Rd(rd) : RdSP(rd);
  Emit(SF(rd) | op | Flags(S) | Rm(operand.GetRegister()) |
       ExtendMode(operand.GetExtend()) |
       ImmExtendShift(operand.GetShiftAmount()) | dest_reg | RnSP(rn));
}

VIXL_ALWAYS_INLINE Instr Assembler::LoadStoreMemOperand(const MemOperand& addr,
                                     unsigned access_size,
                                     LoadStoreScalingOption option) {
  Instr base = RnSP(addr.GetBaseRegister());
  int64_t offset = addr.GetOffset();

  if (addr.IsImmediateOffset()) {
    bool prefer_unscaled =
        (option == PreferUnscaledOffset) || (option == RequireUnscaledOffset);
    if (prefer_unscaled && IsImmLSUnscaled(offset)) {
      // Use the unscaled addressing mode.
      return base | LoadStoreUnscaledOffsetFixed |
             ImmLS(static_cast<int>(offset));
    }

    if ((option != RequireUnscaledOffset) &&
        IsImmLSScaled(offset, access_size)) {
      // Use the scaled addressing mode.
      return base | LoadStoreUnsignedOffsetFixed |
             ImmLSUnsigned(static_cast<int>(offset) >> access_size);
    }

    if ((option != RequireScaledOffset) && IsImmLSUnscaled(offset)) {
      // Use the unscaled addressing mode.
      return base | LoadStoreUnscaledOffsetFixed |
             ImmLS(static_cast<int>(offset));
    }
  }

  // All remaining addressing modes are register-offset, pre-indexed or
  // post-indexed modes.
  VIXL_ASSERT((option != RequireUnscaledOffset) &&
              (option != RequireScaledOffset));

  if (addr.IsRegisterOffset()) {
    Extend ext = addr.GetExtend();
    Shift shift = addr.GetShift();
    unsigned shift_amount = addr.GetShiftAmount();

    // LSL is encoded in the option field as UXTX.
    if (shift == LSL) {
      ext = UXTX;
    }

    // Shifts are encoded in one bit, indicating a left shift by the memory
    // access size.
    VIXL_ASSERT((shift_amount == 0) || (shift_amount == access_size));
    return base | LoadStoreRegisterOffsetFixed | Rm(addr.GetRegisterOffset()) |
           ExtendMode(ext) | ImmShiftLS((shift_amount > 0) ? 1 : 0);
  }

  if (addr.IsPreIndex() && IsImmLSUnscaled(offset)) {
    return base | LoadStorePreIndexFixed | ImmLS(static_cast<int>(offset));
  }

  if (addr.IsPostIndex() && IsImmLSUnscaled(offset)) {
    return base | LoadStorePostIndexFixed | ImmLS(static_cast<int>(offset));
  }

  // If this point is reached, the MemOperand (addr) cannot be encoded.
  VIXL_UNREACHABLE();
  return 0;
}

VIXL_ALWAYS_INLINE void Assembler::LoadStore(const CPURegister& rt,
                          const MemOperand& addr,
                          LoadStoreOp op,
                          LoadStoreScalingOption option) {
  VIXL_ASSERT(CPUHas(rt));
  Emit(op | Rt(rt) | LoadStoreMemOperand(addr, CalcLSDataSize(op), option));
}

VIXL_ALWAYS_INLINE bool Assembler::IsImmAddSub(int64_t immediate) {
  return IsUint12(immediate) ||
         (IsUint12(immediate >> 12) && ((immediate & 0xfff) == 0));
}

VIXL_ALWAYS_INLINE bool Assembler::IsImmLSPair(int64_t offset, unsigned access_size) {
  VIXL_ASSERT(access_size <= kQRegSizeInBytesLog2);
  return IsMultiple(offset, 1 << access_size) &&
         IsInt7(offset / (1 << access_size));
}

VIXL_ALWAYS_INLINE bool Assembler::IsImmLSScaled(int64_t offset, unsigned access_size) {
  VIXL_ASSERT(access_size <= kQRegSizeInBytesLog2);
  return IsMultiple(offset, 1 << access_size) &&
         IsUint12(offset / (1 << access_size));
}

VIXL_ALWAYS_INLINE bool Assembler::IsImmLSUnscaled(int64_t offset) { return IsInt9(offset); }

VIXL_ALWAYS_INLINE LoadStoreOp Assembler::LoadOpFor(const CPURegister& rt) {
  VIXL_ASSERT(rt.IsValid());
  if (rt.IsRegister()) {
    return rt.Is64Bits() ? LDR_x : LDR_w;
  } else {
    VIXL_ASSERT(rt.IsVRegister());
    switch (rt.GetSizeInBits()) {
      case kBRegSize:
        return LDR_b;
      case kHRegSize:
        return LDR_h;
      case kSRegSize:
        return LDR_s;
      case kDRegSize:
        return LDR_d;
      default:
        VIXL_ASSERT(rt.IsQ());
        return LDR_q;
    }
  }
}

VIXL_ALWAYS_INLINE LoadStoreOp Assembler::StoreOpFor(const CPURegister& rt) {
  VIXL_ASSERT(rt.IsValid());
  if (rt.IsRegister()) {
    return rt.Is64Bits() ? STR_x : STR_w;
  } else {
    VIXL_ASSERT(rt.IsVRegister());
    switch (rt.GetSizeInBits()) {
      case kBRegSize:
        return STR_b;
      case kHRegSize:
        return STR_h;
      case kSRegSize:
        return STR_s;
      case kDRegSize:
        return STR_d;
      default:
        VIXL_ASSERT(rt.IsQ());
        return STR_q;
    }
  }
}

VIXL_ALWAYS_INLINE LoadStorePairOp Assembler::StorePairOpFor(const CPURegister& rt,
                                          const CPURegister& rt2) {
  VIXL_ASSERT(AreSameSizeAndType(rt, rt2));
  USE(rt2);
  if (rt.IsRegister()) {
    return rt.Is64Bits() ? STP_x : STP_w;
  } else {
    VIXL_ASSERT(rt.IsVRegister());
    switch (rt.GetSizeInBytes()) {
      case kSRegSizeInBytes:
        return STP_s;
      case kDRegSizeInBytes:
        return STP_d;
      default:
        VIXL_ASSERT(rt.IsQ());
        return STP_q;
    }
  }
}

VIXL_ALWAYS_INLINE LoadStorePairOp Assembler::LoadPairOpFor(const CPURegister& rt,
                                         const CPURegister& rt2) {
  VIXL_ASSERT((STP_w | LoadStorePairLBit) == LDP_w);
  return static_cast<LoadStorePairOp>(StorePairOpFor(rt, rt2) |
                                      LoadStorePairLBit);
}

}  // namespace aarch64
}  // namespace vixl
#endif  // VIXL_AARCH64_ASSEMBLER_AARCH64_INL_H_
