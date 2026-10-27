#pragma once

#include <optional>
#include <vector>

#include "runtime/ir/args.h"

namespace swift::runtime::ir {
class Block;
class HIRBlock;
class HIRFunction;
class Inst;
}

namespace swift::runtime::compile {

// Mutable-IR demand during one flags cleanup. Flag writes/readers can change,
// but this scope does not create or retarget local labels. Recompute demand
// after edits; do not carry this state into sealed IR or another cleanup.
class FlagFlow final {
public:
    explicit FlagFlow(const ir::Block* block);
    explicit FlagFlow(const ir::HIRFunction* function);
    FlagFlow(const FlagFlow&) = delete;
    FlagFlow& operator=(const FlagFlow&) = delete;

    // Function membership is identity-checked. A single Block never builds
    // this table or pays for a Function fixed point.
    void ComputeLiveIn();
    [[nodiscard]] std::optional<ir::Flags> LiveIn(const ir::HIRBlock*) const;
    [[nodiscard]] ir::Flags LiveOut(const ir::HIRBlock*) const;

    // Start an independent reverse walk. Only an actual BindLabel prepares
    // the block's sparse index; a lookup never scans or allocates. Capacity
    // is reused between blocks, and keys can be reused for the same block.
    void BeginBlock(const ir::Block*);
    void RememberLabel(const ir::Inst* target, ir::Flags needed);
    [[nodiscard]] std::optional<ir::Flags> LabelDemand(const ir::Inst* target) const;
    void ForgetLabels();

private:
    struct Transfer {
        ir::Flags generated{ir::Flags::None};
        ir::Flags preserved{ir::Flags::All};
        [[nodiscard]] ir::Flags Apply(ir::Flags out) const {
            return generated | (out & preserved);
        }
    };
    struct FunctionState {
        const ir::HIRBlock* block{};
        Transfer transfer{};
        ir::Flags live_in{ir::Flags::None};
    };
    struct LabelState {
        const ir::Inst* target{};
        Transfer value{};
        u32 epoch{};
    };

    [[nodiscard]] const FunctionState* FindState(const ir::HIRBlock*) const;
    [[nodiscard]] const LabelState* FindLabel(const ir::Inst*) const;
    void PrepareLabels();
    void RememberTransfer(const ir::Inst*, Transfer, bool merge);
    [[nodiscard]] Transfer Summarize(const ir::Block*);

    const ir::Block* single_block{};
    const ir::HIRFunction* function{};
    std::vector<FunctionState> states;
    std::vector<LabelState> labels;
    const ir::Block* current_block{};
    u32 epoch{};
    bool labels_prepared{};
};

}  // namespace swift::runtime::compile
