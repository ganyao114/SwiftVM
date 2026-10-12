#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
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



using Clock=std::chrono::steady_clock;
using Fn=HaltReason(*)(backend::State*);
namespace {
constexpr const char* route_names[]={"local_off","local_on","cfg_off","cfg_on","resident_off","resident_on","profitable_off","profitable_on"};
constexpr const char* scope="synthetic_lowered_cfg_retained_home_reload";
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
u64 Hash(const void* data,size_t bytes) {
    u64 hash=14695981039346656037ULL;const auto* input=static_cast<const u8*>(data);
    for(size_t i=0;i<bytes;++i){hash^=input[i];hash*=1099511628211ULL;}return hash;
}
u64 Hash(const std::string& s){return Hash(s.data(),s.size());}
double Median(std::vector<double> samples){Check(!samples.empty(),"empty samples");std::sort(samples.begin(),samples.end());return samples[samples.size()/2];}
struct Compiled {
    Config config{.backend_isa=kX86_64,.uniform_buffer_size=extent,.stack_alignment=16};
    Program program;
    std::unique_ptr<x64::Translator> code;
    u32 route{},copy_count{};
    u64 input_ir_hash{},output_ir_hash{},before_weighted_reads{},after_weighted_reads{},code_hash{};
    bool source_assigned{},source_retained{},copy_assigned{},copy_register_only{},copy_local{};
    double prepare_emit_us{};
    Fn Entry()const{return code->getCode<Fn>();}
};
std::unique_ptr<Compiled> Compile(u32 route) {
    auto unit=std::make_unique<Compiled>();unit->route=route;
    Fixture(unit->program,target_reads,false,false,true);unit->program.function.body->ReIdInstr();
    auto& p=unit->program;unit->input_ir_hash=Hash(p.function.body->ToString());
    const bool cfg=route>=2,requested=route>=4;
    auto options=x64::FunctionPreparationOptions{};
    options.scalar_home_reload_split=bool(route&1);options.profitable_flags_residency=route>=6;
    const auto historical=x64::CfgRegisterPlan::Build(p.function.body.get(),p.function,
        x64::Translator::AllocationBudget(p.function.body.get(),&p.function,requested));
    Check(historical.Validate(),"historical whole CFG invalid");
    const auto historical_frame=x64::SparseFramePlan::Build(p.function.body.get(),&p.function,historical,{});
    Check(historical_frame.Enabled(),"historical frame invalid");
    unit->before_weighted_reads=x64::CfgHomeReloadPreparation::WeightedHomeReads(historical,historical_frame);
    const auto begin=Clock::now();
    unit->code=x64::Translator::PrepareFunction(p.function,x64::HostFeatures::Detect(),&unit->config,true,cfg,requested,options);
    unit->code->Translate();
    unit->prepare_emit_us=std::chrono::duration<double,std::micro>(Clock::now()-begin).count();
    unit->output_ir_hash=Hash(p.function.body->ToString());unit->code_hash=Hash(unit->code->getCode(),unit->code->getSize());
    const auto& final=unit->code->CfgPlan();const auto& frame=unit->code->FramePlan();
    if(cfg) {
        Check(final.Validate(),"emitted whole CFG invalid");
        unit->after_weighted_reads=x64::CfgHomeReloadPreparation::WeightedHomeReads(final,frame);
        unit->source_assigned=final.Register(p.source.Def()).has_value();
        unit->source_retained=final.RequiresCanonicalHome(p.source.Def())&&frame.Home(p.source.Def()).has_value()&&
            frame.Decision(p.source.Def())==x64::SparseHomeDecision::Retained&&!frame.RematerializedValue(p.source.Def());
        for(auto* inst:final.Instructions())if(inst->GetOp()==O::ZeroExtend64) {
            ++unit->copy_count;Check(inst->GetArg<ir::Value>(0).Def()==p.source.Def(),"copy names a different source");
            unit->copy_assigned=final.Register(inst).has_value();
            unit->copy_register_only=!final.RequiresCanonicalHome(inst)&&!frame.Home(inst)&&
                frame.Decision(inst)==x64::SparseHomeDecision::RegisterOnly;
            unit->copy_local=true;
            for(const auto& block:final.Blocks())if(std::find(block.live_in.begin(),block.live_in.end(),inst)!=block.live_in.end()||
                std::find(block.live_out.begin(),block.live_out.end(),inst)!=block.live_out.end())unit->copy_local=false;
        }
    }
    return unit;
}
void Validate(Compiled& unit) {
    for(u64 seed:{u64{0},UINT64_MAX,u64{0x123456789abcdef0}})for(u64 trips:{u64{1},u64{2},u64{7},u64{19}}) {
        Machine actual{seed,trips},expected{seed,trips};expected.Model(true);const auto mxcsr=_mm_getcsr();
        Check(unit.Entry()(actual.state)==HaltReason::CallHost,"native exit mismatch");
        Check(_mm_getcsr()==mxcsr,"MXCSR changed");Check(actual.bytes==expected.bytes,"independent full State mismatch");
    }
}
void Prove(const std::array<std::unique_ptr<Compiled>,8>& units) {
    for(u32 route=0;route<8;route+=2) {
        const auto& off=*units[route];const auto& on=*units[route+1];
        const auto& a=*off.code;const auto& b=*on.code;const auto& stats=b.HomeReloadSplitStats();
        Check(off.input_ir_hash==units[0]->input_ir_hash&&on.input_ir_hash==off.input_ir_hash,"input IR changed across policy routes");
        Check(a.FlagsResident()==b.FlagsResident(),"split changed final flags choice");
        const auto& ua=a.Statistics().At(x64::TransferRegion::Uniform);const auto& ub=b.Statistics().At(x64::TransferRegion::Uniform);
        Check(ua.scalar_load_sites==ub.scalar_load_sites&&ua.scalar_store_sites==ub.scalar_store_sites&&
              ua.load_bytes==ub.load_bytes&&ua.store_bytes==ub.store_bytes,"Uniform census changed");
        if(stats.changed) {
            Check(!on.source_assigned&&on.source_retained&&on.copy_count==1&&on.copy_assigned&&on.copy_register_only&&on.copy_local,"final copy/source/home proof failed");
            Check(on.after_weighted_reads<on.before_weighted_reads,"whole weighted retained-home reads did not decrease");
            Check(on.after_weighted_reads==stats.final_weighted_home_reads,"final statistics mismatch");
            Check(b.Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_load_sites<
                  a.Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_load_sites,"actual canonical read sites did not decrease");
            Check(stats.final_allocation_builds==1,"accepted copy lacks the final whole allocation");
        } else {
            Check(on.output_ir_hash==off.output_ir_hash,"unaccepted option changed body");
            Check(b.getSize()==a.getSize()&&std::memcmp(b.getCode(),a.getCode(),a.getSize())==0,"unaccepted option changed native code");
        }
        if(route==0)Check(!stats.attempted&&stats.reason==x64::CfgHomeReloadPreparationReason::UnsupportedFlow,"local control attempted a CFG rewrite");
        if(route==2||route==4)Check(stats.changed,"pressure positive control did not accept a copy");
        std::cerr<<"home_reload_pair pair="<<route_names[route]<<" input_ir_equal=1 final_flags_equal=1 uniform_census_equal=1 changed="<<stats.changed
            <<" rolled_back="<<stats.rolled_back<<" reason="<<unsigned(stats.reason)<<" source_retained="<<on.source_retained<<" source_assigned="<<on.source_assigned
            <<" copy_count="<<on.copy_count<<" copy_assigned="<<on.copy_assigned<<" copy_register_only="<<on.copy_register_only<<" copy_local="<<on.copy_local
            <<" weighted_before="<<on.before_weighted_reads<<" weighted_after="<<on.after_weighted_reads<<" ssa_loads_off="
            <<a.Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_load_sites<<" ssa_loads_on="
            <<b.Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_load_sites<<" split_builds="<<stats.trial.full_allocation_builds
            <<" final_builds="<<stats.final_allocation_builds<<" prep_builds="<<b.PreparationStats().full_allocation_builds
            <<" policy_builds="<<b.PreparationStats().flags_policy_allocation_builds<<" policy_recolors="<<b.PreparationStats().flags_policy_allocation_recolors
            <<" discarded_policy_builds="<<stats.discarded_flags_allocation_builds<<" discarded_policy_recolors="<<stats.discarded_flags_allocation_recolors
            <<" code_equal="<<(!stats.changed)<<" code_bytes_off="<<a.getSize()<<" code_bytes_on="<<b.getSize()<<'\n';
    }
}
std::array<double,8> Measure(const std::array<std::unique_ptr<Compiled>,8>& units,u32 calls,u32 trips) {
    std::array<std::unique_ptr<Machine>,8> machines;std::array<std::vector<double>,8> samples;
    for(u32 r=0;r<8;++r){machines[r]=std::make_unique<Machine>(0x123456789abcdef0,trips);
        for(u32 warm=0;warm<32;++warm){machines[r]->Put(8,trips);Check(units[r]->Entry()(machines[r]->state)==HaltReason::CallHost,"warm exit mismatch");}}
    for(u32 repeat=0;repeat<7;++repeat)for(u32 order=0;order<8;++order) {
        const u32 r=(repeat+order)%8;auto& machine=*machines[r];const auto entry=units[r]->Entry();
        std::cerr<<"rotation phase=native repeat="<<repeat<<" order="<<order<<" route="<<route_names[r]<<'\n';
        const auto begin=Clock::now();
        for(u32 c=0;c<calls;++c){machine.Put(8,trips);machine.state->halt_reason=HaltReason::None;
            Check(entry(machine.state)==HaltReason::CallHost,"timed exit mismatch");}
        samples[r].push_back(std::chrono::duration<double,std::nano>(Clock::now()-begin).count()/calls);
    }
    std::array<double,8> result{};for(u32 r=0;r<8;++r)result[r]=Median(samples[r]);return result;
}
u32 Argument(const char* s){size_t consumed{};const auto n=std::stoull(s,&consumed);Check(consumed==std::strlen(s)&&n&&n<=UINT32_MAX,"calls/trips require positive uint32");return u32(n);}
}
int main(int argc,char** argv) {
    try {
        const bool only=argc==2&&std::string{argv[1]}=="--validate-only";
        Check(only||argc<=3,"usage: benchmark [calls [trips]] | --validate-only");
        const u32 calls=only||argc<2?500:Argument(argv[1]),trips=only||argc<3?64:Argument(argv[2]);
        std::cerr<<"scope="<<scope<<"; synthetic_lowered_ir=1; decoded_a64=0; unicorn_equivalence=0; assigned_range_release_claim=0; "
            "whole_weighted_home_reads=structural_estimate; memory_census=partial_static_sites; application_performance_claim=0\n";
        std::array<std::unique_ptr<Compiled>,8> units;std::array<std::vector<double>,8> preparation;
        if(!only)for(u32 repeat=0;repeat<9;++repeat)for(u32 order=0;order<8;++order) {
            const u32 r=(repeat+order)%8;
            std::cerr<<"rotation phase=preparation repeat="<<repeat<<" order="<<order<<" route="<<route_names[r]<<'\n';
            auto sample=Compile(r);preparation[r].push_back(sample->prepare_emit_us);
        }
        for(u32 r=0;r<8;++r){units[r]=Compile(r);Validate(*units[r]);}Prove(units);
        std::cerr<<"validated_routes=8; independent_full_state_cases=96; mxcsr_preserved=1; input_ir_equal=1; "
            "pressure_positives=2; unchanged_uniform_census=1; unaccepted_pair_code_equal=1\n";
        if(only)return 0;
        const auto ns=Measure(units,calls,trips);
        std::cout<<"scope,route,cfg_allocation,flags_requested,profitable_flags,flags_selected,split_requested,split_attempted,split_changed,split_rolled_back,split_reason,"
            "input_ir_hash,output_ir_hash,code_hash,physical_registers,assigned_values,source_assigned,source_retained,copy_count,copy_assigned,copy_register_only,copy_local,"
            "weighted_home_reads_before,weighted_home_reads_after,physical_home_slots,retained_home_slots,home_bytes,frame_bytes,code_bytes,ssa_load_sites,ssa_store_sites,ssa_load_bytes,ssa_store_bytes,"
            "uniform_load_sites,uniform_store_sites,uniform_load_bytes,uniform_store_bytes,flags_state_load_sites,flags_state_store_sites,split_cfg_builds,split_final_cfg_builds,split_attempts,split_rejected_allocation,split_rejected_home,split_rejected_escape,split_rejected_cost,split_rewritten_reads,discarded_flags_builds,discarded_flags_recolors,preparation_cfg_builds,flags_policy_builds,flags_policy_recolors,all_preparation_cfg_builds,reused_final_allocation,"
            "full_state_verified,trips,calls,prepare_emit_us,native_ns_per_call,native_ns_per_loop_iteration\n";
        for(u32 r=0;r<8;++r) {
            const auto& u=*units[r];const auto& c=*u.code;const auto& cfg=c.CfgPlan();const auto& f=c.FramePlan().Statistics();const auto& st=c.Statistics();const auto& split=c.HomeReloadSplitStats();
            const auto& ssa=st.At(x64::TransferRegion::CanonicalSSA);const auto& uniform=st.At(x64::TransferRegion::Uniform);
            const auto assigned=std::count_if(cfg.Allocations().begin(),cfg.Allocations().end(),[](const auto& x){return x.slot.has_value();});
            std::cout<<std::fixed<<std::setprecision(3)<<scope<<','<<route_names[r]<<','<<(r>=2)<<','<<(r>=4)<<','<<(r>=6)<<','<<c.FlagsResident()<<','<<bool(r&1)
                <<','<<split.attempted<<','<<split.changed<<','<<split.rolled_back<<','<<unsigned(split.reason)<<','<<u.input_ir_hash<<','<<u.output_ir_hash<<','<<u.code_hash
                <<','<<cfg.PhysicalRegisters().size()<<','<<assigned<<','<<u.source_assigned<<','<<u.source_retained<<','<<u.copy_count<<','<<u.copy_assigned<<','<<u.copy_register_only<<','<<u.copy_local
                <<','<<u.before_weighted_reads<<','<<u.after_weighted_reads<<','<<f.home_count<<','<<f.retained_homes<<','<<f.home_bytes<<','<<c.FrameSize()<<','<<c.getSize()
                <<','<<ssa.scalar_load_sites+ssa.vector_load_sites<<','<<ssa.scalar_store_sites+ssa.vector_store_sites<<','<<ssa.load_bytes<<','<<ssa.store_bytes
                <<','<<uniform.scalar_load_sites+uniform.vector_load_sites<<','<<uniform.scalar_store_sites+uniform.vector_store_sites<<','<<uniform.load_bytes<<','<<uniform.store_bytes
                <<','<<st.flags_state_load_sites<<','<<st.flags_state_store_sites<<','<<split.trial.full_allocation_builds
                <<','<<split.final_allocation_builds<<','<<split.trial.attempts<<','<<split.trial.rejected_allocation<<','<<split.trial.rejected_home
                <<','<<split.trial.rejected_escape<<','<<split.trial.rejected_cost<<','<<split.trial.rewritten_u64_reads
                <<','<<split.discarded_flags_allocation_builds<<','<<split.discarded_flags_allocation_recolors<<','<<c.PreparationStats().full_allocation_builds
                <<','<<c.PreparationStats().flags_policy_allocation_builds<<','<<c.PreparationStats().flags_policy_allocation_recolors
                <<','<<c.PreparationStats().full_allocation_builds+c.PreparationStats().flags_policy_allocation_builds<<','<<c.PreparationStats().reused_final_allocation
                <<",1,"<<trips<<','<<calls<<','<<Median(preparation[r])<<','<<ns[r]<<','<<ns[r]/trips<<'\n';
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<"home reload benchmark: "<<e.what()<<'\n';return 1;}
}
