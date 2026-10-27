#include "actions.h"

#include <algorithm>

#include "runtime/compile/allocate/allocation.h"
#include "runtime/ir/block.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::arm64 {

static_assert(sizeof(ConsumerAction) == 8);
static_assert(sizeof(IntegerFlagsAction) == 8);
static_assert(sizeof(PublicationAction) == 8);
static_assert(sizeof(MemoryTransferAction) == 8);
static_assert(sizeof(AddressAction) == 16);
static_assert(sizeof(FlagsOnlySpillAction) == 4);
static_assert(sizeof(compile::PseudoFlagsView) == 16);
static_assert(sizeof(GPRResidenceAction) == 4);
static_assert(sizeof(IntegerBranchAction) == 12);
static_assert(sizeof(AluSplitAction) == 16);
static_assert(sizeof(BlockActions) == 12);

bool UnitActions::MatchesAllocation(compile::AllocationView current) const {
    return ready && allocation.RefersToSameResult(current) && current.GPRLocationsFrozen() &&
           generation == current.GPRLocationGeneration() && attempt == current.AllocationAttempt();
}

bool UnitActions::MatchesPreparation(compile::AllocationView current) const {
    return building && allocation.RefersToSameResult(current) && !current.GPRLocationsFrozen() &&
           generation == current.GPRLocationGeneration() && attempt == current.AllocationAttempt();
}

const BlockActions* UnitActions::ForBlock(const ir::Block* block) const {
    if (!building && !Ready()) return nullptr;
    const auto found = blocks.find(block);
    return found == blocks.end() ? nullptr : &found->second;
}

const BackedgeAction* UnitActions::BackedgeFor(const ir::Block* block) const {
    const auto* row = ForBlock(block);
    return row && row->backedge != UINT16_MAX ? &backedges[row->backedge] : nullptr;
}

const PFAFAction* UnitActions::PFAFFor(const ir::Block* block) const {
    const auto* row = BackedgeFor(block);
    return row && row->pfaf != UINT16_MAX ? &pfaf[row->pfaf] : nullptr;
}

const AtomicCohort* UnitActions::AtomicFor(const ir::Inst* inst) const {
    return inst && ForBlock(inst->GetBlock()) ? atomics.For(inst->Id()) : nullptr;
}

bool UnitActions::DefersTerminalLocation(const ir::Inst* inst) const {
    if (!inst || inst->GetOp() != ir::OpCode::SetLocation || !ForBlock(inst->GetBlock()) ||
        !deferred_location_count)
        return false;
    const std::span<const u16> ids{deferred_locations.get(), deferred_location_count};
    return std::binary_search(ids.begin(), ids.end(), inst->Id());
}

bool UnitActions::MatchesTargetInputs(compile::AllocationView current,
                                      const Config& config,
                                      const FeatureSet& features) const {
    return MatchesAllocation(current) && certificate &&
           certificate->MatchesTargetInputs(current, config, features);
}

compile::PseudoFlagsView UnitActions::PseudoFlagsForBlock(const ir::Block* block) const {
    if (!building && !Ready()) return {};
    const auto found = blocks.find(block);
    if (found == blocks.end() || found->second.flags_offset == UINT32_MAX) return {};
    const auto& row = found->second;
    return {pseudo_flags.data() + row.flags_offset,
            u32{row.instruction_last} - row.instruction_first + 1,
            row.instruction_first};
}

bool UnitActions::HasPreparedScratch(const ir::Block* block) const {
    if (!building && !Ready()) return false;
    const auto found = blocks.find(block);
    return unit_scratch.prepared && found != blocks.end() && found->second.has_scratch;
}

bool UnitActions::MatchesScratchInputs(const ir::Block* block,
                                       const GPRSMask& pool,
                                       const FeatureSet& features) const {
    if (!Ready() || !HasPreparedScratch(block) || pool != allocation.GetGprs()) return false;
    return certificate && certificate->MatchesScratchInputs(pool, features);
}

InstructionScratchView UnitActions::ScratchForBlock(const ir::Block* block) const {
    if (!Ready() || !HasPreparedScratch(block)) return {};
    const auto& row = blocks.at(block);
    if (row.instruction_first > row.instruction_last) return {{}, block};
    const auto first =
            std::lower_bound(scratch_ranges.begin(),
                             scratch_ranges.end(),
                             row.instruction_first,
                             [](const ScratchRange& action, u32 id) { return action.last < id; });
    const auto last =
            std::upper_bound(first,
                             scratch_ranges.end(),
                             row.instruction_last,
                             [](u32 id, const ScratchRange& action) { return id < action.first; });
    return {std::span<const ScratchRange>{first, last}, block};
}

