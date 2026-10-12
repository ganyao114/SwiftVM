#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <xmmintrin.h>
#include "runtime/backend/x64/cfg_live_range_split.h"
#include "runtime/backend/x64/sparse_frame_plan.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
constexpr u32 extent = 1024, repetitions = 10, target_reads = 4;
constexpr u64 flags_seed = 0xfedcba9876543219ULL;
struct Program {
    backend::FunctionPlan function;
    ir::Assembler as;
    ir::Value source;
    Program() : as((function.body = new ir::Block(ir::Location{0x1000})).get()) {}
    ir::Value Load(u32 at) { return as.LoadUniform(ir::Uniform{at,T::U64}).SetType(T::U64); }
    ir::Value Add(ir::Value value, u64 imm) { return as.Add(value,ir::Operand{ir::Imm{imm}}).SetType(T::U64); }
    void Store(u32 at, ir::Value value) { as.StoreUniform(ir::Uniform{at,T::U64},value); }
    ir::Inst* Label() {
        auto* label = new ir::Inst(O::BindLabel); label->SetArgs(ir::Value{label}); function.body->AppendInst(label); return label;
    }
    void Jump(ir::Inst* label) {
        auto branch = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
        function.targets[branch.Def()] = label; function.stops.insert(branch.Def());
    }
    void Finish() { function.body->SetTerminal(ir::terminal::ReturnToHost{}); }
};
void Fixture(Program& p, u32 reads = target_reads, bool source_flags = false, bool raw_source = false, bool preheader_flags = false) {
    if(preheader_flags) {
        auto observer=p.as.TestFlags(ir::Flags::NZCV).SetType(T::U8);
        p.as.StoreUniform(ir::Uniform{976,T::U8},observer);
    }
    p.source = p.Add(p.Load(0),17);
    if (source_flags) p.as.SaveFlags(p.source,ir::Flags::All);
    auto* loop = p.Label();
    // These high-benefit temporaries are defined anew on every iteration,
    // overlap each other in this predecessor, and die before the target.
    // Unlike immutable loop invariants, they are not live across the backedge.
    std::array<ir::Value,3> pressure;
    for (u32 lane = 0; lane < 3; ++lane) pressure[lane] = p.Add(p.Load(16+8*lane),lane+1);
    for (u32 stage = 0; stage < repetitions; ++stage) for (u32 lane = 0; lane < 3; ++lane)
        p.Store(128+128*lane+8*stage,p.Add(pressure[lane],31+stage));
    auto* target = p.Label(); p.Jump(target); // fixed up below: jumps belong to predecessors.
    // Relocate the jump before the target label, preserving one incoming edge.
    auto& list = p.function.body->GetInstList();
    auto* jump = &list.back(); p.function.body->RemoveInst(jump); p.function.body->InsertBefore(jump,target);
    auto* constant = jump->GetArg<ir::Value>(0).Def();
    p.function.body->RemoveInst(constant); p.function.body->InsertBefore(constant,jump);
    for (u32 index = 0; index < reads; ++index) p.Store(640+8*index,p.Add(p.source,71+index));
    auto* latch = p.Label(); p.Jump(latch);
    jump = &list.back(); p.function.body->RemoveInst(jump); p.function.body->InsertBefore(jump,latch);
    constant = jump->GetArg<ir::Value>(0).Def();
    p.function.body->RemoveInst(constant); p.function.body->InsertBefore(constant,jump);
    auto remaining = p.as.Sub(p.Load(8),ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    p.Store(8,remaining);
    auto back = p.as.Goto(ir::BOOL{p.as.TestNotZero(remaining).SetType(T::U8)});
    p.function.targets[back.Def()] = loop;
    p.Store(928,p.source);
    if (raw_source) {
        auto raw = p.as.BitCast(p.source).SetType(T::V128);
        p.as.StoreUniform(ir::Uniform{944,T::V128},raw);
    }
    p.Finish();
}
x64::CfgRegisterOptions Budget(std::vector<unsigned> regs = {3,5,15}) {
    return {.physical_registers = std::move(regs),.loop_weighted_costs = true,.scalar_unary_phases = true};
}
struct Snapshot {
    std::string text;
    std::vector<ir::Inst*> membership;
    std::vector<u16> ids;
    std::vector<u8> uses;
    u32 max_id;
    explicit Snapshot(Program& p) : text(p.function.body->ToString()),max_id(p.function.body->MaxInstrId()) {
        for (auto& inst : p.function.body->GetInstList()) { membership.push_back(&inst); ids.push_back(inst.Id()); uses.push_back(inst.GetUses(false)); }
    }
    void RequireSame(Program& p) const {
        REQUIRE(text == p.function.body->ToString()); REQUIRE(max_id == p.function.body->MaxInstrId());
        u32 index{}; for (auto& inst : p.function.body->GetInstList()) {
            REQUIRE(index < membership.size()); REQUIRE(membership[index] == &inst);
            REQUIRE(ids[index] == inst.Id()); REQUIRE(uses[index] == inst.GetUses(false)); ++index;
        }
        REQUIRE(index == membership.size());
    }
};
struct Machine {
    alignas(backend::State) std::array<u8,sizeof(backend::State)+extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine(u64 seed, u64 trips) {
        for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i*37+13);
        state->host_cpu_flags = flags_seed;
        Put(0,seed); Put(8,trips);
        for (u32 lane = 0; lane < 3; ++lane) Put(16+8*lane,seed ^ (0x9e3779b97f4a7c15ULL*(lane+1)));
    }
    ~Machine() { state->~State(); }
    void Put(u32 at, u64 value) { std::memcpy(state->uniform_buffer_begin+at,&value,8); }
    u64 Get(u32 at) const { u64 value; std::memcpy(&value,state->uniform_buffer_begin+at,8); return value; }
    void Model(bool observed_flags = false) {
        const auto value = Get(0)+17;
        const auto trips = Get(8);
        for (u64 trip = 0; trip < trips; ++trip) {
            for (u32 stage = 0; stage < repetitions; ++stage) for (u32 lane = 0; lane < 3; ++lane)
                Put(128+128*lane+8*stage,Get(16+8*lane)+lane+1+31+stage);
            for (u32 index = 0; index < target_reads; ++index) Put(640+8*index,value+71+index);
            Put(8,Get(8)-1);
        }
        Put(928,value);
        if(observed_flags)state->uniform_buffer_begin[976]=1;
        state->current_loc = state->prev_loc = ir::Location{0x1000}; state->halt_reason = HaltReason::CallHost;
    }
};
} // namespace


