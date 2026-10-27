#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_map>
#include <vector>
#include "runtime/compile/analysis/block_summary.h"
#include "runtime/compile/analysis/pseudo_flags.h"
#include "runtime/ir/block.h"

namespace swift::runtime::compile {

// Built for one final numbered IR block. Physical allocation changes do not
// invalidate semantic consumers; preparation releases it after its last user.
class BlockAnalysisIndex {
public:
    BlockAnalysisIndex() = default;
    BlockAnalysisIndex(const BlockAnalysisIndex&) = delete;
    BlockAnalysisIndex& operator=(const BlockAnalysisIndex&) = delete;
    BlockAnalysisIndex(BlockAnalysisIndex&&) = default;
    BlockAnalysisIndex& operator=(BlockAnalysisIndex&&) = default;
    struct Use {
        const ir::Inst* consumer;
        u32 count;
    };
    // Optional caller-owned storage for small graphs. It must outlive the
    // index; exceeding capacity transparently switches to the ordinary map.
    struct CompactUses {
        size_t count{};
        std::array<const ir::Inst*, 16> defs{};
        std::array<Use, 16> uses{};
    };

    void Build(const ir::Block* block, CompactUses* compact = nullptr, bool collect_flags = false) {
        BuildImpl(block, compact, collect_flags, nullptr);
        PrepareUses();
    }

private:
    friend class SemanticFacts;
    void BuildImpl(const ir::Block* block,
                   CompactUses* compact,
                   bool collect_flags,
                   const BlockSummary* prepared_summary) {
        instructions.clear();
        uses.clear();
        dense_ids = false;
        first_id = 0;
        for (auto& consumers : dense_uses) consumers.clear();
        dense_resolved_uses.clear();
        dense_resolved_present.clear();
        uses_ready = resolved_ready = false;
        has_bitcasts = false;
        summary = prepared_summary ? *prepared_summary : BlockSummary{};
        if (prepared_summary) instructions.reserve(summary.InstructionCount());
        pseudo_flags = {};
        resolved_uses.clear();
        compact_index = compact;
        if (compact_index) compact_index->count = 0;
        for (auto& inst : block->GetInstList()) {
            const auto op = inst.GetOp();
            if (collect_flags && (op == ir::OpCode::SaveFlags || op == ir::OpCode::BranchOnlyFlags))
                pseudo_flags.RecordPseudo(
                        inst, block->GetInstList().front().Id(), block->GetInstList().back().Id());
            if (!prepared_summary) summary.Record(inst);
            has_bitcasts |= inst.IsBitCastOperation();
            instructions.push_back(&inst);
        }
        ASSERT(summary.InstructionCount() == instructions.size());
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
        }
    }

public:
    [[nodiscard]] bool UsesReady() const { return uses_ready; }
    // Roster/presence queries do not need reverse edges. Preparation chooses
    // the first rule that needs them; all const edge queries stay read-only.
    void PrepareUses() {
        if (uses_ready) return;
        if (dense_ids) dense_uses.resize(instructions.size());
        const auto record_use = [](std::vector<Use>& consumers, const ir::Inst* consumer) {
            if (!consumers.empty() && consumers.back().consumer == consumer) {
                ++consumers.back().count;
            } else {
                consumers.push_back({consumer, 1});
            }
        };
        for (const auto* inst : instructions) {
            inst->ForEachValue([&](ir::Value value) {
                if (!value.Def()) return;
                has_bitcasts |= value.Def()->IsBitCastOperation();
                if (compact_index) {
                    auto& compact_count = compact_index->count;
                    auto& compact_defs = compact_index->defs;
                    auto& compact_uses = compact_index->uses;
                    size_t at = 0;
                    while (at < compact_count && compact_defs[at] != value.Def()) ++at;
                    while (at < compact_count && compact_defs[at] == value.Def() &&
                           compact_uses[at].consumer != inst)
                        ++at;
                    if (at < compact_count && compact_defs[at] == value.Def()) {
                        ++compact_uses[at].count;
                        return;
                    }
                    if (compact_count < compact_uses.size()) {
                        std::move_backward(compact_defs.begin() + at,
                                           compact_defs.begin() + compact_count,
                                           compact_defs.begin() + compact_count + 1);
                        std::move_backward(compact_uses.begin() + at,
                                           compact_uses.begin() + compact_count,
                                           compact_uses.begin() + compact_count + 1);
                        compact_defs[at] = value.Def();
                        compact_uses[at] = {inst, 1};
                        ++compact_count;
                        return;
                    }
                    for (size_t i = 0; i < compact_count; ++i)
                        uses[compact_defs[i]].push_back(compact_uses[i]);
                    compact_index = nullptr;
                }
                if (const auto slot = InstructionSlot(value.Def())) {
                    record_use(dense_uses[*slot], inst);
                    return;
                }
                record_use(uses[value.Def()], inst);
            });
        }
        uses_ready = true;
    }