const InstructionScratchFacts* UnitActions::ScratchFor(const ir::Inst* inst) const {
    return inst ? ScratchForBlock(inst->GetBlock()).For(inst) : nullptr;
}

SpillInputFacts UnitActions::SpillInputFor(const ir::Inst* consumer, u32 producer) const {
    if (!consumer || producer > UINT16_MAX || !Ready() || !HasPreparedScratch(consumer->GetBlock()))
        return {};
    const u32 key = (u32{consumer->Id()} << 16) | producer;
    const auto found = std::lower_bound(
            spill_inputs.begin(), spill_inputs.end(), key, [](const SpillInputAction& row, u32 id) {
                return row.Key() < id;
            });
    return found != spill_inputs.end() && found->Key() == key
                   ? SpillInputFacts{found->definition, found->last_use}
                   : SpillInputFacts{};
}

const GPRResidenceAction* UnitActions::Residence(const ir::Inst* inst) const {
    if (!inst || !Ready() || !ForBlock(inst->GetBlock())) return nullptr;
    const auto found = std::lower_bound(
            residences.begin(),
            residences.end(),
            inst->Id(),
            [](const GPRResidenceAction& row, u32 id) { return row.instruction < id; });
    return found != residences.end() && found->instruction == inst->Id() ? &*found : nullptr;
}

bool UnitActions::DiscardsFlagsOnlySpill(const ir::Inst* consumer, u32 producer) const {
    if (!consumer || producer > UINT16_MAX || !ForBlock(consumer->GetBlock())) return false;
    const u32 key = (u32{consumer->Id()} << 16) | producer;
    const auto found = building ? std::find_if(flags_only_spills.begin(),
                                               flags_only_spills.end(),
                                               [key](const auto& row) { return row.Key() == key; })
                                : std::lower_bound(flags_only_spills.begin(),
                                                   flags_only_spills.end(),
                                                   key,
                                                   [](const auto& row, u32 value) {
                                                       return row.Key() < value;
                                                   });
    return found != flags_only_spills.end() && found->Key() == key;
}

void UnitActions::Certify(u32 id, Proof proof, u32 capacity) {
    ASSERT(id < capacity);
    if (proofs.empty()) proofs.resize(capacity);
    proofs[id] |= proof;
}

bool UnitActions::FPRVerified(u32 id, FPRAction action) const {
    return HasProof(id, static_cast<Proof>(static_cast<u16>(action) << 4));
}

std::optional<u8> UnitActions::PshufdControl(u32 id) const {
    if (!FPRVerified(id, FPRAction::Pshufd)) return std::nullopt;
    // Preparation may visit function blocks in RPO rather than numeric ID
    // order. The frozen result has one sorted sparse control table.
    const auto found =
            building ? std::find_if(shuffle_controls.begin(),
                                    shuffle_controls.end(),
                                    [id](const auto& row) { return row.instruction == id; })
                     : std::lower_bound(
                               shuffle_controls.begin(),
                               shuffle_controls.end(),
                               id,
                               [](const auto& row, u32 value) { return row.instruction < value; });
    return found != shuffle_controls.end() && found->instruction == id
                   ? std::optional<u8>{found->control}
                   : std::nullopt;
}

void UnitActions::CertifyFPR(u32 id, FPRAction action, u8 control, u32 capacity) {
    Certify(id, static_cast<Proof>(static_cast<u16>(action) << 4), capacity);
    if (static_cast<u8>(action) & static_cast<u8>(FPRAction::Pshufd)) {
        ASSERT(id <= UINT16_MAX);
        shuffle_controls.push_back({static_cast<u16>(id), control});
    }
}

