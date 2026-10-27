#pragma once

#include <array>
#include <memory>

#include "jit_context.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {

class TerminalLocationPublication {
public:
    using RecoveryOffsets = std::array<u32, 32>;

    void Reset();
    [[nodiscard]] Label* GetOrCreateMissLabel(const XRegister& target);
    [[nodiscard]] Label* MissLabel(const XRegister& target) const;
    [[nodiscard]] RecoveryOffsets EmitColdPaths(JitContext& context);

private:
    std::array<std::unique_ptr<Label>, 32> miss_labels{};
};

}  // namespace swift::runtime::backend::arm64
