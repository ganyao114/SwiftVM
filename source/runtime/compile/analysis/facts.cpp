#include "semantic_facts.h"
#include "ssa.h"
#include "uses.h"

#include <algorithm>
#include <type_traits>

#include "runtime/common/perf_stats.h"
#include "runtime/ir/block.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {

Value ResolveBitCastSource(Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<Value>(0);
    }
    return value;
}

}  // namespace swift::runtime::ir

namespace swift::runtime::compile {
namespace {

template <typename Visitor>
void VisitUses(const ir::Block& block, bool physical_reads, Visitor&& record) {
    u32 end = 0;
    const auto& list = block.GetInstList();
    for (const auto& inst : list) {
        end = std::max<u32>(end, inst.Id());
        if (physical_reads && inst.IsBitCastOperation()) continue;
        inst.ForEachValue([&](ir::Value value) { record(value, inst.Id()); });
    }
    if (!physical_reads && list.empty()) return;
    if (!physical_reads) end = list.back().Id();
    VisitImplicitUses(block, end, record);
}

template <typename Visitor>
void VisitBlocks(const ir::Block* block, const ir::HIRFunction* function, Visitor&& visit) {
    if (function) {
        for (auto& item : function->GetHIRBlocksRPO()) visit(*item.GetBlock());
    } else {
        visit(*block);
    }
}

}  // namespace

BlockUseEnds::BlockUseEnds(const ir::Block* block, u32 count, const BlockAnalysisIndex* consumers)
        : instruction_count(count), first(count) {
    u32 last = 0;
    for (const auto& inst : block->GetInstList()) {
        if (inst.Id() < count) {
            first = std::min<u32>(first, inst.Id());
            last = std::max<u32>(last, inst.Id());
        }
    }
    if (first < count) local.resize(last - first + 1);
    if (consumers) {
        consumers->ForEachUsedDefinition([&](const ir::Inst* definition) {
            for (const auto& use : consumers->Uses(definition))
                Record(ir::Value{definition}, use.consumer->Id());
        });
        if (!block->GetInstList().empty())
            VisitImplicitUses(*block,
                              block->GetInstList().back().Id(),
                              [this](ir::Value value, u32 position) { Record(value, position); });
    } else {
        VisitUses(
                *block, false, [this](ir::Value value, u32 position) { Record(value, position); });
    }
    std::sort(external.begin(), external.end());
    size_t output = 0;
    for (const auto& entry : external) {
        if (output && external[output - 1].first == entry.first) {
            external[output - 1].second = std::max(external[output - 1].second, entry.second);
        } else {
            external[output++] = entry;
        }
    }
    external.resize(output);
}

void BlockUseEnds::Record(ir::Value value, u32 position) {
    const auto root = ir::ResolveBitCastSource(value);
    if (!root.Defined() || root.Id() >= instruction_count) return;
    const u32 id = root.Id();
    if (id >= first && id - first < local.size()) {
        local[id - first] = std::max(local[id - first], position);
    } else {
        external.emplace_back(id, position);
    }
}

u32 BlockUseEnds::operator[](u32 id) const {
    if (id >= first && id - first < local.size()) return local[id - first];
    const auto found = std::lower_bound(external.begin(), external.end(), id,
            [](const auto& item, u32 key) { return item.first < key; });
    return found != external.end() && found->first == id ? found->second : 0;
}

SemanticFacts::SemanticFacts(const ir::Block* input) { BeginInput(input, nullptr); }
SemanticFacts::SemanticFacts(const ir::HIRFunction* input) { BeginInput(nullptr, input); }

bool SemanticFacts::InputSealed() const {
    return block ? block->IsIRReadOnly()
                 : function && function->GetEntryBlock() &&
                           function->GetEntryBlock()->GetBlock()->IsIRReadOnly();
}

void SemanticFacts::BeginInput(const ir::Block* input_block,
                               const ir::HIRFunction* input_function) {
    ASSERT((input_block != nullptr) != (input_function != nullptr));
    if (InputSealed()) {
        ASSERT(block == input_block && function == input_function);
        return;
    }
    blocks.clear();
    flags.clear();
    function_values.reset();
    spill_uses.clear();
    value_liveness.reset();
    register_reads.reset();
    guest_state.reset();
    function_cfg.reset();
    ssa_valid.reset();
    flags_ready = spill_ready = false;
    gpr_homes = 0;
    block = input_block;
    function = input_function;
    instruction_count = function ? function->MaxInstrCount() : block->MaxInstrId();
}

bool SemanticFacts::Contains(const ir::Block* input) const {
    if (!input) return false;
    if (block) return input == block;
    if (!function) return false;
    for (auto* member : function->GetHIRBlocks())
        if (member && member->GetBlock() == input) return true;
    return false;
}

std::span<const ir::Inst* const> SemanticFacts::PrepareGuestStateObservations(
        const ir::Block* input) {
    ASSERT(Contains(input));
    if (!guest_state) guest_state = std::make_unique<GuestStateFacts>();
    return guest_state->PrepareObservations(input);
}

std::optional<std::span<const ir::Inst* const>> SemanticFacts::FindGuestStateObservations(
        const ir::Block* input) const {
    return guest_state ? guest_state->FindObservations(input) : std::nullopt;
}

std::span<const ir::Inst* const> SemanticFacts::PrepareGuestStateGPRDefinitions(
        const ir::Block* input) {
    ASSERT(Contains(input));
    if (!guest_state) guest_state = std::make_unique<GuestStateFacts>();
    return guest_state->PrepareGPRDefinitions(input);
}

std::optional<std::span<const ir::Inst* const>> SemanticFacts::FindGuestStateGPRDefinitions(
        const ir::Block* input) const {
    return guest_state ? guest_state->FindGPRDefinitions(input) : std::nullopt;
}

const GuestStateRegisterAccesses& SemanticFacts::PrepareGuestStateRegisterAccesses(const ir::Block* input) {
    ASSERT(Contains(input));
    if (!guest_state) guest_state = std::make_unique<GuestStateFacts>();
    return guest_state->PrepareRegisterAccesses(input);
}

const GuestStateRegisterAccesses* SemanticFacts::FindGuestStateRegisterAccesses(const ir::Block* input) const {
    return guest_state ? guest_state->FindRegisterAccesses(input) : nullptr;
}

bool SemanticFacts::CanUseBlockIntervals() const {
    if (!function || function->GetHIRBlocksRPO().size() != 1) return false;
    auto* entry = function->GetEntryBlock();
    auto* current = &function->GetHIRBlocksRPO().front();
    // The synthetic entry must contribute no definitions, and every incoming
    // path must enter this same executable block. A self-edge is permitted.
    if (!entry || !entry->GetBlock()->GetInstList().empty() ||
        !entry->GetBlock()->GetLoopHoistMetadata().anchors.empty()) return false;
    if (entry->GetOutgoingEdges().size() != 1 ||
        entry->GetOutgoingEdges().front().GetTargetBlock() != current)
        return false;
    const bool direct_entry = VisitVariant<bool>(entry->GetBlock()->GetTerminal(),
            [&](const auto& edge) {
                using T = std::decay_t<decltype(edge)>;
                if constexpr (std::is_same_v<T, ir::terminal::LinkBlock>) {
                    return edge.next == current->GetBlock()->GetStartLocation();
                }
                return false;
            });
    if (!direct_entry) return false;
    for (const auto& edge : current->GetIncomingEdges()) {
        if (edge.GetSourceBlock() != current && edge.GetSourceBlock() != entry) return false;
    }
    for (auto* root : function->GetExternalEntryRoots()) {
        if (root != current && root != entry) return false;
    }
    const auto& list = current->GetBlock()->GetInstList();
    if (list.empty()) return false;
    const auto local = [&](ir::Value value, u32 point, bool implicit) {
        const auto root = ir::ResolveBitCastSource(value);
        const auto* definition = function->GetHIRValue(root);
        return definition && definition->GetBlock() == current &&
               definition->value.Def() == root.Def() && root.Id() < instruction_count &&
               (implicit ? root.Id() <= point : root.Id() < point);
    };
    bool valid = true;
    u32 previous = 0;
    bool first_inst = true;
    for (const auto& inst : list) {
        if (inst.Id() >= instruction_count || (!first_inst && inst.Id() <= previous)) {
            return false;
        }
        first_inst = false;
        previous = inst.Id();
        inst.ForEachValue([&](ir::Value value) {
            valid &= local(value, inst.Id(), false);
        });
        if (!valid) return false;
    }
    VisitImplicitUses(*current->GetBlock(), previous, [&](ir::Value value, u32 point) {
        valid &= local(value, point, true);
    });
    return valid;
}

SemanticFacts::BlockRecord& SemanticFacts::PrepareBlockRecord(const ir::Block* input) {
    const auto found = std::find_if(blocks.begin(), blocks.end(), [input](const auto& item) {
        return item.block == input;
    });
    if (found != blocks.end()) return *found;
    ASSERT(Contains(input));
    if (blocks.empty() && function) blocks.reserve(function->GetHIRBlocks().size());
    blocks.push_back({input, {}, {}, {}, {}});
    return blocks.back();
}

const BlockUseEnds& SemanticFacts::PrepareBlockUses(const ir::Block* input) {
    auto& record = PrepareBlockRecord(input);
    if (!record.ends)
        record.ends.emplace(
                input,
                instruction_count,
                record.index && record.index->UsesReady() ? record.index.get() : nullptr);
    return *record.ends;
}

const BlockSummary& SemanticFacts::PrepareSummary(const ir::Block* input) {
    (void)PrepareFlags();
    return PrepareBlockRecord(input).summary;
}

const BlockSummary* SemanticFacts::FindSummary(const ir::Block* input) const {
    if (!flags_ready) return nullptr;
    const auto found = std::find_if(blocks.begin(), blocks.end(), [input](const auto& record) {
        return record.block == input;
    });
    return found == blocks.end() ? nullptr : &found->summary;
}

size_t SemanticFacts::PreparedBlockCount() const {
    return std::count_if(
            blocks.begin(), blocks.end(), [](const auto& item) { return item.ends.has_value(); });
}

BlockAnalysisIndex& SemanticFacts::PrepareIndex(const ir::Block* input) {
    (void)PrepareFlags();
    auto& record = PrepareBlockRecord(input);
    if (!record.index) {
        auto index = std::make_unique<BlockAnalysisIndex>();
        index->BuildImpl(input, nullptr, false, &record.summary);
        index->SetPseudoFlags(std::move(record.pseudo_flags));
        record.index = std::move(index);
    }
    return *record.index;
}

BlockAnalysisIndex& SemanticFacts::PrepareUses(const ir::Block* input) {
    auto& index = PrepareIndex(input);
    index.PrepareUses();
    return index;
}

const BlockAnalysisIndex* SemanticFacts::FindUses(const ir::Block* input) const {
    const auto* index = FindIndex(input);
    return index && index->UsesReady() ? index : nullptr;
}

const BlockAnalysisIndex* SemanticFacts::FindIndex(const ir::Block* input) const {
    const auto found = std::find_if(blocks.begin(), blocks.end(), [input](const auto& item) {
        return item.block == input;
    });
    return found == blocks.end() ? nullptr : found->index.get();
}

const Vector<FlagsDemand>& SemanticFacts::PrepareFlags() {
    PrepareBaseFacts(false);
    return flags;
}

const ValueLiveness& SemanticFacts::PrepareValueLiveness() {
    PrepareBaseFacts(true);
    return *value_liveness;
}

const RegisterReadUses& SemanticFacts::PrepareRegisterReads() {
    ASSERT(HasInput());
    if (!register_reads)
        register_reads = std::unique_ptr<RegisterReadUses>{new RegisterReadUses(block, function)};
    return *register_reads;
}

void SemanticFacts::PrepareBaseFacts(bool values) {
    ASSERT(HasInput());
    if (flags_ready && (!values || value_liveness)) return;
    const bool collect_flags = !flags_ready;
    auto data =
            values && !value_liveness ? std::unique_ptr<ValueLiveness>{new ValueLiveness} : nullptr;
    std::optional<PerfScope2> perf_values;
    if (data) perf_values.emplace(GetPerfStats2().regalloc_live_scan);
    if (data) {
        data->function = function;
        data->use_ends.resize(instruction_count);
        if (function) data->block_local.resize(instruction_count, true);
    }
    std::optional<SSAValidation> ssa;
    if (data && InputSealed()) {
        ssa.emplace(block, function, instruction_count);
        ASSERT_MSG(ssa->ReferencesClosed(),
                   "sealed Function CFG references unowned or stale blocks");
    }
    if (collect_flags) {
        // Publish only after the entire visit succeeds. A failed preparation
        // can retry without accumulating partial pseudo counts or home bits.
        flags.clear();
        gpr_homes = 0;
    }
    const auto visit = [&](const ir::Block& current, const ir::HIRBlock* member, bool reachable) {
        if (ssa && reachable) ssa->BeginBlock();
        auto& block_record = PrepareBlockRecord(&current);
        auto& summary = block_record.summary;
        if (collect_flags) {
            summary = {};
            block_record.pseudo_flags = {};
        }
        const auto& list = current.GetInstList();
        const u32 block_end = list.empty() ? 0 : list.back().Id();
        const auto record = [&](ir::Value value, u32 point, bool physical_read) {
            value = ir::ResolveBitCastSource(value);
            if (!value.Defined() || value.Id() >= instruction_count) return;
            if (function) {
                const auto* definition = function->GetHIRValue(value);
                if (!definition || definition->GetBlock() != member)
                    data->block_local[value.Id()] = false;
            }
            if (reachable && physical_read) {
                auto& end = data->use_ends[value.Id()];
                end = std::max<u32>(end, point);
            }
        };
        for (const auto& inst : list) {
            const auto op = inst.GetOp();
            if (collect_flags) {
                summary.Record(inst);
                if (reachable && (op == ir::OpCode::GetHostGPR || op == ir::OpCode::SetHostGPR)) {
                    const auto home =
                            inst.GetArg<ir::Imm>(op == ir::OpCode::GetHostGPR ? 0 : 1).Get();
                    ASSERT(home < 32);
                    gpr_homes |= u32{1} << home;
                }
            }
            if (data) {
                if (reachable && op == ir::OpCode::SetHostGPR)
                    data->gpr_writes.writes.push_back(
                            {static_cast<u16>(inst.GetArg<ir::Imm>(1).Get()), inst.Id()});
                else if (reachable && op == ir::OpCode::SetHostFPR)
                    data->fpr_writes.writes.push_back(
                            {static_cast<u16>(inst.GetArg<ir::Imm>(1).Get()), inst.Id()});
                const bool physical_read =
                        function || (!inst.IsGetHostRegOperation() && !inst.IsBitCastOperation());
                const auto use = [&](ir::Value value) {
                    record(value, inst.Id(), physical_read);
                };
                if (ssa && reachable) ssa->ObserveInstruction(current, inst, use);
                else if (ssa) ssa->ObserveInstruction<false>(current, inst, use);
                else inst.ForEachValue(use);
            }
            if (!collect_flags || !reachable) continue;
            if (op != ir::OpCode::SaveFlags && op != ir::OpCode::BranchOnlyFlags) continue;
            if (!inst.ArgAt(0).IsValue()) continue;
            const auto* owner = inst.GetArg<ir::Value>(0).Def();
            if (!owner || owner->Id() >= instruction_count) continue;
            if (flags.empty()) flags.resize(instruction_count);
            auto& demand = flags[owner->Id()];
            demand.requested |= inst.GetArg<ir::Flags>(1);
            ++demand.pseudo_count;
            demand.branch_only |= op == ir::OpCode::BranchOnlyFlags;
            demand.has_save |= op == ir::OpCode::SaveFlags;
            // Local emitter demand and unit-wide scratch demand have different
            // cross-block semantics; collect both during this one IR visit.
            block_record.pseudo_flags.RecordPseudo(inst, list.front().Id(), list.back().Id());
        }
        if (data) {
            // Standalone blocks retain their numbered tail; functions use the
            // executable block tail. Locality also observes unreachable uses,
            // while interval ends and host writes only include reachable IR.
            const u32 tail = function ? block_end : instruction_count ? instruction_count - 1 : 0;
            const auto use = [&](ir::Value value, u32 point) {
                record(value, point, tail != 0 || (!function && instruction_count != 0));
            };
            if (ssa && reachable) ssa->ObserveImplicitUses(current, tail, use);
            else if (ssa) ssa->ObserveImplicitUses<false>(current, tail, use);
            else VisitImplicitUses(current, tail, use);
        }
    };
    if (function) {
        StackVector<u8, 32> reachable;
        reachable.resize(function->MaxBlockCount());
        for (const auto& member : function->GetHIRBlocksRPO()) reachable[member.GetOrderId()] = 1;
        for (const auto* member : function->GetHIRBlocks())
            if (member) visit(*member->GetBlock(), member, reachable[member->GetOrderId()] != 0);
    } else {
        visit(*block, nullptr, true);
    }
    if (data) {
        const auto order = [](const auto& a, const auto& b) {
            return a.reg < b.reg || (a.reg == b.reg && a.position < b.position);
        };
        std::sort(data->gpr_writes.writes.begin(), data->gpr_writes.writes.end(), order);
        std::sort(data->fpr_writes.writes.begin(), data->fpr_writes.writes.end(), order);
        if (function) {
            const auto bound = function->GetHIRValues().size();
            for (const auto* value : function->GetHIRValues()) {
                if (!value) continue;
                const u32 base =
                        std::max<u32>(value->GetOrderId(), data->UseEnd(value->value.Id()));
                u32 end = base;
                for (const auto& use : value->GetUses())
                    if (use.inst->Id() < bound) end = std::max<u32>(end, use.inst->Id());
                if (end > base) data->recorded_ends.emplace_back(value->value.Id(), end);
            }
        }
    }
    if (collect_flags) {
        flags_ready = true;
    }
    if (ssa) ssa_valid = ssa->Finish(*this);
    if (data) value_liveness = std::move(data);
}

const Vector<Vector<u32>>& SemanticFacts::PrepareSpillUses() {
    ASSERT(HasInput());
    if (spill_ready) return spill_uses;
    spill_uses.resize(instruction_count);
    VisitBlocks(block, function, [&](const ir::Block& current) {
        VisitUses(current, true, [&](ir::Value value, u32 position) {
            const auto root = ir::ResolveBitCastSource(value);
            if (root.Defined() && root.Id() < instruction_count) {
                spill_uses[root.Id()].push_back(position);
            }
        });
    });
    for (auto& uses : spill_uses) {
        std::sort(uses.begin(), uses.end());
        uses.erase(std::unique(uses.begin(), uses.end()), uses.end());
    }
    spill_ready = true;
    return spill_uses;
}

}  // namespace swift::runtime::compile