    std::span<const Use> Uses(const ir::Inst* definition) const {
        ASSERT_MSG(uses_ready, "direct consumer index requires explicit preparation");
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
        if (const auto slot = InstructionSlot(definition)) return dense_uses[*slot];
        const auto it = uses.find(definition);
        return it == uses.end() ? std::span<const Use>{} : it->second;
    }

    // Preparation can prove an empty answer from the maintained global count.
    // A nonzero count still needs the shared local graph, including when all
    // actual consumers belong to another block or are terminal occurrences.
    std::span<const Use> PrepareUsesOf(const ir::Inst* definition) {
        if (!definition || definition->GetUses(false) == 0) return {};
        if (!uses_ready) PrepareUses();
        return Uses(definition);
    }

    // A global pseudo count proves only presence, never local ownership.
    [[nodiscard]] bool PrepareHasPseudoUse(const ir::Inst* owner,
                                           std::optional<ir::OpCode> opcode = std::nullopt) {
        if (!owner || owner->GetUses(false) == owner->GetUses()) return false;
        if (!uses_ready) PrepareUses();
        return opcode ? HasPseudoUse(owner, *opcode) : HasPseudoUse(owner);
    }

    std::span<const ir::Inst* const> Instructions() const { return instructions; }
    template <class Visitor> void ForEachUsedDefinition(Visitor&& visit) const {
        ASSERT_MSG(uses_ready, "direct consumer index requires explicit preparation");
        if (compact_index) {
            const ir::Inst* previous = nullptr;
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
    [[nodiscard]] std::optional<size_t> InstructionPosition(const ir::Inst* inst) const {
        if (!inst || instructions.empty()) return std::nullopt;
        // ReId and RPO IDs normally give a dense block span. Verify the slot's
        // ID and pointer, keeping the ordered lookup for sparse/manual layouts.
        const auto first = instructions.front()->Id();
        const size_t offset = inst->Id() >= first ? inst->Id() - first : instructions.size();
        if (offset < instructions.size() && instructions[offset]->Id() == inst->Id())
            return instructions[offset] == inst ? std::optional{offset} : std::nullopt;
        const auto found =
                std::lower_bound(instructions.begin(),
                                 instructions.end(),
                                 inst->Id(),
                                 [](const ir::Inst* node, u32 id) { return node->Id() < id; });
        return found != instructions.end() && *found == inst
                       ? std::optional{static_cast<size_t>(found - instructions.begin())}
                       : std::nullopt;
    }
    bool Contains(const ir::Inst* inst) const { return InstructionPosition(inst).has_value(); }
    [[nodiscard]] const BlockSummary& Summary() const { return summary; }
    bool HasMemoryInstructions() const { return summary.HasMemoryInstructions(); }
    bool HasFPRInstructions() const { return summary.HasFPRInstructions(); }
    bool HasFPRWrites() const { return summary.HasFPRWrites(); }
    bool HasVectorConstants() const { return summary.HasVectorConstants(); }
    bool HasConsumerDecisions() const { return summary.HasConsumerDecisions(); }
    bool HasBooleanSelects() const { return summary.HasBooleanSelects(); }
    const PseudoFlagsPlan& PseudoFlags() const { return pseudo_flags; }
    void SetPseudoFlags(PseudoFlagsPlan flags) { pseudo_flags = std::move(flags); }

    // Pseudo-operation uses from the use index (un-chaining step 3):
    // planning-time pseudo-use queries without an owner chain.
    // A pseudo use is any consumer with a pseudo opcode; consumers
    // that require the owner relationship check arg 0 themselves.
    [[nodiscard]] bool HasPseudoUse(const ir::Inst* owner) const {
        for (const auto& use : Uses(owner))
            if (use.consumer->IsPseudoOperation()) return true;
        return false;
    }
    [[nodiscard]] bool HasPseudoUse(const ir::Inst* owner, ir::OpCode op) const {
        for (const auto& use : Uses(owner))
            if (use.consumer->GetOp() == op) return true;
        return false;
    }
    template <class Visitor> void ForEachPseudoUse(const ir::Inst* owner, Visitor&& visit) const {
        for (const auto& use : Uses(owner))
            if (use.consumer->IsPseudoOperation()) visit(*use.consumer);
    }

    // Preparation explicitly requests the bitcast-aware index only for a family
    // that needs it. All const consumer queries are allocation-free.
    std::span<const Use> ResolvedUses(const ir::Inst* definition) const {
        ASSERT_MSG(uses_ready, "direct consumer index requires explicit preparation");
        if (!definition) return {};
        if (!has_bitcasts) return Uses(definition);
        ASSERT_MSG(resolved_ready, "resolved consumer index requires explicit preparation");
        if (definition->IsBitCastOperation()) return {};
        if (const auto slot = InstructionSlot(definition)) {
            return dense_resolved_present[*slot] ? std::span<const Use>{dense_resolved_uses[*slot]}
                                                 : Uses(definition);
        }
        const auto found = resolved_uses.find(definition);
        return found == resolved_uses.end() ? Uses(definition) : found->second;
    }
    u32 LastInstructionUse(const ir::Inst* definition) const {
        const auto consumers = ResolvedUses(definition);
        return consumers.empty() ? definition->Id()
                                 : std::max<u32>(definition->Id(), consumers.back().consumer->Id());
    }
    u32 DirectUseCount(const ir::Inst* definition) const {
        u32 count = 0;
        for (const auto& use : Uses(definition)) count += use.count;
        return count;
    }

    // IDs follow block order after ReId/IdByRPO. Only inspect the actual proof
    // window instead of scanning the whole block for each publication.
    std::span<const ir::Inst* const> Between(u32 after, u32 before) const {
        const auto first = std::upper_bound(
                instructions.begin(), instructions.end(), after, [](u32 id, const ir::Inst* inst) {
                    return id < inst->Id();
                });
        const auto last = std::lower_bound(
                first, instructions.end(), before, [](const ir::Inst* inst, u32 id) {
                    return inst->Id() < id;
                });
        return {first, last};
    }

    [[nodiscard]] bool DenseInstructionIDs() const { return dense_ids; }
    [[nodiscard]] const ir::Inst* FindInstruction(u32 id) const {
        if (dense_ids)
            return id >= first_id && id - first_id < instructions.size()
                           ? instructions[id - first_id]
                           : nullptr;
        const auto found = std::find_if(instructions.begin(),
                                        instructions.end(),
                                        [id](const ir::Inst* inst) { return inst->Id() == id; });
        return found == instructions.end() ? nullptr : *found;
    }

    [[nodiscard]] std::optional<size_t> InstructionSlot(const ir::Inst* def) const {
        if (!dense_ids || !def || def->Id() < first_id) return std::nullopt;
        const size_t slot = def->Id() - first_id;
        if (slot >= instructions.size() || instructions[slot] != def) {
            return std::nullopt;
        }
        return slot;
    }

    [[nodiscard]] bool ResolvedUsesReady() const {
        return uses_ready && (!has_bitcasts || resolved_ready);
    }

    void PrepareResolvedUses() {
        PrepareUses();
        if (resolved_ready) return;
        if (!has_bitcasts) {
            resolved_ready = true;
            return;
        }
        resolved_uses.clear();
        if (dense_ids) {
            dense_resolved_uses.resize(instructions.size());
            for (auto& consumers : dense_resolved_uses) consumers.clear();
            dense_resolved_present.assign(instructions.size(), false);
        }
        const auto append_alias = [&](const ir::Inst* alias, std::span<const Use> children) {
            if (!alias->IsBitCastOperation()) return;
            auto* root = alias;
            while (root && root->IsBitCastOperation()) root = root->GetArg<ir::Value>(0).Def();
            if (!root) return;
            if (const auto slot = InstructionSlot(root)) {
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
                while (last < compact_index->count &&
                       compact_index->defs[first] == compact_index->defs[last])
                    ++last;
                append_alias(compact_index->defs[first],
                             {compact_index->uses.data() + first, last - first});
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
                if (count && consumers[count - 1].consumer == use.consumer)
                    consumers[count - 1].count += use.count;
                else
                    consumers[count++] = use;
            }
            consumers.resize(count);
        };
        for (size_t i = 0; i < dense_resolved_present.size(); ++i) {
            if (dense_resolved_present[i]) finish(dense_resolved_uses[i]);
        }
        for (auto& [root, consumers] : resolved_uses) finish(consumers);
        resolved_ready = true;
    }

private:
    std::vector<const ir::Inst*> instructions;
    CompactUses* compact_index{};
    bool dense_ids{};
    u32 first_id{};
    std::vector<std::vector<Use>> dense_uses;
    std::vector<std::vector<Use>> dense_resolved_uses;
    std::vector<char> dense_resolved_present;
    std::unordered_map<const ir::Inst*, std::vector<Use>> uses;
    bool uses_ready{}, resolved_ready{};
    bool has_bitcasts{};
    BlockSummary summary;
    PseudoFlagsPlan pseudo_flags;
    std::unordered_map<const ir::Inst*, std::vector<Use>> resolved_uses;
};

}  // namespace swift::runtime::compile