// Synthetic lowered IR: this fixture proves the selective preparation path;
// it is not decoded AArch64 evidence and does not claim arbitrary splitting.
TEST_CASE("Prepare hot home reloads require a fresh final whole allocation", "[x64][cfg][home-reload][integration]") {
    Config config{.backend_isa=kX86_64,.uniform_buffer_size=extent,.stack_alignment=16};
    for(bool resident:{false,true}) {
        Program off,on;Fixture(off,target_reads,false,false,resident);Fixture(on,target_reads,false,false,resident);
        off.function.body->ReIdInstr();on.function.body->ReIdInstr();
        auto options=x64::FunctionPreparationOptions{};
        options.uniform_forwarding=false;options.state_promotion=false;
        options.profitable_flags_residency=false;
        const auto baseline=x64::CfgRegisterPlan::Build(on.function.body.get(),on.function,
            x64::Translator::AllocationBudget(on.function.body.get(),&on.function,resident));
        REQUIRE(baseline.Validate());REQUIRE_FALSE(baseline.Register(on.source.Def()));
        const auto baseline_frame=x64::SparseFramePlan::Build(on.function.body.get(),&on.function,baseline,{});
        REQUIRE(baseline_frame.Enabled());REQUIRE(baseline_frame.Home(on.source.Def()));
        const auto before=x64::CfgHomeReloadPreparation::WeightedHomeReads(baseline,baseline_frame);
        auto a=x64::Translator::PrepareFunction(off.function,x64::HostFeatures::Detect(),&config,true,true,resident,options);
        options.scalar_home_reload_split=true;
        auto b=x64::Translator::PrepareFunction(on.function,x64::HostFeatures::Detect(),&config,true,true,resident,options);
        REQUIRE_FALSE(a->HomeReloadSplitStats().enabled);
        REQUIRE(b->HomeReloadSplitStats().enabled);REQUIRE(b->HomeReloadSplitStats().attempted);
        REQUIRE(b->HomeReloadSplitStats().changed);REQUIRE_FALSE(b->HomeReloadSplitStats().rolled_back);
        REQUIRE(b->HomeReloadSplitStats().reason==x64::CfgHomeReloadPreparationReason::Accepted);
        REQUIRE(b->HomeReloadSplitStats().trial.full_allocation_builds>=2);
        REQUIRE(b->PreparationStats().full_allocation_builds>=3);
        REQUIRE(b->PreparationStats().reused_final_allocation);
        a->Translate();b->Translate();
        REQUIRE(a->FlagsResident()==resident);REQUIRE(b->FlagsResident()==resident);
        REQUIRE(a->CfgPlan().Validate());REQUIRE(b->CfgPlan().Validate());
        REQUIRE(a->CfgPlan().PhysicalRegisters()==b->CfgPlan().PhysicalRegisters());
        REQUIRE(b->CfgPlan().PhysicalRegisters()==(resident?std::vector<unsigned>{5,15}:std::vector<unsigned>{3,5,15}));
        REQUIRE_FALSE(b->CfgPlan().Register(on.source.Def()));REQUIRE(b->CfgPlan().RequiresCanonicalHome(on.source.Def()));
        REQUIRE(b->FramePlan().Home(on.source.Def()));
        REQUIRE(b->FramePlan().Decision(on.source.Def())==x64::SparseHomeDecision::Retained);
        REQUIRE_FALSE(b->FramePlan().RematerializedValue(on.source.Def()));
        u32 copies{};
        for(auto* inst:b->CfgPlan().Instructions())if(inst->GetOp()==O::ZeroExtend64) {
            ++copies;REQUIRE(inst->GetArg<ir::Value>(0).Def()==on.source.Def());REQUIRE(b->CfgPlan().Register(inst));
            REQUIRE_FALSE(b->CfgPlan().RequiresCanonicalHome(inst));REQUIRE_FALSE(b->FramePlan().Home(inst));
            REQUIRE(b->FramePlan().Decision(inst)==x64::SparseHomeDecision::RegisterOnly);
            for(const auto& block:b->CfgPlan().Blocks()) {
                REQUIRE(std::find(block.live_in.begin(),block.live_in.end(),inst)==block.live_in.end());
                REQUIRE(std::find(block.live_out.begin(),block.live_out.end(),inst)==block.live_out.end());
            }
        }
        REQUIRE(copies==1);
        const auto after=x64::CfgHomeReloadPreparation::WeightedHomeReads(b->CfgPlan(),b->FramePlan());
        REQUIRE(after<before);REQUIRE(after==b->HomeReloadSplitStats().final_weighted_home_reads);
        REQUIRE(b->Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_load_sites <
                a->Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_load_sites);
        const auto& ua=a->Statistics().At(x64::TransferRegion::Uniform);
        const auto& ub=b->Statistics().At(x64::TransferRegion::Uniform);
        REQUIRE(ua.scalar_load_sites==ub.scalar_load_sites);REQUIRE(ua.scalar_store_sites==ub.scalar_store_sites);
        for(u64 seed:{u64{0},UINT64_MAX,u64{0x123456789abcdef0}})for(u64 trips:{u64{1},u64{2},u64{7}}) {
            Machine expected{seed,trips},first{seed,trips},second{seed,trips};expected.Model(resident);
            const auto mxcsr=_mm_getcsr();
            REQUIRE(a->getCode<HaltReason(*)(backend::State*)>()(first.state)==HaltReason::CallHost);
            REQUIRE(_mm_getcsr()==mxcsr);
            REQUIRE(b->getCode<HaltReason(*)(backend::State*)>()(second.state)==HaltReason::CallHost);
            REQUIRE(_mm_getcsr()==mxcsr);REQUIRE(first.bytes==expected.bytes);REQUIRE(second.bytes==expected.bytes);
        }
    }
}

