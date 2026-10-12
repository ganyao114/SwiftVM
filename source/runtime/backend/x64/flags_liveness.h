#pragma once

#include "runtime/ir/block.h"

namespace swift::runtime::backend::x64 {

struct FlagsLivenessStats {
    u32 full_publications{};
    u32 narrowed_to_carry{};
    u32 barriers{};
};

// Local x64 NZCV liveness only. Retains every SaveFlags and every store at its
// original position. uniform_extent is the actual State uniform-buffer size;
// zero means that no uniform access can be proven safe to cross.
FlagsLivenessStats NarrowCarryChainFlags(ir::Block*, u32 uniform_extent);

} // namespace swift::runtime::backend::x64
