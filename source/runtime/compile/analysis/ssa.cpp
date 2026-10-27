#include "ssa.h"

#include <algorithm>
#include <functional>
#include <span>

#include "runtime/ir/hir_builder.h"
#include "semantic_facts.h"
#include "uses.h"

namespace swift::runtime::compile {

struct SSAValidation::LocalFlow {
    struct Use {
        u32 definition, fragment;
    };
    struct Branch {
        const ir::Inst* instruction;
        u32 fragment;
    };
    struct Label {
        const ir::Inst* branch;
        u32 fragment;
    };
    struct Block {
        const ir::Block* input;
        u32 fragment{};
        Vector<Use> definitions, uses;
        Vector<Branch> branches;
        Vector<Label> labels;
    };
    Vector<Block> blocks;
    bool active{};

    Block& Prepare(const ir::Block* block) {
        if (!active) {
            blocks.push_back({.input = block});
            active = true;
        }
        ASSERT(blocks.back().input == block);
        return blocks.back();
    }

    void Define(const ir::Inst& instruction) {
        const auto op = instruction.GetOp();
        if (op == ir::OpCode::Goto || op == ir::OpCode::NotGoto) {
            auto& block = Prepare(instruction.GetBlock());
            block.branches.push_back({&instruction, block.fragment});
            ++block.fragment;
        } else if (op == ir::OpCode::BindLabel) {
            auto& block = Prepare(instruction.GetBlock());
            ++block.fragment;
            block.labels.push_back({instruction.GetArg<ir::Value>(0).Def(), block.fragment});
        } else if (active && instruction.HasValue()) {
            blocks.back().definitions.push_back({instruction.Id(), blocks.back().fragment});
        }
    }