TEST_CASE("Prepare provisional home reload rolls back a real changed final budget", "[x64][cfg][home-reload][rollback]") {
    Program p;Fixture(p);p.function.body->ReIdInstr();Snapshot snapshot{p};
    const auto targets=p.function.targets;
    const auto locations=p.function.locations;
    const auto stops=p.function.stops,halts=p.function.halt_checks,labels=p.function.synthetic_labels;
    const auto baseline=x64::CfgRegisterPlan::Build(p.function.body.get(),p.function,Budget());
    REQUIRE(baseline.Validate());REQUIRE_FALSE(baseline.Register(p.source.Def()));
    auto guard=x64::CfgHomeReloadPreparation{p.function,true,true,{.allocation_budget=Budget()}};
    guard.Try();REQUIRE(guard.HasTrial());REQUIRE(guard.Statistics().trial.changed);
    // Build the complete mutated function at an actual smaller flags budget.
    // A successful standalone 3-slot proof cannot authorize these 2 slots.
    const auto final=x64::CfgRegisterPlan::Build(p.function.body.get(),p.function,Budget({5,15}));
    REQUIRE(final.Validate());
    const auto frame=x64::SparseFramePlan::Build(p.function.body.get(),&p.function,final,{});
    REQUIRE(frame.Validate(p.function.body.get(),&p.function,final,{}));
    REQUIRE_FALSE(guard.VerifyFinal(final,frame));
    REQUIRE(guard.Statistics().reason==x64::CfgHomeReloadPreparationReason::BudgetChanged);
    guard.Rollback();REQUIRE(guard.Statistics().rolled_back);REQUIRE_FALSE(guard.Statistics().changed);
    snapshot.RequireSame(p);REQUIRE(p.function.targets==targets);REQUIRE(p.function.locations==locations);
    REQUIRE(p.function.stops==stops);REQUIRE(p.function.halt_checks==halts);REQUIRE(p.function.synthetic_labels==labels);
    const auto restored=x64::CfgRegisterPlan::Build(p.function.body.get(),p.function,Budget());
    REQUIRE(restored.Validate());REQUIRE(restored.Allocations().size()==baseline.Allocations().size());
    for(const auto& old:baseline.Allocations()) {
        REQUIRE(restored.Register(old.producer)==baseline.Register(old.producer));
        REQUIRE(restored.RequiresCanonicalHome(old.producer)==old.requires_canonical_home);
        REQUIRE(restored.RequiresFullContainer(old.producer)==old.requires_full_container);
    }
}

