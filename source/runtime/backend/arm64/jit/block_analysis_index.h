#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_map>
#include <vector>
#include "runtime/ir/block.h"
#include "runtime/backend/arm64/alloc/pseudo_flags_plan.h"

namespace swift::runtime::backend::arm64 {

// Built once after IR/RA are final. Pointers, order and operand multiplicities
// remain valid through emission; rebuild when entering another block.
class BlockAnalysisIndex {
public:
    struct Use {
        ir::Inst* consumer;
        u32 count;
    };
    // Optional caller-owned storage for small graphs. It must outlive the
    // index; exceeding capacity transparently switches to the ordinary map.
    struct CompactUses {
        size_t count{};
        std::array<ir::Inst*, 16> defs{};
        std::array<Use, 16> uses{};
    };

    void Build(ir::Block* block, CompactUses* compact = nullptr, bool collect_flags = false) {
        instructions.clear();
        uses.clear();
        dense_ids = false;
        first_id = 0;
        for (auto& consumers : dense_uses) consumers.clear();
        dense_resolved_uses.clear();
        dense_resolved_present.clear();
        dense_following_writes.clear();
        resolved_ready = false;
        has_bitcasts = false;
        has_memory = false;
        has_fpr = false;
        has_vector_constants = false;
        has_consumer_decisions = false;
        has_boolean_selects = false;
        pseudo_flags = {};
        resolved_uses.clear();
        following_writes.clear();
        compact_index = compact;
        if (compact_index) compact_index->count = 0;
        for (auto& inst : block->GetInstList()) {
            const auto op = inst.GetOp();
            if (collect_flags && (op == ir::OpCode::SaveFlags || op == ir::OpCode::BranchOnlyFlags))
                pseudo_flags.RecordPseudo(inst, block->GetInstList().front().Id(), block->GetInstList().back().Id());
            has_memory |= op >= ir::OpCode::LoadMemory && op <= ir::OpCode::StoreMemoryTSO;
            has_fpr |= op == ir::OpCode::GetHostFPR || op == ir::OpCode::SetHostFPR ||
                       ir::IsFloatValueType(inst.ReturnType());
            has_vector_constants |= op == ir::OpCode::VecLoadConst;
            has_consumer_decisions |= op == ir::OpCode::CondSet || op == ir::OpCode::TestZero ||
                    op == ir::OpCode::TestNotZero || op == ir::OpCode::Sse42Str ||
                    op == ir::OpCode::VecFCmp || op == ir::OpCode::FCmpCondSet ||
                    op == ir::OpCode::TestFlags || op == ir::OpCode::TestNotFlags || op == ir::OpCode::LocalCondSet ||
                    op == ir::OpCode::ClearFlags || op == ir::OpCode::PublishFCmpFlags ||
                    op == ir::OpCode::Div128 || op == ir::OpCode::Div128Remainder ||
                    op == ir::OpCode::Cpuid || op == ir::OpCode::CpuidUpper;
            has_bitcasts |= inst.IsBitCastOperation();
            has_boolean_selects |= op == ir::OpCode::Select;
            instructions.push_back(&inst);
        }
        // Dense-id storage: after ReId/RPO the block's ids form a dense
        // span (Contains() relies on the same property), so def->uses can
        // be indexed by slot (Id - first_id) instead of hashed by
        // pointer. Definitions outside the block, sparse/manual id
        // layouts and the compact small-graph storage keep the hash map.
        if (!compact_index && !instructions.empty()) {
            first_id = instructions.front()->Id();
            dense_ids = true;
            for (size_t i = 0; i < instructions.size(); ++i) {
                if (instructions[i]->Id() != first_id + i) {
                    dense_ids = false;
                    break;
                }
            }
            if (dense_ids) dense_uses.resize(instructions.size());
        }
        const auto record_use = [](std::vector<Use>& consumers, ir::Inst* consumer) {
            if (!consumers.empty() && consumers.back().consumer == consumer) {
                ++consumers.back().count;
            } else {
                consumers.push_back({consumer, 1});
            }
        };
        for (auto& inst : block->GetInstList()) {
            inst.ForEachValue([&](ir::Value value) {
                if (!value.Def()) return;
                has_bitcasts |= value.Def()->IsBitCastOperation();
                if (compact_index) {
                    auto& compact_count = compact_index->count;
                    auto& compact_defs = compact_index->defs;
                    auto& compact_uses = compact_index->uses;
                    size_t at = 0;
                    while (at < compact_count && compact_defs[at] != value.Def()) ++at;
                    while (at < compact_count && compact_defs[at] == value.Def() &&
                           compact_uses[at].consumer != &inst) ++at;
                    if (at < compact_count && compact_defs[at] == value.Def()) {
                        ++compact_uses[at].count;
                        return;
                    }
                    if (compact_count < compact_uses.size()) {
                        std::move_backward(compact_defs.begin() + at, compact_defs.begin() + compact_count,
                                           compact_defs.begin() + compact_count + 1);
                        std::move_backward(compact_uses.begin() + at, compact_uses.begin() + compact_count,
                                           compact_uses.begin() + compact_count + 1);
                        compact_defs[at] = value.Def(); compact_uses[at] = {&inst, 1};
                        ++compact_count;
                        return;
                    }
                    for (size_t i = 0; i < compact_count; ++i)
                        uses[compact_defs[i]].push_back(compact_uses[i]);
                    compact_index = nullptr;
                }
                if (const auto slot = SlotOf(value.Def())) {
                    record_use(dense_uses[*slot], &inst);
                    return;
                }
                record_use(uses[value.Def()], &inst);
            });
        }
    }

