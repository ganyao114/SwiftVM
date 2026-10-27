#pragma once

#include <memory>

#include "uses.h"

namespace swift::runtime::ir {
class Block;
class HIRFunction;
class Inst;
}  // namespace swift::runtime::ir

namespace swift::runtime::compile {
class SemanticFacts;

// A preparation-only checker. Final base-fact collection feeds the same raw
// operands and implicit roots here; the result owns no instruction snapshot.
// Function dominance is needed only after an actual nonlocal value is
// encountered. Local control-flow scratch exists only for Goto/BindLabel.
class SSAValidation final {
public:
    SSAValidation(const ir::Block* block, const ir::HIRFunction* function, u32 instruction_count);
    ~SSAValidation();
    SSAValidation(const SSAValidation&) = delete;
    SSAValidation& operator=(const SSAValidation&) = delete;
    [[nodiscard]] bool ReferencesClosed() const { return closed; }
    void BeginBlock();
    template<bool reachable = true, class Visitor>
    void ObserveInstruction(const ir::Block& block, const ir::Inst& instruction, Visitor&& visit) {
        const bool arguments_valid = instruction.CheckArguments([&](ir::Value value) {
            if constexpr (reachable) Use(block, value, &instruction);
            visit(value);
        });
        if constexpr (reachable) Define(instruction, arguments_valid);
        else valid &= arguments_valid && instruction.HasValidResultType();
    }
    template<bool reachable = true, class Visitor>
    void ObserveImplicitUses(const ir::Block& block, u32 position, Visitor&& visit) {
        const bool arguments_valid = CheckImplicitUses(block, position,
                [&](ir::Value value, u32 point) {
                    if constexpr (reachable) Use(block, value, nullptr);
                    visit(value, point);
                });
        valid &= arguments_valid;
    }
    [[nodiscard]] bool Finish(SemanticFacts& facts);

private:
    // Ordinary operands and implicit roots have passed their shared checks.
    // Neither path needs a second argument traversal.
    void Use(const ir::Block& block, ir::Value value, const ir::Inst* consumer);
    void Define(const ir::Inst& instruction, bool arguments_valid);
    struct NonlocalUse {
        const ir::Inst* definition;
        const ir::Block* consumer;
    };
    const ir::HIRFunction* function;
    u32 instruction_count;
    bool closed{}, valid{};
    StackVector<u64, 4> defined;
    Vector<NonlocalUse> nonlocal;
    struct LocalFlow;
    std::unique_ptr<LocalFlow> local;
};
}  // namespace swift::runtime::compile
