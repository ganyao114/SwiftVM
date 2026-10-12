#include "cfg_live_range_split.h"
#include "flags_usage.h"
#include "sparse_frame_plan.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace swift::runtime::backend::x64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
struct Candidate { ir::Inst* source{}; ir::Inst* anchor{}; u32 block{}; std::vector<ir::Inst*> reads; };

struct Metadata {
    std::unordered_set<ir::Inst*> flags, protected_values;
    bool valid{true};
};
Metadata InspectMetadata(const CfgRegisterPlan& plan) {
    Metadata result;
    std::unordered_set<ir::Inst*> seen;
    const auto tail=[](const ir::Inst* inst,u32 first) {
        for(u32 i=first;i<ir::Inst::max_args;++i)if(!inst->ArgAt(i).IsVoid())return false;
        return true;
    };
    const auto mask=[](const ir::Inst* inst,u32 slot) {
        return inst->ArgAt(slot).GetType()==ir::ArgType::Flags &&
               False(inst->GetArg<ir::Flags>(slot)&~ir::Flags::All);
    };
    const auto named_value=[&](const ir::Inst* inst) {
        if(!inst->ArgAt(0).IsValue())return false;
        const auto value=inst->GetArg<ir::Value>(0);
        if(!value.Defined()||!seen.contains(value.Def())||value.Type()<T::U8||value.Type()>T::S64)return false;
        result.protected_values.insert(value.Def());return true;
    };
    for(auto* inst:plan.Instructions()) {
        bool valid=true;
        switch(inst->GetOp()) {
            case O::GetFlags: case O::SaveFlags: case O::BranchOnlyFlags:
                // ir.inc declares GetFlags(Value producer, Flags mask), just
                // like the two publication pseudos. It is NOT Flags-only.
                valid=named_value(inst)&&mask(inst,1)&&tail(inst,2);
                if(valid&&inst->GetOp()!=O::GetFlags) {
                    auto* source=inst->GetArg<ir::Value>(0).Def();
                    if(ClassifyProducerFlags(source->GetOp(),source->ReturnType(),inst->GetArg<ir::Flags>(1)).needs_metadata)
                        result.flags.insert(source);
                }
                break;
            case O::TestFlags: case O::TestNotFlags: case O::ClearFlags:
                valid=mask(inst,0)&&tail(inst,1);break;
            case O::SetCarry: case O::SetOverflow:
                valid=named_value(inst)&&tail(inst,1);break;
            case O::InvertCarry:
                valid=tail(inst,0);break;
            case O::PublishFCmpFlags:
                valid=named_value(inst)&&inst->ArgAt(1).IsImm()&&inst->GetArg<ir::Imm>(1).Get()<=1&&tail(inst,2);break;
            case O::PublishSse42StrFlags:
                valid=named_value(inst)&&mask(inst,1)&&tail(inst,2);break;
            default: break;
        }
        if(!valid){result.valid=false;return result;} // Unsupported optimization shape: compile normally.
        seen.insert(inst);
    }
    return result;
}

