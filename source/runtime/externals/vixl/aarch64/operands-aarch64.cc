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

#include "operands-aarch64.h"

namespace vixl {
namespace aarch64 {

// CPURegList utilities.
CPURegister CPURegList::PopLowestIndex() {
  if (IsEmpty()) {
    return NoCPUReg;
  }
  int index = CountTrailingZeros(list_);
  VIXL_ASSERT((1 << index) & list_);
  Remove(index);
  return CPURegister(index, size_, type_);
}


CPURegister CPURegList::PopHighestIndex() {
  VIXL_ASSERT(IsValid());
  if (IsEmpty()) {
    return NoCPUReg;
  }
  int index = CountLeadingZeros(list_);
  index = kRegListSizeInBits - 1 - index;
  VIXL_ASSERT((1 << index) & list_);
  Remove(index);
  return CPURegister(index, size_, type_);
}


bool CPURegList::IsValid() const {
  if ((type_ == CPURegister::kRegister) || (type_ == CPURegister::kVRegister)) {
    bool is_valid = true;
    // Try to create a CPURegister for each element in the list.
    for (int i = 0; i < kRegListSizeInBits; i++) {
      if (((list_ >> i) & 1) != 0) {
        is_valid &= CPURegister(i, size_, type_).IsValid();
      }
    }
    return is_valid;
  } else if (type_ == CPURegister::kNoRegister) {
    // We can't use IsEmpty here because that asserts IsValid().
    return list_ == 0;
  } else {
    return false;
  }
}


void CPURegList::RemoveCalleeSaved() {
  if (GetType() == CPURegister::kRegister) {
    Remove(GetCalleeSaved(GetRegisterSizeInBits()));
  } else if (GetType() == CPURegister::kVRegister) {
    Remove(GetCalleeSavedV(GetRegisterSizeInBits()));
  } else {
    VIXL_ASSERT(GetType() == CPURegister::kNoRegister);
    VIXL_ASSERT(IsEmpty());
    // The list must already be empty, so do nothing.
  }
}


CPURegList CPURegList::Union(const CPURegList& list_1,
                             const CPURegList& list_2,
                             const CPURegList& list_3) {
  return Union(list_1, Union(list_2, list_3));
}


CPURegList CPURegList::Union(const CPURegList& list_1,
                             const CPURegList& list_2,
                             const CPURegList& list_3,
                             const CPURegList& list_4) {
  return Union(Union(list_1, list_2), Union(list_3, list_4));
}


CPURegList CPURegList::Intersection(const CPURegList& list_1,
                                    const CPURegList& list_2,
                                    const CPURegList& list_3) {
  return Intersection(list_1, Intersection(list_2, list_3));
}


CPURegList CPURegList::Intersection(const CPURegList& list_1,
                                    const CPURegList& list_2,
                                    const CPURegList& list_3,
                                    const CPURegList& list_4) {
  return Intersection(Intersection(list_1, list_2),
                      Intersection(list_3, list_4));
}


CPURegList CPURegList::GetCalleeSaved(unsigned size) {
  return CPURegList(CPURegister::kRegister, size, 19, 29);
}


CPURegList CPURegList::GetCalleeSavedV(unsigned size) {
  return CPURegList(CPURegister::kVRegister, size, 8, 15);
}


CPURegList CPURegList::GetCallerSaved(unsigned size) {
  // Registers x0-x18 and lr (x30) are caller-saved.
  CPURegList list = CPURegList(CPURegister::kRegister, size, 0, 18);
  // Do not use lr directly to avoid initialisation order fiasco bugs for users.
  list.Combine(Register(30, kXRegSize));
  return list;
}


CPURegList CPURegList::GetCallerSavedV(unsigned size) {
  // Registers d0-d7 and d16-d31 are caller-saved.
  CPURegList list = CPURegList(CPURegister::kVRegister, size, 0, 7);
  list.Combine(CPURegList(CPURegister::kVRegister, size, 16, 31));
  return list;
}


const CPURegList kCalleeSaved = CPURegList::GetCalleeSaved();
const CPURegList kCalleeSavedV = CPURegList::GetCalleeSavedV();
const CPURegList kCallerSaved = CPURegList::GetCallerSaved();
const CPURegList kCallerSavedV = CPURegList::GetCallerSavedV();


// Registers.
#define WREG(n) w##n,
const Register Register::wregisters[] = {AARCH64_REGISTER_CODE_LIST(WREG)};
#undef WREG

#define XREG(n) x##n,
const Register Register::xregisters[] = {AARCH64_REGISTER_CODE_LIST(XREG)};
#undef XREG

#define BREG(n) b##n,
const VRegister VRegister::bregisters[] = {AARCH64_REGISTER_CODE_LIST(BREG)};
#undef BREG

#define HREG(n) h##n,
const VRegister VRegister::hregisters[] = {AARCH64_REGISTER_CODE_LIST(HREG)};
#undef HREG

#define SREG(n) s##n,
const VRegister VRegister::sregisters[] = {AARCH64_REGISTER_CODE_LIST(SREG)};
#undef SREG

#define DREG(n) d##n,
const VRegister VRegister::dregisters[] = {AARCH64_REGISTER_CODE_LIST(DREG)};
#undef DREG

#define QREG(n) q##n,
const VRegister VRegister::qregisters[] = {AARCH64_REGISTER_CODE_LIST(QREG)};
#undef QREG

#define VREG(n) v##n,
const VRegister VRegister::vregisters[] = {AARCH64_REGISTER_CODE_LIST(VREG)};
#undef VREG


const Register& Register::GetWRegFromCode(unsigned code) {
  if (code == kSPRegInternalCode) {
    return wsp;
  } else {
    VIXL_ASSERT(code < kNumberOfRegisters);
    return wregisters[code];
  }
}


const Register& Register::GetXRegFromCode(unsigned code) {
  if (code == kSPRegInternalCode) {
    return sp;
  } else {
    VIXL_ASSERT(code < kNumberOfRegisters);
    return xregisters[code];
  }
}


const VRegister& VRegister::GetBRegFromCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return bregisters[code];
}


const VRegister& VRegister::GetHRegFromCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return hregisters[code];
}


