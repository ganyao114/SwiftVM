#pragma once

#include <algorithm>
#include <exception>
#include <unordered_set>
#include <vector>
#include "cfg_live_range_split.h"
#include "sparse_frame_plan.h"

namespace swift::runtime::backend::x64 {
enum class CfgHomeReloadPreparationReason {
    Disabled, UnsupportedFlow, NonCanonicalIds, NoChange, BudgetChanged, FinalProof, Accepted,
};
struct CfgHomeReloadPreparationStats {
    bool enabled{}, attempted{}, changed{}, rolled_back{};
    CfgHomeReloadPreparationReason reason{CfgHomeReloadPreparationReason::Disabled};
    CfgLiveRangeSplitStats trial;
    u64 final_weighted_home_reads{};
    u32 final_allocation_builds{};
    u32 discarded_flags_allocation_builds{}, discarded_flags_allocation_recolors{};
};

// Private synchronous PrepareFunction guard. It owns no allocation token and
// never exposes a mutable callback. Only an existing spill-home reload region
// may be localized; assigned original ranges are never forcibly spilled.
class CfgHomeReloadPreparation {
public:
    CfgHomeReloadPreparation(backend::FunctionPlan& function, bool enabled, bool supported,
                            CfgLiveRangeSplitOptions options)
        : function(function), options(std::move(options)) {
        stats.enabled=enabled;
        if(!enabled)return;
        if(!supported||!function.body){stats.reason=CfgHomeReloadPreparationReason::UnsupportedFlow;return;}
        u32 count{};
        for(auto& inst:function.body->GetInstList()) {
            if(inst.Id()!=count++){stats.reason=CfgHomeReloadPreparationReason::NonCanonicalIds;return;}
        }
        if(count!=function.body->MaxInstrId()){stats.reason=CfgHomeReloadPreparationReason::NonCanonicalIds;return;}
        ready=true;
    }
    CfgHomeReloadPreparation(const CfgHomeReloadPreparation&)=delete;
    ~CfgHomeReloadPreparation(){if(copy&&!committed)Rollback();}
    void Try() {
        if(!ready)return;
        for(auto& inst:function.body->GetInstList()) {
            saved.push_back({&inst,inst.Id(),inst.GetUses(false),inst.ArgAt(0).IsValue(),
                             inst.ArgAt(0).IsValue()?inst.ArgAt(0).Get<ir::Value>():ir::Value{}});
            original.insert(&inst);
        }
        stats.attempted=true;
        stats.trial=SplitCfgScalarHomeReloads(function,options);
        if(!stats.trial.changed){stats.reason=CfgHomeReloadPreparationReason::NoChange;return;}
        stats.reason=CfgHomeReloadPreparationReason::FinalProof;
        for(auto& inst:function.body->GetInstList())if(!original.contains(&inst)) {
            if(copy||inst.GetOp()!=ir::OpCode::ZeroExtend64||!inst.ArgAt(0).IsValue())std::terminate();
            copy=&inst;source=inst.ArgAt(0).Get<ir::Value>().Def();
        }
        if(!copy||!source||!original.contains(source))std::terminate();
    }
    bool HasTrial()const{return copy!=nullptr;}
    static u64 WeightedHomeReads(const CfgRegisterPlan& plan,const SparseFramePlan& frame) {
        u64 reads{};
        for(const auto& a:plan.Allocations())if(!a.slot&&frame.Home(a.producer))reads+=a.weighted_reads;
        return reads;
    }
    bool VerifyFinal(const CfgRegisterPlan& plan,const SparseFramePlan& frame) {
        if(!copy)return false;
        if(plan.PhysicalRegisters()!=options.allocation_budget.physical_registers||
           plan.LoopWeightedCostsEnabled()!=options.allocation_budget.loop_weighted_costs||
           plan.ScalarUnaryPhasesEnabled()!=options.allocation_budget.scalar_unary_phases) {
            stats.reason=CfgHomeReloadPreparationReason::BudgetChanged;return false;
        }
        const auto failed=[&]{stats.reason=CfgHomeReloadPreparationReason::FinalProof;return false;};
        if(!plan.Enabled()||!plan.Validate()||!plan.MatchesBody(function.body.get())||!frame.Enabled()||
           plan.Register(source)||!plan.RequiresCanonicalHome(source)||plan.RequiresFullContainer(source)||
           !frame.Home(source)||frame.RematerializedValue(source)||frame.Decision(source)!=SparseHomeDecision::Retained||
           !plan.Register(copy)||plan.RequiresCanonicalHome(copy)||plan.RequiresFullContainer(copy)||frame.Home(copy)||
           frame.Decision(copy)!=SparseHomeDecision::RegisterOnly)return failed();
        u32 copy_block=UINT32_MAX,source_block=UINT32_MAX,reads{};
        for(u32 b=0;b<plan.Blocks().size();++b) {
            const auto& block=plan.Blocks()[b];
            if(std::find(block.live_in.begin(),block.live_in.end(),copy)!=block.live_in.end()||
               std::find(block.live_out.begin(),block.live_out.end(),copy)!=block.live_out.end())return failed();
            for(u32 p=block.begin;p<block.end;++p){
                if(plan.Instructions()[p]==copy)copy_block=b;
                if(plan.Instructions()[p]==source)source_block=b;
            }
        }
        if(copy_block==UINT32_MAX||source_block==UINT32_MAX||!plan.Blocks()[copy_block].reachable||
           !plan.Blocks()[copy_block].loop_depth||plan.Blocks()[source_block].loop_depth||
           !plan.DominatesBlock(source_block,copy_block)||plan.Blocks()[copy_block].incoming_edges.size()!=1||
           plan.Edges()[plan.Blocks()[copy_block].incoming_edges[0]].host_entry)return failed();
        for(u32 p=0;p<plan.Instructions().size();++p)for(u32 slot=0;slot<ir::Inst::max_args;++slot) {
            auto& arg=plan.Instructions()[p]->ArgAt(slot);
            if(!arg.IsValue()||arg.Get<ir::Value>().Def()!=copy)continue;
            if(slot||p<plan.Blocks()[copy_block].begin||p>=plan.Blocks()[copy_block].end)return failed();
            ++reads;
        }
        if(reads!=stats.trial.rewritten_u64_reads||copy->GetUses(false)!=reads)return failed();
        stats.final_weighted_home_reads=WeightedHomeReads(plan,frame);
        if(stats.final_weighted_home_reads>=stats.trial.weighted_home_reads_before)return failed();
        stats.reason=CfgHomeReloadPreparationReason::Accepted;return true;
    }
    void Commit() noexcept {
        if(!copy||stats.reason!=CfgHomeReloadPreparationReason::Accepted)std::terminate();
        stats.changed=true;committed=true;
    }
    void Rollback() noexcept {
        if(!copy)return;
        for(auto it=saved.rbegin();it!=saved.rend();++it)
            if(it->has_value&&it->inst->ArgAt(0).IsValue()&&it->inst->ArgAt(0).Get<ir::Value>().Def()==copy)
                it->inst->SetArg(0,it->value);
        function.body->RemoveInst(copy);delete copy;copy=nullptr;
        // Eligibility required canonical IDs, so this restores IDs/MaxInstrId
        // exactly despite the standalone pass's accepted ReIdInstr.
        function.body->ReIdInstr();
        auto it=function.body->GetInstList().begin();
        for(const auto& before:saved) {
            if(it==function.body->GetInstList().end()||&*it!=before.inst||it->Id()!=before.id||
               it->GetUses(false)!=before.uses)std::terminate();
            ++it;
        }
        if(it!=function.body->GetInstList().end()||function.body->MaxInstrId()!=saved.size())std::terminate();
        stats.changed=false;stats.rolled_back=true;stats.final_weighted_home_reads=stats.trial.weighted_home_reads_before;
    }
    const CfgHomeReloadPreparationStats& Statistics()const{return stats;}
private:
    struct Saved {ir::Inst* inst;u16 id;u8 uses;bool has_value;ir::Value value;};
    backend::FunctionPlan& function;
    CfgLiveRangeSplitOptions options;
    CfgHomeReloadPreparationStats stats;
    bool ready{},committed{};
    ir::Inst* source{},*copy{};
    std::vector<Saved> saved;
    std::unordered_set<ir::Inst*> original;
};
} // namespace swift::runtime::backend::x64
