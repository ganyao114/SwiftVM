#include "internal.h"

#include <map>
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/analysis/uses.h"

namespace swift::runtime::compile::detail {
namespace {
// Positions are unique per consumer; occurrences retain every raw typed use.
// Keep source and raw allocation IDs independent when a physical alias has
// not been installed. The ordered keys preserve ownership selection order.
struct SpillValueUses {
    Vector<size_t> positions;
    u32 occurrences{};
    bool has_occurrences{}, transferable{true}, handoff{true}, prepared{};
};
using SpillValues = std::map<u32, SpillValueUses>;

Vector<u32> TerminalAllocationUses(const Terminal& terminal, const AllocationWriter& allocation) {
    Vector<u32> uses;
    VisitTerminalValues(terminal, [&](Value value) {
        if (value.Defined()) uses.push_back(allocation.AllocationId(value));
    });
    std::sort(uses.begin(), uses.end());
    uses.erase(std::unique(uses.begin(), uses.end()), uses.end());
    return uses;
}

size_t SegmentDefinition(std::span<const Inst* const> instructions, u32 value_id, bool dense_ids) {
    if (instructions.empty()) return instructions.size();
    if (dense_ids) {
        const auto first = instructions.front()->Id();
        return value_id >= first && value_id - first < instructions.size() ? value_id - first
                                                                           : instructions.size();
    }
    // Borrow the block index's verified dense property. Sparse/manual IDs
    // retain the original ordered-instruction search, with no extra index.
    const auto found = std::find_if(instructions.begin(),
                                    instructions.end(),
                                    [value_id](auto* inst) { return inst->Id() == value_id; });
    return std::distance(instructions.begin(), found);
}

u32 SpillReloadDemand(const Inst& inst, AllocationWriter& reg_alloc, u32 excluded_value) {
    StackVector<u32, 8> counted{};
    auto count = [&](Value value) {
        if (!value.Defined() || reg_alloc.ValueType(value) != AllocationWriter::MEM) {
            return;
        }
        const u32 value_id = reg_alloc.AllocationId(value);
        auto source = ResolveBitCastSource(value);
        if (!source.Defined() || IsFloatValueType(source.Type()) || value_id == excluded_value ||
            reg_alloc.HasSpillReload(value_id, inst.Id()) ||
            std::find(counted.begin(), counted.end(), value_id) != counted.end()) {
            return;
        }
        counted.push_back(value_id);
    };
    inst.ForEachValue(count);
    if (inst.HasValue()) {
        count(Value{&inst});
    }
    return static_cast<u32>(counted.size());
}

bool HasHeadroom(const Inst& inst,
                 AllocationWriter& reg_alloc,
                 u32 excluded_value,
                 const AllocationInputs& inputs) {
    const auto need = inputs.ScratchFor(inst.Id(), reg_alloc.GetGprs());
    const u32 reloads = SpillReloadDemand(inst, reg_alloc, excluded_value);
    u32 required = need.gpr + reloads;
    const u32 scratch_only = need.spill_outside_value_pool;
    if (need.overlap_gpr_reloads && scratch_only) {
        required = std::max<u32>(need.gpr, need.reload_headroom + reloads);
    }
    const u32 available = reg_alloc.DirtyGPR(inst.Id()).GetClearCount() + scratch_only;
    return available > required;
}

std::optional<u16> FindRegionRegister(std::span<const Inst* const> instructions,
                                      size_t first,
                                      size_t last,
                                      u32 value_id,
                                      AllocationWriter* reg_alloc,
                                      const AllocationInputs& inputs,
                                      u32 forbidden = 0) {
    for (u16 reg = 0; reg < 31; ++reg) {
        if ((forbidden & (1u << reg)) || reg_alloc->GetGprs().Get(reg)) {
            continue;
        }
        bool available = true;
        for (size_t i = first; i <= last; ++i) {
            auto* inst = instructions[i];
            if (reg_alloc->DirtyGPR(inst->Id()).Get(reg) ||
                !HasHeadroom(*inst, *reg_alloc, value_id, inputs)) {
                available = false;
                break;
            }
        }
        if (available) {
            return reg;
        }
    }
    return std::nullopt;
}

bool TryLoadOwnershipPair(std::span<const Inst* const> instructions,
                          std::span<const u32> terminal_uses,
                          u32 input_id,
                          const Vector<size_t>& input_positions,
                          size_t definition_pos,
                          const SpillValueUses& input,
                          SpillValues& uses,
                          AllocationWriter* reg_alloc,
                          const SpillInputs& data,
                          const SpillInputs::BlockRange& range,
                          const AllocationInputs& inputs) {
    if (input_positions.size() != 1) {
        return false;
    }
    const size_t consumer_pos = input_positions.front();
    auto* consumer = instructions[consumer_pos];
    if (definition_pos == instructions.size()) return false;
    const auto* definition = instructions[definition_pos];
    if (!(data.FlagsFor(range, definition->Id()) & SpillInputs::OwnershipInput) ||
        definition->Id() + 1 != consumer->Id() ||
        !(data.FlagsFor(range, consumer->Id()) & SpillInputs::OwnershipResult) ||
        input.occurrences != 1 || definition->GetUses(false) != 1 ||
        std::binary_search(terminal_uses.begin(), terminal_uses.end(), input_id)) {
        return false;
    }

    const auto result = Value{consumer};
    if (reg_alloc->ValueType(result) != AllocationWriter::MEM || IsFloatValueType(result.Type())) {
        return false;
    }
    const u32 result_id = reg_alloc->AllocationId(result);
    const auto result_uses = uses.find(result_id);
    if (result_uses == uses.end() || result_uses->second.positions.empty() ||
        !result_uses->second.has_occurrences || !result_uses->second.transferable ||
        consumer->GetUses(false) != result_uses->second.occurrences ||
        std::binary_search(terminal_uses.begin(), terminal_uses.end(), result_id) ||
        result_uses->second.prepared) {
        return false;
    }
    const auto& result_positions = result_uses->second.positions;

    const auto result_reg = FindRegionRegister(
            instructions, consumer_pos, result_positions.back(), result_id, reg_alloc, inputs);
    if (!result_reg) {
        return false;
    }
    const auto input_reg = FindRegionRegister(instructions,
                                              definition_pos,
                                              consumer_pos,
                                              input_id,
                                              reg_alloc,
                                              inputs,
                                              1u << *result_reg);
    if (!input_reg) {
        return false;
    }

    reg_alloc->MapSpillReload(
            input_id, definition->Id(), consumer->Id(), HostGPR{*input_reg}, true, true);
    reg_alloc->MapSpillReload(result_id,
                              consumer->Id(),
                              instructions[result_positions.back()]->Id(),
                              HostGPR{*result_reg},
                              true);
    result_uses->second.prepared = true;
    return true;
}

void BuildSegment(std::span<const Inst* const> instructions,
                  bool dense_ids,
                  std::span<const u32> terminal_uses,
                  AllocationWriter* reg_alloc,
                  const SpillInputs& data,
                  const SpillInputs::BlockRange& range,
                  const AllocationInputs& inputs) {
    SpillValues uses;
    for (size_t i = 0; i < instructions.size(); ++i) {
        const auto* inst = instructions[i];
        if (inst->IsPseudoOperation()) continue;
        u8 flags{};
        bool flags_ready{};
        inst->ForEachValue([&](Value value) {
            if (!value.Defined() || reg_alloc->ValueType(value) != AllocationWriter::MEM) return;
            const auto source = ResolveBitCastSource(value);
            if (source.Defined() && !IsFloatValueType(source.Type())) {
                auto& positions = uses[reg_alloc->AllocationId(source)].positions;
                if (positions.empty() || positions.back() != i) positions.push_back(i);
            }
            if (!IsFloatValueType(source.Type())) {
                if (!flags_ready) {
                    flags = data.FlagsFor(range, inst->Id());
                    flags_ready = true;
                }
                auto& use = uses[reg_alloc->AllocationId(value)];
                use.has_occurrences = true;
                ++use.occurrences;
                use.transferable &= (flags & SpillInputs::DefinitionConsumer) != 0;
                use.handoff &= (flags & SpillInputs::PendingHandoff) != 0;
            }
        });
    }

    for (auto& [value_id, use] : uses) {
        if (use.positions.empty() || use.prepared) continue;
        const auto& positions = use.positions;
        const auto definition_pos = SegmentDefinition(instructions, value_id, dense_ids);
        const auto* definition =
                definition_pos == instructions.size() ? nullptr : instructions[definition_pos];
        if (TryLoadOwnershipPair(instructions,
                                 terminal_uses,
                                 value_id,
                                 positions,
                                 definition_pos,
                                 use,
                                 uses,
                                 reg_alloc,
                                 data,
                                 range,
                                 inputs)) {
            continue;
        }
        if (definition &&
            (data.FlagsFor(range, definition->Id()) & SpillInputs::DefinitionTransfer) &&
            use.has_occurrences && use.transferable &&
            definition->GetUses(false) == use.occurrences &&
            !std::binary_search(terminal_uses.begin(), terminal_uses.end(), value_id)) {
            if (definition_pos < positions.front()) {
                auto reg = FindRegionRegister(instructions,
                                              definition_pos,
                                              positions.back(),
                                              value_id,
                                              reg_alloc,
                                              inputs);
                if (reg) {
                    reg_alloc->MapSpillReload(value_id,
                                              definition->Id(),
                                              instructions[positions.back()]->Id(),
                                              HostGPR{*reg},
                                              true);
                    continue;
                }
            }
        }
        size_t begin = 0;
        while (begin + 1 < positions.size()) {
            size_t best = begin;
            std::optional<u16> best_reg;
            for (size_t end = begin + 1; end < positions.size(); ++end) {
                auto reg = FindRegionRegister(instructions,
                                              positions[begin],
                                              positions[end],
                                              value_id,
                                              reg_alloc,
                                              inputs);
                if (!reg) {
                    break;
                }
                best = end;
                best_reg = reg;
            }
            if (!best_reg) {
                ++begin;
                continue;
            }
            const bool owns_all_uses =
                    begin == 0 && best + 1 == positions.size() && definition &&
                    use.has_occurrences && use.handoff &&
                    definition->GetUses(false) == use.occurrences &&
                    !std::binary_search(terminal_uses.begin(), terminal_uses.end(), value_id);
            reg_alloc->MapSpillReload(value_id,
                                      instructions[positions[begin]]->Id(),
                                      instructions[positions[best]]->Id(),
                                      HostGPR{*best_reg},
                                      owns_all_uses);
            begin = best + 1;
        }
    }
}

}  // namespace

void LinearScan::BuildSpillReloads() {
    if (!inputs.spill) return;
    const auto prepare = [&](const Block* member) {
        const auto* range = inputs.spill->BlockFor(member);
        if (!range) return;
        const auto* graph = facts.FindIndex(member);
        ASSERT(graph);
        const auto nodes = graph->Instructions();
        // MapSpillReload changes reload metadata and dirty masks, never the
        // allocation aliases used by this block-local terminal projection.
        const auto terminal_uses = TerminalAllocationUses(member->GetTerminal(), *reg_alloc);
        for (const auto& segment : inputs.spill->Segments(*range))
            BuildSegment(nodes.subspan(segment.first, segment.count),
                         graph->DenseInstructionIDs(),
                         terminal_uses,
                         reg_alloc,
                         *inputs.spill,
                         *range,
                         inputs);
    };
    if (function)
        for (const auto& member : function->GetHIRBlocksRPO()) prepare(member.GetBlock());
    else
        prepare(block);
}
}  // namespace swift::runtime::compile::detail
