#pragma once

#include "aarch64/macro-assembler-aarch64.h"
#include "runtime/common/types.h"

namespace swift::runtime::backend::arm64 {

struct PairCallFrame {
    static constexpr u32 TargetRegister = 11;
    static constexpr u32 Size = 64;
    static constexpr u32 Link = 0;
    static constexpr u32 SavedX11 = 8;
    static constexpr u32 SavedX16 = 16;
    static constexpr u32 Argument(u32 index) { return 24 + index * sizeof(u64); }
    static constexpr u32 PrimaryResult = 48;
    static constexpr u32 SecondaryResult = 56;
};

void EmitPairCallTrampoline(vixl::aarch64::MacroAssembler& assembler);

}  // namespace swift::runtime::backend::arm64