bool PureSource(ir::Inst* inst) {
    if (inst->ReturnType() != T::U64 || inst->IsPseudoOperation()) return false;
    // Keep the first experiment away from uniform/guest memory state, Phi,
    // constants/rematerialization, raw aliases, callbacks and result pairs.
    switch (inst->GetOp()) {
        case O::Add: case O::Sub: case O::And: case O::Or: case O::Xor: case O::AndNot: return true;
        default: return false;
    }
}
bool PlainU64Binary(ir::Inst* inst) {
    if (!PureSource(inst) || !inst->ArgAt(0).IsValue() || inst->GetArg<ir::Value>(0).Type() != T::U64 ||
        !inst->ArgAt(1).IsOperand()) return false;
    // Operand occupies slots 1..3. The null right and unused physical tail
    // must actually be Void; ToDataClass alone would hide opaque arguments.
    if (!inst->ArgAt(3).IsVoid() || !inst->ArgAt(4).IsVoid() ||
        (!inst->ArgAt(2).IsImm() && !inst->ArgAt(2).IsValue())) return false;
    const auto operand = inst->GetArg<ir::Operand>(1);
    const auto op = operand.GetOp();
    const auto left = operand.GetLeft();
    // Operand{Imm}/Operand{Value} use default Plus with an empty right.
    // Both None and this canonical Plus emit only the left data value.
    return (op.type == ir::OperandOp::None || op.type == ir::OperandOp::Plus) &&
        op.shift_ext == 0 && operand.GetRight().Null() &&
        ((left.IsImm() && left.imm.GetType() == T::U64) ||
         (left.IsValue() && left.value.Type() == T::U64));
}
bool RegionInstruction(const backend::FunctionPlan& function, ir::Inst* inst, const Metadata& metadata) {
    if (function.exits.contains(inst) || function.halt_checks.contains(inst) || metadata.protected_values.contains(inst)) return false;
    if (PlainU64Binary(inst)) return true;
    // Unchanged architectural stores and a final jump may coexist with the
    // pure reads. No uniform/guest loads, observers, metadata or helper calls
    // are permitted in this minimal complete-block region.
    switch (inst->GetOp()) {
        case O::BindLabel: case O::Nop: case O::LoadImm: case O::Goto: case O::NotGoto: return true;
        case O::StoreUniform:
            return inst->ArgAt(0).GetType() == ir::ArgType::Uniform &&
                   inst->GetArg<ir::Uniform>(0).GetType() == T::U64 && inst->ArgAt(1).IsValue() &&
                   inst->GetArg<ir::Value>(1).Type() == T::U64;
        default: return false;
    }
}
u64 WeightedHomeReads(const CfgRegisterPlan& plan, const SparseFramePlan& frame) {
    u64 cost{};
    for (const auto& allocation : plan.Allocations())
        if (!allocation.slot && frame.Home(allocation.producer)) cost += allocation.weighted_reads;
    return cost;
}
bool SourceHome(const CfgRegisterPlan& plan, const SparseFramePlan& frame, ir::Inst* source) {
    return !plan.Register(source) && plan.RequiresCanonicalHome(source) && !plan.RequiresFullContainer(source) &&
        frame.Decision(source) == SparseHomeDecision::Retained && frame.Home(source) && !frame.RematerializedValue(source);
}
std::vector<Candidate> FindCandidates(const backend::FunctionPlan& function, const CfgRegisterPlan& plan,
                                     const SparseFramePlan& frame, const Metadata& metadata, u32 minimum) {
    std::unordered_map<ir::Inst*,u32> blocks;
    for (u32 b = 0; b < plan.Blocks().size(); ++b)
        for (u32 p = plan.Blocks()[b].begin; p < plan.Blocks()[b].end; ++p) blocks.emplace(plan.Instructions()[p],b);
    std::vector<Candidate> result;
    for (const auto& allocation : plan.Allocations()) {
        auto* source = allocation.producer;
        const auto source_block = blocks.at(source);
        if (!PlainU64Binary(source) || metadata.protected_values.contains(source) || allocation.uses >= 254 ||
            plan.Blocks()[source_block].loop_depth || !plan.Blocks()[source_block].reachable || !SourceHome(plan,frame,source)) continue;
        for (u32 b = 0; b < plan.Blocks().size(); ++b) {
            const auto& block = plan.Blocks()[b];
            if (!block.reachable || !block.loop_depth || b == source_block || !plan.DominatesBlock(source_block,b) ||
                block.incoming_edges.size() != 1 || plan.Edges()[block.incoming_edges[0]].host_entry ||
                block.end <= block.begin) continue;
            Candidate candidate{source,nullptr,b,{}};
            bool safe = true;
            for (u32 p = block.begin; p < block.end; ++p) {
                auto* inst = plan.Instructions()[p];
                if (!RegionInstruction(function,inst,metadata)) { safe = false; break; }
                if (!candidate.anchor && inst->GetOp() != O::BindLabel) candidate.anchor = inst;
                if (PlainU64Binary(inst) && inst->GetArg<ir::Value>(0).Def() == source) candidate.reads.push_back(inst);
            }
            if (safe && candidate.anchor && candidate.reads.size() >= minimum && candidate.reads.size() <= 32)
                result.push_back(std::move(candidate));
        }
    }
    // Allocation and block iteration are already in explicit instruction/CFG
    // order; no pointer, hash-table or optimistic cost tie chooses a candidate.
    return result;
}
class Trial {
public:
    Trial(ir::Block& body, const Candidate& candidate) : body(body), source(candidate.source), original_max_id(body.MaxInstrId()) {
        for (auto& inst : body.GetInstList()) snapshot.push_back({&inst,inst.Id(),inst.GetUses(false)});
        for (auto* inst : candidate.reads) edits.push_back({inst,inst->GetArg<ir::Value>(0)});
        // Construction only captures evidence. All mutations belong to Apply
        // on a fully constructed guard, whose destructor runs on exceptions.
    }
    void Apply(ir::Inst* anchor) {
        // Take ownership before registering the new copy's source use.
        copy.reset(new ir::Inst(O::ZeroExtend64));
        copy->SetArg(0,ir::Value{source}.SetCastType(T::U64));
        copy->SetReturn(T::U64); ir::Inst::Validate(copy.get());
        body.InsertBefore(copy.get(),anchor);
        for (const auto& edit : edits) {
            // Include the current scalar setter in restoration if it throws.
            ++applied; edit.first->SetArg(0,ir::Value{copy.get()}.SetCastType(T::U64));
        }
    }
    Trial(const Trial&) = delete;
    ~Trial() { if (!committed) Restore(); }
    ir::Inst* Copy() const { return copy.get(); }
    void Restore() noexcept {
        for (u32 i = applied; i-- > 0;) edits[i].first->SetArg(0,edits[i].second);
        applied = 0;
        // Check the actual intrusive node, including a partially failed link.
        if (copy && copy->list_node.IsLinked()) body.RemoveInst(copy.get());
        copy.reset();
        auto it = body.GetInstList().begin();
        for (const auto& saved : snapshot) {
            // A violation is an internal ownership error, never an accepted
            // optimization. IDs/MaxInstrId are never changed during a trial.
            if (it == body.GetInstList().end() || &*it != saved.inst || it->Id() != saved.id ||
                it->GetUses(false) != saved.uses) std::terminate();
            ++it;
        }
        if (it != body.GetInstList().end() || body.MaxInstrId() != original_max_id) std::terminate();
    }
    void Commit() noexcept { committed = true; copy.release(); body.ReIdInstr(); }
private:
    struct Saved { ir::Inst* inst; u16 id; u8 uses; };
    ir::Block& body;
    ir::Inst* source;
    u32 original_max_id;
    std::unique_ptr<ir::Inst> copy;
    std::vector<Saved> snapshot;
    std::vector<std::pair<ir::Inst*,ir::Value>> edits;
    u32 applied{};
    bool committed{};
};
bool CopyContained(const CfgRegisterPlan& plan, const Candidate& candidate, ir::Inst* copy) {
    if (candidate.block >= plan.Blocks().size()) return false;
    const auto& block = plan.Blocks()[candidate.block];
    if (std::find(block.live_in.begin(),block.live_in.end(),copy) != block.live_in.end() ||
        std::find(block.live_out.begin(),block.live_out.end(),copy) != block.live_out.end()) return false;
    u32 occurrences{};
    for (u32 p = 0; p < plan.Instructions().size(); ++p) {
        auto* inst = plan.Instructions()[p];
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst->ArgAt(slot);
            if (!arg.IsValue() || arg.Get<ir::Value>().Def() != copy) continue;
            if (slot != 0 || p < block.begin || p >= block.end ||
                std::find(candidate.reads.begin(),candidate.reads.end(),inst) == candidate.reads.end()) return false;
            ++occurrences;
        }
    }
    return occurrences == candidate.reads.size() && copy->GetUses(false) == occurrences;
}
} // namespace

