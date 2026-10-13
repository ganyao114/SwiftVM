#pragma once

#include <array>
#include <cstdint>
#include <xbyak_loongarch64/xbyak_loongarch64.h>

namespace swift::runtime::backend::loongarch64 {

using GPR = Xbyak_loongarch64::XReg;
// Upstream uses XReg for the scalar FPR operand class as well.
using FPR = Xbyak_loongarch64::XReg;
using VReg = Xbyak_loongarch64::VReg;
using XVReg = Xbyak_loongarch64::XVReg;
using Label = Xbyak_loongarch64::Label;

inline const GPR zero{0}, ra{1}, tp{2}, sp{3};
inline const GPR a0{4}, a1{5}, a2{6}, a3{7}, a4{8}, a5{9}, a6{10}, a7{11};
inline const GPR t0{12}, t1{13}, t2{14}, t3{15}, t4{16}, t5{17}, t6{18}, t7{19}, t8{20};
inline const GPR reserved{21};

// Generated blocks use the LP64D C ABI: HaltReason block(State*).
// r2/tp and r21 are never allocated. The anchors are callee-saved.
inline const GPR frame{22}, state{23}, values{24}, flags{31};
inline const std::array scalar_registers{GPR{25}, GPR{26}, GPR{27},
        GPR{28}, GPR{29}, GPR{30}};
inline const std::array saved_gprs{ra, frame, state, values, GPR{25},
        GPR{26}, GPR{27}, GPR{28}, GPR{29}, GPR{30}, flags};
inline const std::array saved_fprs{FPR{24}, FPR{25}, FPR{26}, FPR{27},
        FPR{28}, FPR{29}, FPR{30}, FPR{31}};
inline const FPR fcsr0{0};

inline constexpr unsigned kValueStride = 16;
inline constexpr unsigned kSavedFrameSize = 32;
// Fault recovery can bypass helper epilogues. Save the complete LP64D
// callee-saved GP/FPR set and FCSR0 in the generated block's recovery frame.
inline constexpr unsigned kBlockSavedFrameSize = 176;
inline constexpr unsigned kGPRSaveOffset = 0;
inline constexpr unsigned kFPRSaveOffset = 88;
inline constexpr unsigned kFCSRSaveOffset = 152;
inline constexpr unsigned kNegateBit = 31;
inline constexpr unsigned kZeroBit = 30;
inline constexpr unsigned kCarryBit = 29;
inline constexpr unsigned kOverflowBit = 28;

static_assert(kBlockSavedFrameSize % 16 == 0);
static_assert(kFPRSaveOffset == saved_gprs.size() * sizeof(std::uint64_t));
static_assert(kFCSRSaveOffset == kFPRSaveOffset + saved_fprs.size() * sizeof(std::uint64_t));

}  // namespace swift::runtime::backend::loongarch64