    bool Validate(std::span<const NonlocalUse> nonlocal) {
        // A value exported to another HIR block must also dominate this
        // block's exit. HIR-level dominance alone cannot prove that a local
        // branch did not skip its definition.
        for (const auto& use : nonlocal) {
            auto source = std::find_if(blocks.begin(), blocks.end(), [&](const auto& block) {
                return block.input == use.definition->GetBlock();
            });
            if (source != blocks.end())
                source->uses.push_back({use.definition->Id(), source->fragment});
        }
        for (auto& block : blocks) {
            if (block.labels.size() != block.branches.size()) return false;
            const auto pointer_less = std::less<const ir::Inst*>{};
            std::sort(block.labels.begin(), block.labels.end(), [&](auto a, auto b) {
                return pointer_less(a.branch, b.branch);
            });
            for (size_t i = 1; i < block.labels.size(); ++i)
                if (block.labels[i - 1].branch == block.labels[i].branch) return false;
            constexpr auto missing = UINT32_MAX;
            Vector<u32> targets(block.fragment + 1, missing);
            for (const auto& branch : block.branches) {
                auto label = std::lower_bound(block.labels.begin(),
                                              block.labels.end(),
                                              branch.instruction,
                                              [&](auto entry, auto* instruction) {
                                                  return pointer_less(entry.branch, instruction);
                                              });
                if (label == block.labels.end() || label->branch != branch.instruction ||
                    label->fragment <= branch.fragment)
                    return false;
                targets[branch.fragment] = label->fragment;
            }
            std::sort(block.definitions.begin(), block.definitions.end(), [](auto a, auto b) {
                return a.definition < b.definition;
            });
            Vector<std::pair<u32, u32>> queries;
            for (const auto& use : block.uses) {
                auto definition =
                        std::lower_bound(block.definitions.begin(),
                                         block.definitions.end(),
                                         use.definition,
                                         [](auto entry, auto id) { return entry.definition < id; });
                // Definitions before the first local branch belong to the
                // entry fragment and dominate every local path.
                const auto source = definition != block.definitions.end() &&
                                                    definition->definition == use.definition
                                            ? definition->fragment
                                            : 0;
                if (source != 0 && source != use.fragment)
                    queries.emplace_back(source, use.fragment);
            }
            std::sort(queries.begin(), queries.end());
            queries.erase(std::unique(queries.begin(), queries.end()), queries.end());
            if (queries.empty()) continue;
            Vector<u32> reached(block.fragment + 1);
            StackVector<u32, 16> work;
            u32 generation{};
            for (size_t first = 0; first < queries.size();) {
                const auto source = queries[first].first;
                size_t end = first + 1;
                while (end < queries.size() && queries[end].first == source) ++end;
                if (!++generation) {
                    std::fill(reached.begin(), reached.end(), 0);
                    generation = 1;
                }
                const auto enqueue = [&](u32 next) {
                    if (next == missing || next == source || reached[next] == generation) return;
                    reached[next] = generation;
                    work.push_back(next);
                };
                enqueue(0);
                while (!work.empty()) {
                    const auto current = work.back();
                    work.pop_back();
                    if (current < block.fragment) enqueue(current + 1);
                    enqueue(targets[current]);
                }
                for (size_t use = first; use < end; ++use)
                    if (reached[queries[use].second] == generation) return false;
                first = end;
            }
        }
        return true;
    }
};

SSAValidation::SSAValidation(const ir::Block* block,
                             const ir::HIRFunction* function,
                             u32 instruction_count)
        : function(function)
        , instruction_count(instruction_count)
        , closed(block || (function && FunctionCFGClosed(*function)))
        , valid(closed) {
    ASSERT((block != nullptr) != (function != nullptr));
    if (valid) defined.resize((size_t{instruction_count} + 63) / 64, 0);
}

SSAValidation::~SSAValidation() = default;

void SSAValidation::BeginBlock() {
    if (local) local->active = false;
}

void SSAValidation::Use(const ir::Block& block, ir::Value value, const ir::Inst* consumer) {
    // BindLabel names a control token; it does not read a materialized value.
    // Define validates the token and its unique forward binding separately.
    if (consumer && consumer->GetOp() == ir::OpCode::BindLabel) return;
    if (!valid) return;
    const auto* definition = value.Def();
    const auto id = definition->Id();
    if (id == ir::Inst::invalid_id || id >= instruction_count || !definition->GetBlock()) {
        valid = false;
        return;
    }
    if (definition->GetBlock() == &block) {
        valid = (defined[id / 64] & (u64{1} << (id % 64))) != 0;
        if (valid && local && local->active)
            local->blocks.back().uses.push_back({id, local->blocks.back().fragment});
    } else if (function) {
        nonlocal.push_back({definition, &block});
    } else {
        valid = false;
    }
}

void SSAValidation::Define(const ir::Inst& instruction, bool arguments_valid) {
    valid = valid && arguments_valid && instruction.HasValidResultType();
    if (!valid) return;
    const auto id = instruction.Id();
    if (id == ir::Inst::invalid_id || id >= instruction_count) {
        valid = false;
        return;
    }
    const auto mask = u64{1} << (id % 64);
    if (defined[id / 64] & mask) valid = false;
    defined[id / 64] |= mask;
    const auto op = instruction.GetOp();
    if (op == ir::OpCode::BindLabel) {
        if (!instruction.ArgAt(0).IsValue()) {
            valid = false;
            return;
        }
        const auto token = instruction.GetArg<ir::Value>(0);
        const auto* branch = token.Def();
        if (!branch || branch->GetBlock() != instruction.GetBlock() ||
            (branch->GetOp() != ir::OpCode::Goto && branch->GetOp() != ir::OpCode::NotGoto) ||
            branch->Id() >= instruction_count ||
            !(defined[branch->Id() / 64] & (u64{1} << (branch->Id() % 64)))) {
            valid = false;
            return;
        }
    }
    if (!valid) return;
    if (!local &&
        (op == ir::OpCode::Goto || op == ir::OpCode::NotGoto || op == ir::OpCode::BindLabel))
        local = std::make_unique<LocalFlow>();
    if (local) local->Define(instruction);
}

bool SSAValidation::Finish(SemanticFacts& facts) {
    if (facts.function_values) facts.function_values->entry_values.clear();
    if (valid && local) valid = local->Validate(nonlocal);
    if (!valid || nonlocal.empty()) return valid;
    const auto& graph = facts.PrepareFunctionCFG();
    if (!graph.Matches(*function)) return false;
    const auto blocks = graph.Blocks();
    Vector<std::pair<u32, u32>> uses;
    for (const auto& use : nonlocal) {
        const auto id = use.definition->Id();
        if (!(defined[id / 64] & (u64{1} << (id % 64)))) return false;
        const auto source = graph.FindBlock(use.definition->GetBlock());
        const auto target = graph.FindBlock(use.consumer);
        if (!source || !target) return false;
        uses.emplace_back(*source, *target);
    }
    std::sort(uses.begin(), uses.end());
    uses.erase(std::unique(uses.begin(), uses.end()), uses.end());
    StackVector<u8, 32> returned;
    returned.resize(blocks.size(), 0);
    StackVector<std::pair<u32, u32>, 8> returns;
    for (size_t i = 0; i < blocks.size(); ++i)
        if (blocks[i].return_block != FunctionCFG::MissingBlock) {
            returned[blocks[i].return_block] = 1;
            returns.emplace_back(blocks[i].return_block, static_cast<u32>(i));
        }
    std::sort(returns.begin(), returns.end());
    StackVector<u32, 8> roots;
    for (size_t i = 0; i < blocks.size(); ++i)
        if (blocks[i].function_entry || blocks[i].external_entry ||
            (!blocks[i].has_incoming_edges && !returned[i]))
            roots.push_back(static_cast<u32>(i));
    // A definition dominates a use exactly when no declared entry can reach
    // that use while avoiding the definition's block. Group by definition
    // block, reuse one worklist, and keep storage linear rather than building
    // a quadratic dominator matrix for a large function.
    Vector<u32> reached(blocks.size());
    StackVector<u64, 1> entry_values;
    entry_values.resize((blocks.size() + 63) / 64, 0);
    StackVector<u32, 32> work;
    u32 generation{};
    const auto restart = [&] {
        if (!++generation) {
            std::fill(reached.begin(), reached.end(), 0);
            generation = 1;
        }
        work.clear();
    };
    for (size_t first = 0; first < uses.size();) {
        const auto source = uses[first].first;
        size_t end = first + 1;
        while (end < uses.size() && uses[end].first == source) ++end;
        const auto enqueue = [&](u32 next) {
            if (next == FunctionCFG::MissingBlock || next == source || reached[next] == generation)
                return;
            reached[next] = generation;
            work.push_back(next);
        };
        restart();
        for (const auto root : roots) enqueue(root);
        while (!work.empty()) {
            const auto current = work.back();
            work.pop_back();
            for (const auto next : graph.Successors(blocks[current])) enqueue(next);
            // Logical returns do not prove that a guest-call ABI saved an
            // ordinary SSA residence.
            enqueue(blocks[current].return_block);
        }
        for (size_t use = first; use < end; ++use)
            if (reached[uses[use].second] == generation) return false;

        // Include relay blocks, even when their only need for the value is a
        // later successor. Stop at the definition and reuse linear scratch.
        restart();
        for (size_t use = first; use < end; ++use) enqueue(uses[use].second);
        while (!work.empty()) {
            const auto current = work.back();
            work.pop_back();
            entry_values[current / 64] |= u64{1} << (current % 64);
            for (const auto previous : graph.Predecessors(blocks[current])) enqueue(previous);
            auto returned_from =
                    std::lower_bound(returns.begin(), returns.end(), std::pair{current, u32{0}});
            for (; returned_from != returns.end() && returned_from->first == current;
                 ++returned_from)
                enqueue(returned_from->second);
        }
        first = end;
    }
    facts.PrepareFunctionValues().entry_values = std::move(entry_values);
    return true;
}

bool SemanticFacts::PrepareSSAValidation() {
    ASSERT(HasInput());
    const bool sealed = InputSealed();
    if (sealed && ssa_valid) return *ssa_valid;
    if (sealed && !value_liveness) {
        (void)PrepareValueLiveness();
        if (ssa_valid) return *ssa_valid;
    }
    // Manual preparation may change mutable fixtures between calls. Recheck
    // those inputs; only a sealed input may reuse the cached proof.
    SSAValidation validation{block, function, instruction_count};
    if (!validation.ReferencesClosed()) {
        if (sealed) ssa_valid = false;
        return false;
    }
    const auto visit = [&]<bool reachable>(const ir::Block& current) {
        if constexpr (reachable) validation.BeginBlock();
        for (const auto& instruction : current.GetInstList()) {
            validation.ObserveInstruction<reachable>(current, instruction, [](ir::Value) {});
        }
        validation.ObserveImplicitUses<reachable>(current, 0, [](ir::Value, u32) {});
    };
    if (function) {
        // Unreachable members still obey the IR shape contract. They do not
        // acquire reachable-path definition or dominance requirements.
        StackVector<u8, 32> reachable;
        reachable.resize(function->MaxBlockCount());
        for (const auto& member : function->GetHIRBlocksRPO()) reachable[member.GetOrderId()] = 1;
        for (const auto* member : function->GetHIRBlocks()) {
            if (!member) continue;
            if (reachable[member->GetOrderId()]) visit.template operator()<true>(*member->GetBlock());
            else visit.template operator()<false>(*member->GetBlock());
        }
    } else {
        visit.template operator()<true>(*block);
    }
    const bool valid = validation.Finish(*this);
    if (sealed) ssa_valid = valid;
    return valid;
}
}  // namespace swift::runtime::compile
