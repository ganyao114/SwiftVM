#pragma once

#include <array>
#include <biscuit/registers.hpp>

namespace swift::runtime::backend::riscv64 {

// Generated blocks use the RV64 C ABI: HaltReason block(State*).
// Guest registers remain in State; these anchors are callee-saved.
inline constexpr auto frame = biscuit::s0;
inline constexpr auto state = biscuit::s1;
inline constexpr auto values = biscuit::s2;
inline constexpr std::array scalar_registers{biscuit::s3, biscuit::s4, biscuit::s5,
        biscuit::s6, biscuit::s7, biscuit::s8, biscuit::s9, biscuit::s10, biscuit::s11};
inline constexpr unsigned kValueStride = 16;
inline constexpr unsigned kSavedFrameSize = 32;
// Faults in C++ helpers can bypass their register restores. Keep the complete
// LP64D callee-saved set at the generated block's recovery frame.
inline constexpr unsigned kBlockSavedFrameSize = 208;
// State's virtual flag word follows the ARM64 backend/interpreter NZCV layout,
// rather than the low-bit IR mask enum.
inline constexpr unsigned kNegateBit = 31;
inline constexpr unsigned kZeroBit = 30;
inline constexpr unsigned kCarryBit = 29;
inline constexpr unsigned kOverflowBit = 28;

}  // namespace swift::runtime::backend::riscv64
