#include "verify.h"

#include <algorithm>
#include <functional>

#include "runtime/compile/analysis/uses.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile::audit {

IRReference::IRReference(std::span<const ir::Block* const> emitted_blocks,
                         const ir::HIRFunction* function,
                         u32 instruction_capacity) {
    std::vector<const ir::Block*> members;
    if (!BuildGraph(emitted_blocks, function, members)) return;
    for (const auto* block : members) {
        u32 position{};
        for (const auto& instruction : block->GetInstList()) {
            if (instruction.GetBlock() != block) {
                (void)Reject(FailureKind::Owner);
                return;
            }
            definitions.push_back({&instruction, block, position++});
        }
    }
    std::sort(definitions.begin(), definitions.end(), [](const auto& left, const auto& right) {
        return std::less<>{}(left.instruction, right.instruction);
    });
    for (size_t i = 1; i < definitions.size(); ++i) {
        if (definitions[i - 1].instruction == definitions[i].instruction) {
            (void)Reject(FailureKind::Owner);
            return;
        }
    }
    (void)Verify(members, instruction_capacity);
}

bool IRReference::Reject(FailureKind reason) {
    if (failure == FailureKind::None) failure = reason;
    return false;
}

u32 IRReference::BlockIndex(const ir::Block* block) const {
    const auto found = std::lower_bound(block_ids.begin(), block_ids.end(), block,
            [](const auto& entry, const auto* key) { return std::less<>{}(entry.first, key); });
    return found != block_ids.end() && found->first == block ? found->second : missing;
}

const IRReference::Definition* IRReference::Find(const ir::Inst* instruction) const {
    const auto found = std::lower_bound(definitions.begin(), definitions.end(), instruction,
            [](const auto& entry, const auto* key) {
                return std::less<>{}(entry.instruction, key);
            });
    return found != definitions.end() && found->instruction == instruction ? &*found : nullptr;
}

const ir::Block* IRReference::Owner(const ir::Inst* instruction) const {
    const auto* definition = Find(instruction);
    return definition ? definition->block : nullptr;
}

bool IRReference::Dominates(const ir::Block* definition, const ir::Block* use) const {
    if (!Valid()) return false;
    const auto source = BlockIndex(definition);
    const auto target = BlockIndex(use);
    return source < dominators.size() && target < dominators.size() &&
           source / 64 < dominators[target].size() &&
           (dominators[target][source / 64] & (u64{1} << (source % 64))) != 0;
}