    std::span<const Use> Uses(ir::Inst* definition) const {
        if (compact_index) {
            const auto compact_count = compact_index->count;
            const auto& compact_defs = compact_index->defs;
            const auto& compact_uses = compact_index->uses;
            size_t first = 0;
            while (first < compact_count && compact_defs[first] != definition) ++first;
            size_t last = first;
            while (last < compact_count && compact_defs[last] == definition) ++last;
            return {compact_uses.data() + first, last - first};
        }
        if (const auto slot = SlotOf(definition)) return dense_uses[*slot];
        const auto it = uses.find(definition);
        return it == uses.end() ? std::span<const Use>{} : it->second;
    }

    std::span<ir::Inst* const> Instructions() const { return instructions; }
    template<class Visitor> void ForEachUsedDefinition(Visitor&& visit) const {
        if (compact_index) {
            ir::Inst* previous = nullptr;
            for (size_t i = 0; i < compact_index->count; ++i) {
                auto* definition = compact_index->defs[i];
                if (definition != previous) visit(definition);
                previous = definition;
            }
        } else {
            if (dense_ids) {
                for (size_t i = 0; i < dense_uses.size(); ++i) {
                    if (!dense_uses[i].empty()) visit(instructions[i]);
                }
            }
            for (const auto& [definition, consumers] : uses) visit(definition);
        }
    }
    bool Contains(const ir::Inst* inst) const {
        if (!inst || instructions.empty()) return false;
        // ReId and RPO IDs normally give a dense block span. Verify the slot's
        // ID and pointer, keeping the ordered lookup for sparse/manual layouts.
        const auto first = instructions.front()->Id();
        const size_t offset = inst->Id() >= first ? inst->Id() - first : instructions.size();
        if (offset < instructions.size() && instructions[offset]->Id() == inst->Id())
            return instructions[offset] == inst;
        const auto found = std::lower_bound(instructions.begin(), instructions.end(), inst->Id(),
                [](const ir::Inst* node, u32 id) { return node->Id() < id; });
        return found != instructions.end() && *found == inst;
    }
    bool HasMemoryInstructions() const { return has_memory; }
    bool HasFPRInstructions() const { return has_fpr; }
    bool HasVectorConstants() const { return has_vector_constants; }
    bool HasConsumerDecisions() const { return has_consumer_decisions; }
    bool HasBooleanSelects() const { return has_boolean_selects; }
    const PseudoFlagsPlan& PseudoFlags() const { return pseudo_flags; }
    PseudoFlagsPlan ReleasePseudoFlags() { return std::move(pseudo_flags); }

