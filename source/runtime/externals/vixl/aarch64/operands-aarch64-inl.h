// Copyright 2016, VIXL authors
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

#ifndef VIXL_AARCH64_OPERANDS_AARCH64_INL_H_
#define VIXL_AARCH64_OPERANDS_AARCH64_INL_H_

#include "operands-aarch64.h"

namespace vixl {
namespace aarch64 {

VIXL_ALWAYS_INLINE Operand::Operand(int64_t immediate)
    : immediate_(immediate),
      reg_(NoReg),
      shift_(NO_SHIFT),
      extend_(NO_EXTEND),
      shift_amount_(0) {}

VIXL_ALWAYS_INLINE Operand::Operand(int64_t immediate, Shift shift)
        : immediate_(immediate),
          reg_(NoReg),
          shift_(shift),
          extend_(NO_EXTEND),
          shift_amount_(0) {}

VIXL_ALWAYS_INLINE Operand::Operand(Register reg, Shift shift, unsigned shift_amount)
    : reg_(reg),
      shift_(shift),
      extend_(NO_EXTEND),
      shift_amount_(shift_amount) {
  VIXL_ASSERT(shift != MSL);
  VIXL_ASSERT(reg.Is64Bits() || (shift_amount < kWRegSize));
  VIXL_ASSERT(reg.Is32Bits() || (shift_amount < kXRegSize));
  VIXL_ASSERT(!reg.IsSP());
}

VIXL_ALWAYS_INLINE Operand::Operand(Register reg, Extend extend, unsigned shift_amount)
    : reg_(reg),
      shift_(NO_SHIFT),
      extend_(extend),
      shift_amount_(shift_amount) {
  VIXL_ASSERT(reg.IsValid());
  VIXL_ASSERT(shift_amount <= 4);
  VIXL_ASSERT(!reg.IsSP());

  // Extend modes SXTX and UXTX require a 64-bit register.
  VIXL_ASSERT(reg.Is64Bits() || ((extend != SXTX) && (extend != UXTX)));
}

VIXL_ALWAYS_INLINE bool Operand::IsImmediate() const { return reg_.Is(NoReg); }

VIXL_ALWAYS_INLINE bool Operand::IsPlainRegister() const {
  return reg_.IsValid() &&
         (((shift_ == NO_SHIFT) && (extend_ == NO_EXTEND)) ||
          // No-op shifts.
          ((shift_ != NO_SHIFT) && (shift_amount_ == 0)) ||
          // No-op extend operations.
          // We can't include [US]XTW here without knowing more about the
          // context; they are only no-ops for 32-bit operations.
          //
          // For example, this operand could be replaced with w1:
          //   __ Add(w0, w0, Operand(w1, UXTW));
          // However, no plain register can replace it in this context:
          //   __ Add(x0, x0, Operand(w1, UXTW));
          (((extend_ == UXTX) || (extend_ == SXTX)) && (shift_amount_ == 0)));
}

VIXL_ALWAYS_INLINE bool Operand::IsShiftedRegister() const {
  return reg_.IsValid() && (shift_ != NO_SHIFT);
}

VIXL_ALWAYS_INLINE bool Operand::IsExtendedRegister() const {
  return reg_.IsValid() && (extend_ != NO_EXTEND);
}

VIXL_ALWAYS_INLINE bool Operand::IsZero() const {
  if (IsImmediate()) {
    return GetImmediate() == 0;
  } else {
    return GetRegister().IsZero();
  }
}

VIXL_ALWAYS_INLINE Operand Operand::ToExtendedRegister() const {
  VIXL_ASSERT(IsShiftedRegister());
  VIXL_ASSERT((shift_ == LSL) && (shift_amount_ <= 4));
  return Operand(reg_, reg_.Is64Bits() ? UXTX : UXTW, shift_amount_);
}

VIXL_ALWAYS_INLINE MemOperand::MemOperand()
    : base_(NoReg),
      regoffset_(NoReg),
      offset_(0),
      addrmode_(Offset),
      shift_(NO_SHIFT),
      extend_(NO_EXTEND) {}

VIXL_ALWAYS_INLINE MemOperand::MemOperand(Register base, int64_t offset, AddrMode addrmode)
    : base_(base),
      regoffset_(NoReg),
      offset_(offset),
      addrmode_(addrmode),
      shift_(NO_SHIFT),
      extend_(NO_EXTEND),
      shift_amount_(0) {
  VIXL_ASSERT(base.Is64Bits() && !base.IsZero());
}

VIXL_ALWAYS_INLINE MemOperand::MemOperand(Register base,
                       Register regoffset,
                       Extend extend,
                       unsigned shift_amount)
    : base_(base),
      regoffset_(regoffset),
      offset_(0),
      addrmode_(Offset),
      shift_(NO_SHIFT),
      extend_(extend),
      shift_amount_(shift_amount) {
  VIXL_ASSERT(base.Is64Bits() && !base.IsZero());
  VIXL_ASSERT(!regoffset.IsSP());
  VIXL_ASSERT((extend == UXTW) || (extend == SXTW) || (extend == SXTX));

  // SXTX extend mode requires a 64-bit offset register.
  VIXL_ASSERT(regoffset.Is64Bits() || (extend != SXTX));
}

VIXL_ALWAYS_INLINE MemOperand::MemOperand(Register base,
                       Register regoffset,
                       Shift shift,
                       unsigned shift_amount)
    : base_(base),
      regoffset_(regoffset),
      offset_(0),
      addrmode_(Offset),
      shift_(shift),
      extend_(NO_EXTEND),
      shift_amount_(shift_amount) {
  VIXL_ASSERT(base.Is64Bits() && !base.IsZero());
  VIXL_ASSERT(regoffset.Is64Bits() && !regoffset.IsSP());
  VIXL_ASSERT(shift == LSL);
}

VIXL_ALWAYS_INLINE MemOperand::MemOperand(Register base, const Operand& offset, AddrMode addrmode)
    : base_(base),
      regoffset_(NoReg),
      addrmode_(addrmode),
      shift_(NO_SHIFT),
      extend_(NO_EXTEND),
      shift_amount_(0) {
  VIXL_ASSERT(base.Is64Bits() && !base.IsZero());

  if (offset.IsImmediate()) {
    offset_ = offset.GetImmediate();
  } else if (offset.IsShiftedRegister()) {
    VIXL_ASSERT((addrmode == Offset) || (addrmode == PostIndex));

    regoffset_ = offset.GetRegister();
    shift_ = offset.GetShift();
    shift_amount_ = offset.GetShiftAmount();

    extend_ = NO_EXTEND;
    offset_ = 0;

    // These assertions match those in the shifted-register constructor.
    VIXL_ASSERT(regoffset_.Is64Bits() && !regoffset_.IsSP());
    VIXL_ASSERT(shift_ == LSL);
  } else {
    VIXL_ASSERT(offset.IsExtendedRegister());
    VIXL_ASSERT(addrmode == Offset);

    regoffset_ = offset.GetRegister();
    extend_ = offset.GetExtend();
    shift_amount_ = offset.GetShiftAmount();

    shift_ = NO_SHIFT;
    offset_ = 0;

    // These assertions match those in the extended-register constructor.
    VIXL_ASSERT(!regoffset_.IsSP());
    VIXL_ASSERT((extend_ == UXTW) || (extend_ == SXTW) || (extend_ == SXTX));
    VIXL_ASSERT((regoffset_.Is64Bits() || (extend_ != SXTX)));
  }
}

VIXL_ALWAYS_INLINE bool MemOperand::IsImmediateOffset() const {
  return (addrmode_ == Offset) && regoffset_.Is(NoReg);
}

VIXL_ALWAYS_INLINE bool MemOperand::IsRegisterOffset() const {
  return (addrmode_ == Offset) && !regoffset_.Is(NoReg);
}

VIXL_ALWAYS_INLINE bool MemOperand::IsPreIndex() const { return addrmode_ == PreIndex; }

VIXL_ALWAYS_INLINE bool MemOperand::IsPostIndex() const { return addrmode_ == PostIndex; }

VIXL_ALWAYS_INLINE void MemOperand::AddOffset(int64_t offset) {
  VIXL_ASSERT(IsImmediateOffset());
  offset_ += offset;
}

}  // namespace aarch64
}  // namespace vixl
#endif  // VIXL_AARCH64_OPERANDS_AARCH64_INL_H_