void UnitActions::SealPayloads() {
    const auto scratch_before = [](const auto& a, const auto& b) { return a.first < b.first; };
    // Numbered RPO input is already ordered. Keep normal sealing linear;
    // manual/nonmonotonic blocks still get the same sorted lookup contract.
    if (!std::is_sorted(scratch_ranges.begin(), scratch_ranges.end(), scratch_before))
        std::sort(scratch_ranges.begin(), scratch_ranges.end(), scratch_before);
    ASSERT(std::adjacent_find(
                   scratch_ranges.begin(), scratch_ranges.end(), [](const auto& a, const auto& b) {
                       return a.last >= b.first;
                   }) == scratch_ranges.end());
    std::sort(spill_inputs.begin(), spill_inputs.end(), [](const auto& a, const auto& b) {
        return a.Key() < b.Key();
    });
    size_t input_count = 0;
    for (size_t i = 0; i < spill_inputs.size();) {
        auto row = spill_inputs[i++];
        bool valid = row.definition != nullptr;
        while (i < spill_inputs.size() && spill_inputs[i].Key() == row.Key()) {
            valid &= spill_inputs[i].definition == row.definition &&
                     spill_inputs[i].last_use == row.last_use;
            ++i;
        }
        if (valid) spill_inputs[input_count++] = row;
    }
    spill_inputs.resize(input_count);
    std::sort(residences.begin(), residences.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    size_t residence_count = 0;
    for (size_t i = 0; i < residences.size();) {
        auto row = residences[i++];
        while (i < residences.size() && residences[i].instruction == row.instruction) {
            const auto& next = residences[i++];
            row.bits |= next.bits;
            ASSERT(row.result_home == UINT8_MAX || next.result_home == UINT8_MAX ||
                   row.result_home == next.result_home);
            if (next.result_home != UINT8_MAX) row.result_home = next.result_home;
        }
        residences[residence_count++] = row;
    }
    residences.resize(residence_count);
    std::sort(flags_only_spills.begin(), flags_only_spills.end(), [](const auto& a, const auto& b) {
        return a.Key() < b.Key();
    });
    flags_only_spills.erase(std::unique(flags_only_spills.begin(), flags_only_spills.end()),
                            flags_only_spills.end());
    std::sort(shuffle_controls.begin(), shuffle_controls.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(shuffle_controls.begin(),
                              shuffle_controls.end(),
                              [](const auto& a, const auto& b) {
                                  return a.instruction == b.instruction;
                              }) == shuffle_controls.end());
    std::sort(scalar_fpr.begin(), scalar_fpr.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(
                   scalar_fpr.begin(), scalar_fpr.end(), [](const auto& a, const auto& b) {
                       return a.instruction == b.instruction;
                   }) == scalar_fpr.end());
    std::sort(selects.begin(), selects.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(selects.begin(), selects.end(), [](const auto& a, const auto& b) {
               return a.instruction == b.instruction;
           }) == selects.end());
    std::sort(flags_tokens.begin(), flags_tokens.end(), [](const auto& a, const auto& b) {
        return std::pair{a.producer, a.point} < std::pair{b.producer, b.point};
    });
    ASSERT(std::adjacent_find(
                   flags_tokens.begin(), flags_tokens.end(), [](const auto& a, const auto& b) {
                       return a.producer == b.producer && a.point == b.point;
                   }) == flags_tokens.end());
    std::sort(narrow.begin(), narrow.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(narrow.begin(), narrow.end(), [](const auto& a, const auto& b) {
               return a.instruction == b.instruction;
           }) == narrow.end());
    std::sort(carry.begin(), carry.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(carry.begin(), carry.end(), [](const auto& a, const auto& b) {
               return a.instruction == b.instruction;
           }) == carry.end());
    std::sort(memory.begin(), memory.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(memory.begin(), memory.end(), [](const auto& a, const auto& b) {
               return a.instruction == b.instruction;
           }) == memory.end());
    std::sort(consumers.begin(), consumers.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(consumers.begin(), consumers.end(), [](const auto& a, const auto& b) {
               return a.instruction == b.instruction;
           }) == consumers.end());
    std::sort(publications.begin(), publications.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(
                   publications.begin(), publications.end(), [](const auto& a, const auto& b) {
                       return a.instruction == b.instruction;
                   }) == publications.end());
    std::sort(memory_transfers.begin(), memory_transfers.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(memory_transfers.begin(),
                              memory_transfers.end(),
                              [](const auto& a, const auto& b) {
                                  return a.instruction == b.instruction;
                              }) == memory_transfers.end());
    std::sort(addresses.begin(), addresses.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(addresses.begin(), addresses.end(), [](const auto& a, const auto& b) {
               return a.instruction == b.instruction;
           }) == addresses.end());
    std::sort(integer_flags.begin(), integer_flags.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(
                   integer_flags.begin(), integer_flags.end(), [](const auto& a, const auto& b) {
                       return a.instruction == b.instruction;
                   }) == integer_flags.end());
    std::sort(integer_branches.begin(), integer_branches.end(), [](const auto& a, const auto& b) {
        return a.condition < b.condition;
    });
    ASSERT(std::adjacent_find(integer_branches.begin(),
                              integer_branches.end(),
                              [](const auto& a, const auto& b) {
                                  return a.condition == b.condition;
                              }) == integer_branches.end());
    std::sort(alu_split.begin(), alu_split.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    ASSERT(std::adjacent_find(alu_split.begin(), alu_split.end(), [](const auto& a, const auto& b) {
               return a.instruction == b.instruction;
           }) == alu_split.end());
}

const ScalarFPRAction* UnitActions::FindScalarFPR(u32 id) const {
    if (!building && !Ready()) return nullptr;
    const auto found =
            building ? std::find_if(scalar_fpr.begin(),
                                    scalar_fpr.end(),
                                    [id](const auto& row) { return row.instruction == id; })
                     : std::lower_bound(
                               scalar_fpr.begin(),
                               scalar_fpr.end(),
                               id,
                               [](const auto& row, u32 value) { return row.instruction < value; });
    return found != scalar_fpr.end() && found->instruction == id ? &*found : nullptr;
}

const ScalarFPRAction* UnitActions::ScalarFPRLoad(u32 id) const {
    const auto* action = FindScalarFPR(id);
    return action && action->IsLoad() ? action : nullptr;
}

const ScalarFPRAction* UnitActions::ScalarFPRValue(u32 id) const {
    const auto* action = FindScalarFPR(id);
    return action && action->kind == ScalarFPRAction::Value ? action : nullptr;
}

const ScalarFPRAction* UnitActions::ScalarFPRConversion(u32 id) const {
    const auto* action = FindScalarFPR(id);
    return action && action->IsConversion() ? action : nullptr;
}

const ScalarFPRAction* UnitActions::ScalarFPRStore(u32 id) const {
    const auto* action = FindScalarFPR(id);
    return action && action->kind == ScalarFPRAction::Store ? action : nullptr;
}

const CarryAction* UnitActions::CarryForTest(u32 id) const {
    if (!building && !Ready()) return nullptr;
    const auto found =
            building ? std::find_if(carry.begin(),
                                    carry.end(),
                                    [id](const auto& row) { return row.instruction == id; })
                     : std::lower_bound(
                               carry.begin(), carry.end(), id, [](const auto& row, u32 value) {
                                   return row.instruction < value;
                               });
    return found != carry.end() && found->instruction == id ? &*found : nullptr;
}

const MemoryAction* UnitActions::Memory(u32 id) const {
    if (!building && !Ready()) return nullptr;
    const auto found =
            building ? std::find_if(memory.begin(),
                                    memory.end(),
                                    [id](const auto& row) { return row.instruction == id; })
                     : std::lower_bound(
                               memory.begin(), memory.end(), id, [](const auto& row, u32 value) {
                                   return row.instruction < value;
                               });
    return found != memory.end() && found->instruction == id ? &*found : nullptr;
}

ir::Value MemoryAction::OffsetBase(const ir::Inst& access) const {
    ASSERT(access.Id() == instruction && Has(Offset));
    const auto operand = access.GetArg<ir::Operand>(0);
    ASSERT(operand.GetLeft().IsValue() && operand.GetRight().Null());
    return operand.GetLeft().value.Def()->GetArg<ir::Value>(0);
}

const ConsumerAction* UnitActions::Consumer(u32 id) const {
    if (!building && !Ready()) return nullptr;
    const auto found =
            building
                    ? std::find_if(consumers.begin(),
                                   consumers.end(),
                                   [id](const auto& row) { return row.instruction == id; })
                    : std::lower_bound(
                              consumers.begin(), consumers.end(), id, [](const auto& row, u32 key) {
                                  return row.instruction < key;
                              });
    return found != consumers.end() && found->instruction == id ? &*found : nullptr;
}

const AluSplitFacts* UnitActions::AluSplit(u32 id) const {
    if (!building && !Ready()) return nullptr;
    const auto found =
            building
                    ? std::find_if(alu_split.begin(),
                                   alu_split.end(),
                                   [id](const auto& row) { return row.instruction == id; })
                    : std::lower_bound(
                              alu_split.begin(), alu_split.end(), id, [](const auto& row, u32 key) {
                                  return row.instruction < key;
                              });
    return found != alu_split.end() && found->instruction == id ? &found->facts : nullptr;
}

bool ConsumerAction::MatchesZeroInput(const ir::Inst& consumer, ir::Value value) const {
    return consumer.Id() == instruction && Has(ConsumerFlag::IncomingZero) &&
           consumer.GetArg<ir::Value>(auxiliary).Def() == value.Def();
}

const SelectAction* UnitActions::Select(u32 id) const {
    if (!building && !Ready()) return nullptr;
    const auto found =
            building ? std::find_if(selects.begin(),
                                    selects.end(),
                                    [id](const auto& row) { return row.instruction == id; })
                     : std::lower_bound(
                               selects.begin(), selects.end(), id, [](const auto& row, u32 value) {
                                   return row.instruction < value;
                               });
    return found != selects.end() && found->instruction == id ? &*found : nullptr;
}

bool FlagsTokenAction::Retains(std::span<const FlagsTokenAction> actions,
                               u32 producer,
                               u32 home,
                               u32 point) {
    if (producer >= UINT16_MAX || home >= 31) return false;
    const auto find = [&](u32 capture) {
        const auto key = std::pair{producer, capture};
        const auto found = std::lower_bound(
                actions.begin(), actions.end(), key, [](const auto& row, const auto& value) {
                    return std::pair<u32, u32>{row.producer, row.point} < value;
                });
        return found != actions.end() && found->producer == producer && found->point == capture &&
               (found->homes & (1u << home));
    };
    return (point < UINT16_MAX && find(point)) || find(UINT16_MAX);
}

bool UnitActions::RetainsFlagsToken(u32 producer, u32 home, u32 point) const {
    if ((!building && !Ready()) || producer >= UINT16_MAX || home >= 31) return false;
    // Function preparation appends blocks in RPO. Only sealing gives the
    // single sparse table its global numeric ordering.
    if (building)
        return std::ranges::any_of(flags_tokens, [&](const auto& row) {
            return row.producer == producer && (row.point == UINT16_MAX || row.point == point) &&
                   (row.homes & (1u << home));
        });
    return FlagsTokenAction::Retains(flags_tokens, producer, home, point);
}

const NarrowAction* UnitActions::Narrow(u32 id) const {
    if (!building && !Ready()) return nullptr;
    const auto found =
            building ? std::find_if(narrow.begin(),
                                    narrow.end(),
                                    [id](const auto& row) { return row.instruction == id; })
                     : std::lower_bound(
                               narrow.begin(), narrow.end(), id, [](const auto& row, u32 value) {
                                   return row.instruction < value;
                               });
    return found != narrow.end() && found->instruction == id ? &*found : nullptr;
}

ir::Value NarrowAction::Source(const ir::Inst& consumer) const {
    ASSERT(consumer.Id() == instruction);
    auto source = ir::Value{&consumer};
    const auto depth = kind == ExtractShift ? 3u : 2u;
    for (u32 edge = 0; edge < depth; ++edge) {
        ASSERT(source.Defined());
        source = source.Def()->GetArg<ir::Value>(0);
    }
    ASSERT(source.Defined());
    return source;
}

}  // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {
bool IntegerFlagsAction::MatchesInput(const ir::Inst& consumer, ir::Value value) const {
    return IsInput() && argument == consumer.Id() && value.Defined() && value.Id() == instruction &&
           value.Def()->GetBlock() == consumer.GetBlock() &&
           consumer.GetArg<ir::Value>(0).Def() == value.Def();
}
ir::Value IntegerFlagsAction::Source(const ir::Inst& consumer) const {
    if (IsInput()) {
        const auto extract = consumer.GetArg<ir::Value>(0);
        ASSERT(MatchesInput(consumer, extract));
        return kind == InputLow ? extract : extract.Def()->GetArg<ir::Value>(0);
    }
    ASSERT(instruction == consumer.Id());
    if (kind == CarryRight) return consumer.GetArg<ir::Operand>(1).GetLeft().value;
    const auto first = consumer.GetArg<ir::Value>(0);
    return IsCompare() && (options & 1) ? first.Def()->GetArg<ir::Value>(0) : first;
}
std::optional<ir::Value> IntegerFlagsAction::Right(const ir::Inst& consumer) const {
    ASSERT(IsCompare() && instruction == consumer.Id());
    return kind == CompareValue ? std::optional{consumer.GetArg<ir::Operand>(1).GetLeft().value}
                                : std::nullopt;
}
const IntegerFlagsAction* UnitActions::IntegerFlags(u32 id) const {
    if (!building && !Ready()) return nullptr;
    const auto it = building ? std::find_if(integer_flags.begin(),
                                            integer_flags.end(),
                                            [id](const auto& row) { return row.instruction == id; })
                             : std::lower_bound(integer_flags.begin(),
                                                integer_flags.end(),
                                                id,
                                                [](const auto& row, u32 key) {
                                                    return row.instruction < key;
                                                });
    return it != integer_flags.end() && it->instruction == id ? &*it : nullptr;
}
const IntegerFlagsAction* UnitActions::FlagsInput(u32 id) const {
    const auto* row = IntegerFlags(id);
    return row && row->IsInput() ? row : nullptr;
}
const IntegerFlagsAction* UnitActions::NarrowCarry(u32 id) const {
    const auto* row = IntegerFlags(id);
    return row && row->IsCarry() ? row : nullptr;
}
const IntegerFlagsAction* UnitActions::NarrowCompare(u32 id) const {
    const auto* row = IntegerFlags(id);
    return row && row->IsCompare() ? row : nullptr;
}
const IntegerBranchAction* UnitActions::IntegerBranch(const ir::Block* block) const {
    if (!ForBlock(block)) return nullptr;
    const auto& terminal = block->GetTerminal();
    const auto* branch = boost::get<ir::terminal::If>(&terminal);
    if (!branch || !branch->cond.Defined()) return nullptr;
    const auto id = branch->cond.Id();
    const auto it = building ? std::find_if(integer_branches.begin(),
                                            integer_branches.end(),
                                            [id](const auto& row) { return row.condition == id; })
                             : std::lower_bound(integer_branches.begin(),
                                                integer_branches.end(),
                                                id,
                                                [](const auto& row, u32 key) {
                                                    return row.condition < key;
                                                });
    return it != integer_branches.end() && it->condition == id ? &*it : nullptr;
}
}  // namespace swift::runtime::backend::arm64

