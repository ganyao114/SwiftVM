#pragma once

#include <optional>

#include "runtime/ir/instr.h"

namespace swift::runtime::compile {

// Guest flag bits, independent of the target's physical flags carrier.
[[nodiscard]] std::optional<ir::Flags> ConditionFlags(ir::Cond condition);

struct FlagAccess {
    ir::Flags read{};
    // Only bits unconditionally defined by the instruction. An opaque helper
    // may observe and change flags, but cannot kill incoming liveness here.
    ir::Flags written{};
};

[[nodiscard]] FlagAccess InstructionFlagAccess(const ir::Inst& inst);

enum class UniformWritePolicy : u8 {
    // A declared call-wide footprint suffices for forward cache invalidation.
    Declared,
    // Loop-invariant state additionally requires a direct, non-reentrant
    // helper. Under that contract, a read-only body also proves no writes.
    NoReentry,
};

// Null means unknown or malformed. Neither policy proves helper reads absent;
// these write-only facts cannot justify removing preceding state stores.
[[nodiscard]] const ir::UniformEffectSet* HelperUniformWrites(const ir::Inst& inst,
                                                              u32 uniform_size,
                                                              UniformWritePolicy policy);

}  // namespace swift::runtime::compile