CfgLiveRangeSplitStats SplitCfgScalarHomeReloads(backend::FunctionPlan& function, CfgLiveRangeSplitOptions options) {
    CfgLiveRangeSplitStats stats;
    if (!function.body || options.minimum_reads < 3 || options.minimum_reads > 32 || !options.maximum_attempts ||
        !options.allocation_budget.loop_weighted_costs) return stats;
    auto* body = function.body.get();
    // Commit's single ReIdInstr remains within the existing u16 ID contract.
    u32 count{}; for (auto& inst : body->GetInstList()) ++count;
    if (count >= ir::Inst::invalid_id) return stats;
    ++stats.full_allocation_builds;
    const auto baseline = CfgRegisterPlan::Build(body,function,options.allocation_budget);
    if (!baseline.Enabled() || !baseline.Validate()) return stats;
    const auto metadata = InspectMetadata(baseline);
    if (!metadata.valid) return stats;
    const auto frame = SparseFramePlan::Build(body,&function,baseline,metadata.flags,options.scalar_rematerialization);
    if (!frame.Enabled() || !frame.Validate(body,&function,baseline,metadata.flags,options.scalar_rematerialization)) return stats;
    stats.accepted = true;
    stats.weighted_home_reads_before = stats.weighted_home_reads_after = WeightedHomeReads(baseline,frame);
    const auto candidates = FindCandidates(function,baseline,frame,metadata,options.minimum_reads);
    stats.candidate_regions = candidates.size();
    for (const auto& candidate : candidates) {
        if (stats.attempts == options.maximum_attempts) break;
        ++stats.attempts;
        Trial trial{*body,candidate};
        trial.Apply(candidate.anchor);
        // Baseline analysis is now historical evidence, not current allocation
        // authority. All semantic and frame postchecks use this fresh build.
        ++stats.full_allocation_builds;
        const auto fresh = CfgRegisterPlan::Build(body,function,options.allocation_budget);
        if (!fresh.Enabled() || !fresh.Validate() || fresh.PhysicalRegisters() != baseline.PhysicalRegisters()) {
            ++stats.rejected_allocation; continue;
        }
        const auto fresh_metadata = InspectMetadata(fresh);
        if (!fresh_metadata.valid) { ++stats.rejected_allocation; continue; }
        const auto fresh_frame = SparseFramePlan::Build(body,&function,fresh,fresh_metadata.flags,options.scalar_rematerialization);
        auto* copy = trial.Copy();
        if (!fresh_frame.Enabled() || !fresh_frame.Validate(body,&function,fresh,fresh_metadata.flags,options.scalar_rematerialization) || !SourceHome(fresh,fresh_frame,candidate.source) || !fresh.Register(copy) ||
            fresh.RequiresCanonicalHome(copy) || fresh.RequiresFullContainer(copy) || fresh_frame.Home(copy) ||
            fresh_frame.Decision(copy) != SparseHomeDecision::RegisterOnly) {
            ++stats.rejected_home; continue;
        }
        if (!CopyContained(fresh,candidate,copy)) { ++stats.rejected_escape; continue; }
        const auto cost = WeightedHomeReads(fresh,fresh_frame);
        if (cost >= stats.weighted_home_reads_before) { ++stats.rejected_cost; continue; }
        const auto weight = fresh.Blocks().at(candidate.block).loop_weight;
        stats.weighted_home_reads_after = cost;
        stats.extra_scalar_copies = 1; stats.rewritten_u64_reads = candidate.reads.size();
        stats.estimated_region_home_reads_removed = u64(candidate.reads.size()) * weight;
        stats.estimated_copy_home_reads_added = weight;
        trial.Commit(); stats.changed = true;
        return stats;
    }
    return stats;
}
} // namespace swift::runtime::backend::x64