namespace swift::runtime::backend::arm64 {
ir::Value PublicationAction::Source(const ir::Inst& publication) const {
    ASSERT(kind == Value && publication.Id() == instruction);
    const auto value = publication.GetArg<ir::Value>(0);
    return Has(FusedSource) ? value.Def()->GetArg<ir::Value>(0) : value;
}
bool MemoryTransferAction::MatchesMemory(const ir::Inst& access) const {
    if ((kind != PreIndex && kind != Biased) || related != access.Id()) return false;
    const auto operand = access.GetArg<ir::Operand>(0);
    if (!operand.GetLeft().IsValue()) return false;
    const auto source = operand.GetLeft().value;
    return source.Defined() && source.Id() == instruction &&
           source.Def()->GetBlock() == access.GetBlock();
}
bool MemoryTransferAction::MatchesValue(const ir::Inst& store, ir::Value value) const {
    return kind == Value && related == store.Id() && value.Defined() && value.Id() == instruction &&
           value.Def()->GetBlock() == store.GetBlock() &&
           store.GetOp() == ir::OpCode::StoreMemory &&
           store.GetArg<ir::Value>(1).Def() == value.Def();
}
const PublicationAction* UnitActions::Publication(u32 id) const {
    if (!building && !Ready()) return nullptr;
    const auto it = building ? std::find_if(publications.begin(),
                                            publications.end(),
                                            [id](const auto& row) { return row.instruction == id; })
                             : std::lower_bound(publications.begin(),
                                                publications.end(),
                                                id,
                                                [](const auto& row, u32 key) {
                                                    return row.instruction < key;
                                                });
    return it != publications.end() && it->instruction == id ? &*it : nullptr;
}
const MemoryTransferAction* UnitActions::MemoryTransfer(u32 id) const {
    if (!building && !Ready()) return nullptr;
    const auto it = building ? std::find_if(memory_transfers.begin(),
                                            memory_transfers.end(),
                                            [id](const auto& row) { return row.instruction == id; })
                             : std::lower_bound(memory_transfers.begin(),
                                                memory_transfers.end(),
                                                id,
                                                [](const auto& row, u32 key) {
                                                    return row.instruction < key;
                                                });
    return it != memory_transfers.end() && it->instruction == id ? &*it : nullptr;
}
const AddressAction* UnitActions::Address(u32 id) const {
    if (!building && !Ready()) return nullptr;
    const auto it =
            building
                    ? std::find_if(addresses.begin(),
                                   addresses.end(),
                                   [id](const auto& row) { return row.instruction == id; })
                    : std::lower_bound(
                              addresses.begin(), addresses.end(), id, [](const auto& row, u32 key) {
                                  return row.instruction < key;
                              });
    return it != addresses.end() && it->instruction == id ? &*it : nullptr;
}
}  // namespace swift::runtime::backend::arm64