bool IRReference::BuildGraph(std::span<const ir::Block* const> emitted_blocks,
                             const ir::HIRFunction* function,
                             std::vector<const ir::Block*>& members) {
    if (emitted_blocks.empty() || emitted_blocks.size() >= missing)
        return Reject(FailureKind::Blocks);
    for (size_t i = 0; i < emitted_blocks.size(); ++i) {
        if (!emitted_blocks[i]) return Reject(FailureKind::Blocks);
        block_ids.emplace_back(emitted_blocks[i], static_cast<u32>(i));
    }
    std::sort(block_ids.begin(), block_ids.end(), [](auto left, auto right) {
        return std::less<>{}(left.first, right.first);
    });
    for (size_t i = 1; i < block_ids.size(); ++i)
        if (block_ids[i - 1].first == block_ids[i].first) return Reject(FailureKind::Blocks);
    if (!function) {
        if (emitted_blocks.size() != 1) return Reject(FailureKind::Blocks);
        members.assign(emitted_blocks.begin(), emitted_blocks.end());
        dominators = {{u64{1}}};
        return true;
    }

    // Validate membership before dereferencing any edge, entry or return target.
    // This audit deliberately does not call FunctionCFGClosed or consume its
    // cached projection; it checks the actual HIR lists and their cached order.
    std::vector<const ir::HIRBlock*> owned;
    const auto all = function->GetHIRBlocks();
    for (const auto* member : all)
        if (member) owned.push_back(member);
    std::sort(owned.begin(), owned.end(), std::less<>{});
    if (std::adjacent_find(owned.begin(), owned.end()) != owned.end())
        return Reject(FailureKind::CFG);
    const auto owns = [&](const auto* member) {
        return member && std::binary_search(owned.begin(), owned.end(), member, std::less<>{});
    };
    if (!owns(function->GetEntryBlock())) return Reject(FailureKind::CFG);
    for (const auto& member : function->GetHIRBlockList())
        if (!owns(&member)) return Reject(FailureKind::CFG);
    for (const auto* root : function->GetExternalEntryRoots())
        if (!owns(root)) return Reject(FailureKind::CFG);
    for (const auto& link : function->GetExternalDirectLinks())
        if (!owns(link.source)) return Reject(FailureKind::CFG);
    for (size_t i = 0; i < all.size(); ++i) {
        const auto* member = all[i];
        if (!member) continue;
        if (member->GetOrderId() != i || !member->GetBlock()) return Reject(FailureKind::CFG);
        members.push_back(member->GetBlock());
        if (member->GetCallReturnBlock() && !owns(member->GetCallReturnBlock()))
            return Reject(FailureKind::CFG);
        const auto predecessors = member->GetPredecessors();
        const auto successors = member->GetSuccessors();
        if (predecessors.size() != member->GetIncomingEdges().size() ||
            successors.size() != member->GetOutgoingEdges().size())
            return Reject(FailureKind::CFG);
        size_t next{};
        for (const auto& edge : member->GetIncomingEdges())
            if (!owns(edge.GetSourceBlock()) || edge.GetTargetBlock() != member ||
                predecessors[next++] != edge.GetSourceBlock())
                return Reject(FailureKind::CFG);
        next = 0;
        for (const auto& edge : member->GetOutgoingEdges())
            if (!owns(edge.GetTargetBlock()) || edge.GetSourceBlock() != member ||
                successors[next++] != edge.GetTargetBlock())
                return Reject(FailureKind::CFG);
    }
    std::sort(members.begin(), members.end(), std::less<>{});
    if (std::adjacent_find(members.begin(), members.end()) != members.end())
        return Reject(FailureKind::Blocks);

    const auto count = emitted_blocks.size();
    std::vector<const ir::HIRBlock*> hir(count);
    size_t rpo_count{};
    for (const auto& block : function->GetHIRBlocksRPO()) {
        if (!owns(&block)) return Reject(FailureKind::CFG);
        const auto id = BlockIndex(block.GetBlock());
        if (id == missing || hir[id]) return Reject(FailureKind::Blocks);
        hir[id] = &block;
        ++rpo_count;
    }
    if (rpo_count != count) return Reject(FailureKind::Blocks);
    std::vector<std::vector<u32>> predecessors(count);
    std::vector<bool> roots(count), returned(count);
    for (size_t i = 0; i < count; ++i) {
        const auto* block = hir[i];
        roots[i] = block == function->GetEntryBlock();
        for (const auto& edge : block->GetIncomingEdges()) {
            const auto* source = edge.GetSourceBlock();
            const auto id = BlockIndex(source->GetBlock());
            if (id != missing) predecessors[i].push_back(id);
            else if (source == function->GetEntryBlock()) roots[i] = true;
        }
        if (const auto* target = block->GetCallReturnBlock()) {
            const auto id = BlockIndex(target->GetBlock());
            if (id != missing) {
                predecessors[id].push_back(static_cast<u32>(i));
                returned[id] = true;
            }
        }
    }
    for (const auto* root : function->GetExternalEntryRoots()) {
        const auto id = BlockIndex(root->GetBlock());
        if (id != missing) roots[id] = true;
    }
    for (size_t i = 0; i < count; ++i)
        if (!hir[i]->HasIncomingEdges() && !returned[i]) roots[i] = true;

    // Independent full dominator reference. Production SSA uses a different
    // path-avoidance query and no result from it participates in this audit.
    const size_t words = (count + 63) / 64;
    dominators.assign(count, std::vector<u64>(words, ~u64{}));
    std::vector<u64> next(words);
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < count; ++i) {
            const bool entry = roots[i] || predecessors[i].empty();
            std::fill(next.begin(), next.end(), entry ? u64{} : ~u64{});
            if (!entry)
                for (const auto previous : predecessors[i])
                    for (size_t word = 0; word < words; ++word)
                        next[word] &= dominators[previous][word];
            next[i / 64] |= u64{1} << (i % 64);
            if (next != dominators[i]) {
                dominators[i] = next;
                changed = true;
            }
        }
    }
    return true;
}