TEST_CASE("Prepare home reload controls expose no candidate and unsupported feedback", "[x64][cfg][home-reload][controls]") {
    Config config{.backend_isa=kX86_64,.uniform_buffer_size=extent,.stack_alignment=16};
    for(u32 variant=0;variant<3;++variant) {
        Program p;Fixture(p,variant==0?2:target_reads);p.function.body->ReIdInstr();
        auto options=x64::FunctionPreparationOptions{};options.scalar_home_reload_split=true;
        options.uniform_forwarding=variant==1;
        options.loop_weighted_costs=variant!=2;
        auto code=x64::Translator::PrepareFunction(p.function,x64::HostFeatures::Detect(),&config,true,true,false,options);
        code->Translate();REQUIRE(code->CfgPlan().Validate());REQUIRE_FALSE(code->HomeReloadSplitStats().changed);
        if(variant==1) {
            REQUIRE_FALSE(code->HomeReloadSplitStats().attempted);
            REQUIRE(code->HomeReloadSplitStats().reason==x64::CfgHomeReloadPreparationReason::UnsupportedFlow);
        } else {
            REQUIRE(code->HomeReloadSplitStats().attempted);
            REQUIRE(code->HomeReloadSplitStats().reason==x64::CfgHomeReloadPreparationReason::NoChange);
        }
    }
}