    // Pseudo-operation uses from the use index (un-chaining step 3):
    // planning-time pseudo-use queries without an owner chain.
    // A pseudo use is any consumer with a pseudo opcode; consumers
    // that require the owner relationship check arg 0 themselves.
    [[nodiscard]] bool HasPseudoUse(const ir::Inst* owner) const {
        for (const auto& use : Uses(const_cast<ir::Inst*>(owner)))
            if (use.consumer->IsPseudoOperation()) return true;
        return false;
    }
    [[nodiscard]] bool HasPseudoUse(const ir::Inst* owner, ir::OpCode op) const {
        for (const auto& use : Uses(const_cast<ir::Inst*>(owner)))
            if (use.consumer->GetOp() == op) return true;
        return false;
    }
    template<class Visitor> void ForEachPseudoUse(const ir::Inst* owner, Visitor&& visit) const {
        for (const auto& use : Uses(const_cast<ir::Inst*>(owner)))
            if (use.consumer->IsPseudoOperation()) visit(*use.consumer);
    }

    // Shared bitcast-aware def/use facts. Built lazily so families that need
    // only direct consumers do not pay for a second index.
    std::span<const Use> ResolvedUses(ir::Inst* definition) const {
        if (!has_bitcasts) return Uses(definition);
        BuildResolvedUses();
        if (definition->IsBitCastOperation()) return {};
        if (const auto slot = SlotOf(definition)) {
            return dense_resolved_present[*slot]
                    ? std::span<const Use>{dense_resolved_uses[*slot]}
                    : Uses(definition);
        }
        const auto found = resolved_uses.find(definition);
        return found == resolved_uses.end() ? Uses(definition) : found->second;
    }
    u32 LastInstructionUse(ir::Inst* definition) const {
        const auto consumers = ResolvedUses(definition);
        return consumers.empty() ? definition->Id() : std::max<u32>(definition->Id(), consumers.back().consumer->Id());
    }
    u32 DirectUseCount(ir::Inst* definition) const {
        u32 count = 0;
        for (const auto& use : Uses(definition)) count += use.count;
        return count;
    }

    // IDs follow block order after ReId/IdByRPO. Only inspect the actual proof
    // window instead of scanning the whole block for each publication.
    std::span<ir::Inst* const> Between(u32 after, u32 before) const {
        const auto first = std::upper_bound(instructions.begin(), instructions.end(), after,
                [](u32 id, const ir::Inst* inst) { return id < inst->Id(); });
        const auto last = std::lower_bound(first, instructions.end(), before,
                [](const ir::Inst* inst, u32 id) { return inst->Id() < id; });
        return {first, last};
    }

    template<class IsObserver, class IsFullWrite>
    void BuildWriteSuccessors(IsObserver observes, IsFullWrite full_write) {
        following_writes.clear();
        dense_following_writes.clear();
        if (dense_ids) dense_following_writes.assign(instructions.size(), nullptr);
        std::array<ir::Inst*, 32> next{};
        for (auto* inst : std::views::reverse(instructions)) {
            const auto op = inst->GetOp();
            if (op == ir::OpCode::SetHostGPR) {
                const auto home = inst->GetArg<ir::Imm>(1).Get();
                if (home < next.size() && next[home]) {
                    if (const auto slot = SlotOf(inst)) {
                        dense_following_writes[*slot] = next[home];
                    } else {
                        following_writes.emplace(inst, next[home]);
                    }
                }
            }
            if (observes(*inst)) {
                next.fill(nullptr);
            } else if (op == ir::OpCode::GetHostGPR) {
                const auto home = inst->GetArg<ir::Imm>(0).Get();
                if (home < next.size()) next[home] = nullptr;
            } else if (op == ir::OpCode::SetHostGPR) {
                const auto home = inst->GetArg<ir::Imm>(1).Get();
                if (home < next.size()) next[home] = full_write(*inst) ? inst : nullptr;
            }
        }
    }