bool IRReference::Verify(std::span<const ir::Block* const> members,
                         u32 instruction_capacity) {
    // Validate every producer before checking a possibly forward operand's
    // view. The latter may inspect the producer's opcode and result metadata.
    for (const auto& definition : definitions)
        if (!definition.instruction->HasValidResultType()) return Reject(FailureKind::Arguments);
    std::vector<bool> numbered(instruction_capacity);
    // Identity is checked before a Value's type/id accesses its definition.
    // A foreign instruction with a colliding id is never an owned definition.
    for (const auto* block : members) {
        const bool emitted = BlockIndex(block) != missing;
        for (const auto& instruction : block->GetInstList()) {
            if (emitted) {
                const auto id = instruction.Id();
                if (id == ir::Inst::invalid_id || id >= numbered.size() || numbered[id])
                    return Reject(FailureKind::Numbering);
                numbered[id] = true;
            }
            bool owned = true;
            instruction.ForEachValue([&](ir::Value value) { owned &= Owner(value.Def()) != nullptr; });
            if (!owned) return Reject(FailureKind::Owner);
            if (!instruction.HasValidArguments())
                return Reject(FailureKind::Arguments);
        }
        bool owned = true;
        VisitImplicitUses(*block, 0, [&](ir::Value value, u32) {
            owned &= Owner(value.Def()) != nullptr;
        });
        if (!owned) return Reject(FailureKind::Owner);
        if (!CheckImplicitUses(*block, 0, [](ir::Value, u32) {}))
            return Reject(FailureKind::Terminal);
    }

    struct Jump {
        const ir::Inst* branch;
        u32 source, target{missing};
    };
    struct LocalFlow {
        u32 length{};
        std::vector<Jump> jumps;
    };
    std::vector<LocalFlow> local(block_ids.size());
    for (const auto* block : members) {
        const auto id = BlockIndex(block);
        if (id == missing) continue;
        auto& flow = local[id];
        for (const auto& instruction : block->GetInstList()) {
            const auto position = flow.length++;
            const auto op = instruction.GetOp();
            if (op == ir::OpCode::Goto || op == ir::OpCode::NotGoto)
                flow.jumps.push_back({&instruction, position});
            else if (op == ir::OpCode::BindLabel) {
                const auto* branch = instruction.GetArg<ir::Value>(0).Def();
                auto jump = std::find_if(flow.jumps.begin(), flow.jumps.end(),
                        [&](const auto& entry) { return entry.branch == branch; });
                if (jump == flow.jumps.end() || jump->target != missing)
                    return Reject(FailureKind::LocalControl);
                jump->target = position;
            }
        }
        for (const auto& jump : flow.jumps)
            if (jump.target == missing) return Reject(FailureKind::LocalControl);
    }
    const auto local_dominates = [&](const Definition& definition, u32 use) {
        if (definition.position >= use) return false;
        const auto& flow = local[BlockIndex(definition.block)];
        // Both conditional local branches have a fallthrough. With their
        // required unique forward bindings, a definition can be bypassed iff
        // an earlier branch lands after it and no later than this use. For an
        // exported value, the use is the source block's exit. This reference
        // does not reuse production fragment ids, worklists or SSA results.
        return std::none_of(flow.jumps.begin(), flow.jumps.end(), [&](const auto& jump) {
            return jump.source < definition.position && jump.target > definition.position &&
                   jump.target <= use;
        });
    };
    for (const auto* block : members) {
        const auto id = BlockIndex(block);
        if (id == missing) continue;
        const auto verify_use = [&](ir::Value value, u32 position) {
            const auto* definition = Find(value.Def());
            if (!definition || BlockIndex(definition->block) == missing) return false;
            if (definition->block == block) return local_dominates(*definition, position);
            return Dominates(definition->block, block) &&
                   local_dominates(*definition, local[BlockIndex(definition->block)].length);
        };
        u32 position{};
        for (const auto& instruction : block->GetInstList()) {
            bool valid = true;
            // BindLabel reads a control identity, whose unique forward binding
            // was checked above; it does not read a materialized SSA result.
            if (instruction.GetOp() != ir::OpCode::BindLabel)
                instruction.ForEachValue([&](ir::Value value) { valid &= verify_use(value, position); });
            if (!valid) return Reject(FailureKind::Dominance);
            ++position;
        }
        bool valid = true;
        VisitImplicitUses(*block, position, [&](ir::Value value, u32 point) {
            valid &= verify_use(value, point);
        });
        if (!valid) return Reject(FailureKind::Dominance);
    }
    return true;
}

}  // namespace swift::runtime::compile::audit