const VRegister& VRegister::GetSRegFromCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return sregisters[code];
}


const VRegister& VRegister::GetDRegFromCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return dregisters[code];
}


const VRegister& VRegister::GetQRegFromCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return qregisters[code];
}


const VRegister& VRegister::GetVRegFromCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return vregisters[code];
}


const Register& CPURegister::W() const {
  VIXL_ASSERT(IsValidRegister());
  return Register::GetWRegFromCode(code_);
}


const Register& CPURegister::X() const {
  VIXL_ASSERT(IsValidRegister());
  return Register::GetXRegFromCode(code_);
}


const VRegister& CPURegister::B() const {
  VIXL_ASSERT(IsValidVRegister());
  return VRegister::GetBRegFromCode(code_);
}


const VRegister& CPURegister::H() const {
  VIXL_ASSERT(IsValidVRegister());
  return VRegister::GetHRegFromCode(code_);
}


const VRegister& CPURegister::S() const {
  VIXL_ASSERT(IsValidVRegister());
  return VRegister::GetSRegFromCode(code_);
}


const VRegister& CPURegister::D() const {
  VIXL_ASSERT(IsValidVRegister());
  return VRegister::GetDRegFromCode(code_);
}


const VRegister& CPURegister::Q() const {
  VIXL_ASSERT(IsValidVRegister());
  return VRegister::GetQRegFromCode(code_);
}


const VRegister& CPURegister::V() const {
  VIXL_ASSERT(IsValidVRegister());
  return VRegister::GetVRegFromCode(code_);
}

bool CPURegister::operator==(const CPURegister &rhs) const {
  return code_ == rhs.code_ &&
                   size_ == rhs.size_ &&
                   type_ == rhs.type_;
}

bool CPURegister::operator!=(const CPURegister &rhs) const {
  return !(rhs == *this);
}


// Operand.


// Operand.



























// MemOperand






























GenericOperand::GenericOperand(const CPURegister& reg)
    : cpu_register_(reg), mem_op_size_(0) {
  if (reg.IsQ()) {
    VIXL_ASSERT(reg.GetSizeInBits() > static_cast<int>(kXRegSize));
    // Support for Q registers is not implemented yet.
    VIXL_UNIMPLEMENTED();
  }
}


GenericOperand::GenericOperand(const MemOperand& mem_op, size_t mem_op_size)
    : cpu_register_(NoReg), mem_op_(mem_op), mem_op_size_(mem_op_size) {
  if (mem_op_size_ > kXRegSizeInBytes) {
    // We only support generic operands up to the size of X registers.
    VIXL_UNIMPLEMENTED();
  }
}

bool GenericOperand::Equals(const GenericOperand& other) const {
  if (!IsValid() || !other.IsValid()) {
    // Two invalid generic operands are considered equal.
    return !IsValid() && !other.IsValid();
  }
  if (IsCPURegister() && other.IsCPURegister()) {
    return GetCPURegister().Is(other.GetCPURegister());
  } else if (IsMemOperand() && other.IsMemOperand()) {
    return GetMemOperand().Equals(other.GetMemOperand()) &&
           (GetMemOperandSizeInBytes() == other.GetMemOperandSizeInBytes());
  }
  return false;
}
}
}  // namespace vixl::aarch64