TEST_CASE("Prepare final flags rejection rebuilds the restored whole function", "[x64][cfg][home-reload][rollback][flags]") {
    Config config{.backend_isa=kX86_64,.uniform_buffer_size=extent,.stack_alignment=16};
    Program off,on;Fixture(off,target_reads,false,false,true);Fixture(on,target_reads,false,false,true);
    off.function.body->ReIdInstr();on.function.body->ReIdInstr();Snapshot snapshot{on};
    const auto targets=on.function.targets;
    const auto locations=on.function.locations;
    const auto stops=on.function.stops,halts=on.function.halt_checks,labels=on.function.synthetic_labels;
    auto options=x64::FunctionPreparationOptions{};options.profitable_flags_residency=true;
    auto a=x64::Translator::PrepareFunction(off.function,x64::HostFeatures::Detect(),&config,true,true,true,options);
    options.scalar_home_reload_split=true;
    auto b=x64::Translator::PrepareFunction(on.function,x64::HostFeatures::Detect(),&config,true,true,true,options);
    // One cold TestFlags read cannot pay even the two-site residency entry/exit
    // overhead. An actual two-slot accepted trial must roll back when the
    // final selector chooses three scalar slots, then reselect on restored IR.
    REQUIRE(b->HomeReloadSplitStats().trial.changed);
    REQUIRE(b->HomeReloadSplitStats().rolled_back);REQUIRE_FALSE(b->HomeReloadSplitStats().changed);
    REQUIRE(b->HomeReloadSplitStats().reason==x64::CfgHomeReloadPreparationReason::BudgetChanged);
    REQUIRE(b->HomeReloadSplitStats().final_allocation_builds==1);
    REQUIRE(b->HomeReloadSplitStats().discarded_flags_allocation_builds>0);
    REQUIRE(b->PreparationStats().flags_policy_allocation_builds>a->PreparationStats().flags_policy_allocation_builds);
    REQUIRE_FALSE(a->FlagsResident());REQUIRE_FALSE(b->FlagsResident());
    snapshot.RequireSame(on);
    REQUIRE(on.function.targets==targets);REQUIRE(on.function.locations==locations);
    REQUIRE(on.function.stops==stops);REQUIRE(on.function.halt_checks==halts);REQUIRE(on.function.synthetic_labels==labels);
    REQUIRE(a->FlagsDecision().selected==b->FlagsDecision().selected);
    REQUIRE(a->FlagsDecision().reason==b->FlagsDecision().reason);
    REQUIRE(a->FlagsDecision().scalar_lost_benefit==b->FlagsDecision().scalar_lost_benefit);
    REQUIRE(a->FlagsDecision().net_savings==b->FlagsDecision().net_savings);
    REQUIRE(a->CfgPlan().Validate());REQUIRE(b->CfgPlan().Validate());
    REQUIRE(b->CfgPlan().PhysicalRegisters()==(std::vector<unsigned>{3,5,15}));
    REQUIRE(b->PreparationStats().reused_final_allocation);
    REQUIRE(a->CfgPlan().Allocations().size()==b->CfgPlan().Allocations().size());
    for(u32 i=0;i<a->CfgPlan().Allocations().size();++i) {
        const auto& first=a->CfgPlan().Allocations()[i];const auto& second=b->CfgPlan().Allocations()[i];
        REQUIRE(first.producer->Id()==second.producer->Id());REQUIRE(first.slot==second.slot);
        REQUIRE(first.requires_canonical_home==second.requires_canonical_home);
        REQUIRE(first.requires_full_container==second.requires_full_container);
        REQUIRE(a->FramePlan().Home(first.producer)==b->FramePlan().Home(second.producer));
        REQUIRE(a->FramePlan().Decision(first.producer)==b->FramePlan().Decision(second.producer));
    }
    a->Translate();b->Translate();
    REQUIRE(a->getSize()==b->getSize());REQUIRE(std::memcmp(a->getCode(),b->getCode(),a->getSize())==0);
    for(u64 seed:{u64{0},UINT64_MAX,u64{0x123456789abcdef0}})for(u64 trips:{u64{1},u64{2},u64{7}}) {
        Machine actual{seed,trips},expected{seed,trips};expected.Model(true);const auto mxcsr=_mm_getcsr();
        REQUIRE(b->getCode<HaltReason(*)(backend::State*)>()(actual.state)==HaltReason::CallHost);
        REQUIRE(_mm_getcsr()==mxcsr);REQUIRE(actual.bytes==expected.bytes);
    }
}

TEST_CASE("Prepare hot copy declines two incoming edge occurrences", "[x64][cfg][home-reload][entry]") {
    Program p;Fixture(p);
    ir::Inst* branch{};
    for(auto& inst:p.function.body->GetInstList())if(inst.GetOp()==O::Goto){branch=&inst;break;}
    REQUIRE(branch);REQUIRE(p.function.targets.contains(branch));
    auto* target=p.function.targets.at(branch);
    // The explicit taken edge and lexical fallthrough now both reach target.
    // This is valid CFG metadata, but not the pass's one-entry eligibility.
    REQUIRE(p.function.stops.erase(branch)==1);p.function.body->ReIdInstr();Snapshot snapshot{p};
    const auto baseline=x64::CfgRegisterPlan::Build(p.function.body.get(),p.function,Budget());
    REQUIRE(baseline.Validate());
    REQUIRE_FALSE(baseline.Register(p.source.Def()));
    bool witnessed{};
    for(const auto& block:baseline.Blocks())if(baseline.Instructions()[block.begin]==target) {
        REQUIRE(block.reachable);REQUIRE(block.loop_depth>0);REQUIRE(block.incoming_edges.size()==2);witnessed=true;
    }
    REQUIRE(witnessed);
    auto options=x64::FunctionPreparationOptions{};options.scalar_home_reload_split=true;
    auto code=x64::Translator::PrepareFunction(p.function,x64::HostFeatures::Detect(),nullptr,true,true,false,options);
    REQUIRE(code->HomeReloadSplitStats().attempted);REQUIRE_FALSE(code->HomeReloadSplitStats().changed);
    REQUIRE(code->HomeReloadSplitStats().trial.candidate_regions==0);snapshot.RequireSame(p);
    code->Translate();REQUIRE(code->CfgPlan().Validate());
    Machine actual{7,2},expected{7,2};expected.Model();const auto mxcsr=_mm_getcsr();
    REQUIRE(code->getCode<HaltReason(*)(backend::State*)>()(actual.state)==HaltReason::CallHost);
    REQUIRE(_mm_getcsr()==mxcsr);REQUIRE(actual.bytes==expected.bytes);
}