    ir::Inst* FollowingWrite(ir::Inst* write) const {
        if (const auto slot = SlotOf(write)) {
            return *slot < dense_following_writes.size()
                    ? dense_following_writes[*slot] : nullptr;
        }
        const auto it = following_writes.find(write);
        return it == following_writes.end() ? nullptr : it->second;
    }

private:
    [[nodiscard]] std::optional<size_t> SlotOf(const ir::Inst* def) const {
        if (!dense_ids || !def || def->Id() < first_id) return std::nullopt;
        const size_t slot = def->Id() - first_id;
        if (slot >= instructions.size() || instructions[slot] != def) {
            return std::nullopt;
        }
        return slot;
    }

    void BuildResolvedUses() const {
        if (resolved_ready) return;
        if (dense_ids) {
            dense_resolved_uses.resize(instructions.size());
            for (auto& consumers : dense_resolved_uses) consumers.clear();
            dense_resolved_present.assign(instructions.size(), false);
        }
        const auto append_alias = [&](ir::Inst* alias, std::span<const Use> children) {
            if (!alias->IsBitCastOperation()) return;
            auto* root = alias;
            while (root && root->IsBitCastOperation()) root = root->GetArg<ir::Value>(0).Def();
            if (!root) return;
            if (const auto slot = SlotOf(root)) {
                auto& entry = dense_resolved_uses[*slot];
                if (!dense_resolved_present[*slot]) {
                    dense_resolved_present[*slot] = true;
                    const auto direct = Uses(root);
                    entry.assign(direct.begin(), direct.end());
                }
                entry.insert(entry.end(), children.begin(), children.end());
                return;
            }
            auto [entry, inserted] = resolved_uses.try_emplace(root);
            if (inserted) {
                const auto direct = Uses(root);
                entry->second.assign(direct.begin(), direct.end());
            }
            entry->second.insert(entry->second.end(), children.begin(), children.end());
        };
        if (compact_index) {
            for (size_t first = 0; first < compact_index->count;) {
                size_t last = first + 1;
                while (last < compact_index->count && compact_index->defs[first] == compact_index->defs[last]) ++last;
                append_alias(compact_index->defs[first], {compact_index->uses.data() + first, last - first});
                first = last;
            }
        } else {
            if (dense_ids) {
                for (size_t i = 0; i < dense_uses.size(); ++i) {
                    if (!dense_uses[i].empty()) append_alias(instructions[i], dense_uses[i]);
                }
            }
            for (const auto& [definition, consumers] : uses) append_alias(definition, consumers);
        }
        const auto finish = [](std::vector<Use>& consumers) {
            std::sort(consumers.begin(), consumers.end(), [](const Use& a, const Use& b) {
                return a.consumer->Id() < b.consumer->Id();
            });
            size_t count = 0;
            for (const auto use : consumers) {
                if (count && consumers[count - 1].consumer == use.consumer) consumers[count - 1].count += use.count;
                else consumers[count++] = use;
            }
            consumers.resize(count);
        };
        for (size_t i = 0; i < dense_resolved_present.size(); ++i) {
            if (dense_resolved_present[i]) finish(dense_resolved_uses[i]);
        }
        for (auto& [root, consumers] : resolved_uses) finish(consumers);
        resolved_ready = true;
    }
    std::vector<ir::Inst*> instructions;
    CompactUses* compact_index{};
    bool dense_ids{};
    u32 first_id{};
    std::vector<std::vector<Use>> dense_uses;
    std::vector<ir::Inst*> dense_following_writes;
    mutable std::vector<std::vector<Use>> dense_resolved_uses;
    mutable std::vector<char> dense_resolved_present;
    std::unordered_map<ir::Inst*, std::vector<Use>> uses;
    mutable bool resolved_ready{};
    bool has_bitcasts{};
    bool has_memory{}, has_fpr{}, has_vector_constants{};
    bool has_consumer_decisions{};
    bool has_boolean_selects{};
    PseudoFlagsPlan pseudo_flags;
    mutable std::unordered_map<ir::Inst*, std::vector<Use>> resolved_uses;
    std::unordered_map<ir::Inst*, ir::Inst*> following_writes;
};

} // namespace swift::runtime::backend::arm64
