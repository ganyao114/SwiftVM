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
void Fixture(Program& p, u32 reads = target_reads, bool source_flags = false, bool raw_source = false) {
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
    void Model() {
        const auto value = Get(0)+17;
        const auto trips = Get(8);
        for (u64 trip = 0; trip < trips; ++trip) {
            for (u32 stage = 0; stage < repetitions; ++stage) for (u32 lane = 0; lane < 3; ++lane)
                Put(128+128*lane+8*stage,Get(16+8*lane)+lane+1+31+stage);
            for (u32 index = 0; index < target_reads; ++index) Put(640+8*index,value+71+index);
            Put(8,Get(8)-1);
        }
        Put(928,value);
        state->current_loc = state->prev_loc = ir::Location{0x1000}; state->halt_reason = HaltReason::CallHost;
    }
};
} // namespace

TEST_CASE("CFG spill-home reload copy is confined and profitable", "[x64][cfg][split]") {
    Program p; Fixture(p);
    auto baseline = x64::CfgRegisterPlan::Build(p.function.body.get(),p.function,Budget());
    REQUIRE(baseline.Validate()); REQUIRE_FALSE(baseline.Register(p.source.Def()));
    REQUIRE(baseline.RequiresCanonicalHome(p.source.Def())); REQUIRE_FALSE(baseline.RequiresFullContainer(p.source.Def()));
    const auto old_targets = p.function.targets;
    const auto old_locations = p.function.locations;
    const auto old_exits = p.function.exits;
    const auto old_stops = p.function.stops, old_halts = p.function.halt_checks, old_labels = p.function.synthetic_labels;
    const auto stats = x64::SplitCfgScalarHomeReloads(p.function,{.allocation_budget = Budget()});
    REQUIRE(stats.accepted); REQUIRE(stats.changed); REQUIRE(stats.extra_scalar_copies == 1);
    REQUIRE(stats.rewritten_u64_reads == target_reads); REQUIRE(stats.weighted_home_reads_after < stats.weighted_home_reads_before);
    REQUIRE(stats.estimated_copy_home_reads_added < stats.estimated_region_home_reads_removed);
    REQUIRE(p.function.targets == old_targets); REQUIRE(p.function.locations == old_locations);
    REQUIRE(p.function.exits.size() == old_exits.size()); REQUIRE(p.function.stops == old_stops);
    REQUIRE(p.function.halt_checks == old_halts); REQUIRE(p.function.synthetic_labels == old_labels);
    auto fresh = x64::CfgRegisterPlan::Build(p.function.body.get(),p.function,Budget());
    REQUIRE(fresh.Validate()); REQUIRE_FALSE(fresh.Register(p.source.Def())); REQUIRE(fresh.RequiresCanonicalHome(p.source.Def()));
    u32 copies{};
    for (auto* inst : fresh.Instructions()) if (inst->GetOp() == O::ZeroExtend64) {
        ++copies; REQUIRE(fresh.Register(inst)); REQUIRE_FALSE(fresh.RequiresCanonicalHome(inst));
        for (const auto& block : fresh.Blocks()) {
            REQUIRE(std::find(block.live_in.begin(),block.live_in.end(),inst) == block.live_in.end());
            REQUIRE(std::find(block.live_out.begin(),block.live_out.end(),inst) == block.live_out.end());
        }
    }
    REQUIRE(copies == 1);
}

TEST_CASE("CFG split rejected trial restores body ids uses and metadata", "[x64][cfg][split]") {
    Program p; Fixture(p); Snapshot before{p};
    const auto targets = p.function.targets;
    const auto locations = p.function.locations;
    const auto stats = x64::SplitCfgScalarHomeReloads(p.function,{.allocation_budget = Budget({})});
    REQUIRE(stats.accepted); REQUIRE_FALSE(stats.changed); REQUIRE(stats.attempts > 0); REQUIRE(stats.rejected_home > 0);
    before.RequireSame(p); REQUIRE(p.function.targets == targets); REQUIRE(p.function.locations == locations);
    REQUIRE(x64::CfgRegisterPlan::Build(p.function.body.get(),p.function,Budget({})).Validate());
}

TEST_CASE("CFG split rejects insufficient reads flags metadata and raw source", "[x64][cfg][split]") {
    for (u32 variant = 0; variant < 3; ++variant) {
        Program p; Fixture(p,variant == 0 ? 2 : target_reads,variant == 1,variant == 2); Snapshot before{p};
        const auto stats = x64::SplitCfgScalarHomeReloads(p.function,{.allocation_budget = Budget()});
        REQUIRE(stats.accepted); REQUIRE_FALSE(stats.changed); before.RequireSame(p);
    }
}

TEST_CASE("CFG split plain U64 reads require exact physical shape", "[x64][cfg][split][shape]") {
    for (u32 variant = 0; variant < 6; ++variant) {
        Program p; Fixture(p);
        for (auto& inst : p.function.body->GetInstList()) {
            if (inst.GetOp() != O::Add || !inst.ArgAt(0).IsValue() ||
                inst.GetArg<ir::Value>(0).Def() != p.source.Def()) continue;
            if (variant == 0) inst.SetArg(1,ir::Operand::Op{ir::OperandOp::None,1});
            if (variant == 1) inst.SetArg(1,ir::Operand::Op{ir::OperandOp::Plus,1});
            if (variant == 2) inst.SetArg(2,ir::Imm{u32{71}});
            if (variant == 3) inst.SetArg(4,p.source);
            if (variant == 4) inst.SetArg(3,ir::Flags::All);
            if (variant == 5) inst.SetArg(3,p.source);
        }
        Snapshot before{p};
        const auto stats = x64::SplitCfgScalarHomeReloads(p.function,{.allocation_budget = Budget()});
        REQUIRE(stats.accepted); REQUIRE_FALSE(stats.changed); REQUIRE(stats.candidate_regions == 0);
        before.RequireSame(p);
    }
    for (auto op : {ir::OperandOp::None,ir::OperandOp::Plus}) {
        Program p; Fixture(p);
        for (auto& inst : p.function.body->GetInstList())
            if (inst.GetOp() == O::Add && inst.ArgAt(0).IsValue() &&
                inst.GetArg<ir::Value>(0).Def() == p.source.Def()) inst.SetArg(1,ir::Operand::Op{op,0});
        const auto stats = x64::SplitCfgScalarHomeReloads(p.function,{.allocation_budget = Budget()});
        REQUIRE(stats.accepted); REQUIRE(stats.changed); REQUIRE(stats.rewritten_u64_reads == target_reads);
    }
}

TEST_CASE("CFG hot copy OFF and ON match an independent full state model", "[x64][cfg][split]") {
    Config config{.backend_isa = kX86_64,.uniform_buffer_size = extent,.stack_alignment = 16};
    Program off, on; Fixture(off); Fixture(on);
    const auto stats = x64::SplitCfgScalarHomeReloads(on.function,{.allocation_budget = Budget()});
    REQUIRE(stats.changed);
    auto compile = [&](Program& p) {
        auto code = x64::Translator::PrepareFunction(p.function,x64::HostFeatures::Detect(),&config,true,true,false,
            {.uniform_forwarding = false,.state_promotion = false,.scalar_rematerialization = true,
             .scalar_binary_immediates = true,.loop_weighted_costs = true,
             .profitable_flags_residency = false,.scalar_unary_phases = true,.scalar_unary_coalescing = true,.scalar_shift_immediates = true});
        code->Translate(); REQUIRE(code->CfgPlan().Validate()); return code;
    };
    auto a = compile(off), b = compile(on);
    REQUIRE_FALSE(a->CfgPlan().Register(off.source.Def())); REQUIRE_FALSE(b->CfgPlan().Register(on.source.Def()));
    const auto& a_reads = a->Statistics().At(x64::TransferRegion::CanonicalSSA);
    const auto& b_reads = b->Statistics().At(x64::TransferRegion::CanonicalSSA);
    REQUIRE(b_reads.scalar_load_sites < a_reads.scalar_load_sites);
    REQUIRE(a->Statistics().At(x64::TransferRegion::Uniform).scalar_load_sites == b->Statistics().At(x64::TransferRegion::Uniform).scalar_load_sites);
    REQUIRE(a->Statistics().At(x64::TransferRegion::Uniform).scalar_store_sites == b->Statistics().At(x64::TransferRegion::Uniform).scalar_store_sites);
    for (u64 seed : {u64{0},UINT64_MAX,u64{0x123456789abcdef0}}) for (u64 trips : {u64{1},u64{2},u64{7}}) {
        Machine expected{seed,trips}, first{seed,trips}, second{seed,trips}; expected.Model();
        const auto mxcsr = _mm_getcsr();
        REQUIRE(a->getCode<HaltReason (*)(backend::State*)>()(first.state) == HaltReason::CallHost);
        REQUIRE(_mm_getcsr() == mxcsr);
        REQUIRE(b->getCode<HaltReason (*)(backend::State*)>()(second.state) == HaltReason::CallHost);
        REQUIRE(_mm_getcsr() == mxcsr);
        REQUIRE(first.bytes == expected.bytes); REQUIRE(second.bytes == expected.bytes);
    }
}


TEST_CASE("CFG reload split honors actual GetFlags TestFlags and late flags schemas", "[x64][cfg][split][metadata]") {
    for(u32 variant=0;variant<5;++variant) {
        Program p;Fixture(p);
        ir::Value observer;
        if(variant==0) observer=p.as.GetFlags(p.source,ir::Flags::NZCV).SetType(T::U64);
        if(variant==1) observer=p.as.TestFlags(ir::Flags::NZCV).SetType(T::U8);
        if(variant==2) observer=p.as.TestNotFlags(ir::Flags::Carry).SetType(T::U8);
        if(variant==3) p.as.SetCarry(p.source);
        if(variant==4) p.as.SaveFlags(p.source,ir::Flags::NZCV);
        if(observer.Defined()) p.as.StoreUniform(ir::Uniform{976,observer.Type()},observer);
        // These observers execute in the cold tail, not inside the hot copy
        // region. Valid IR must never turn normal preparation into a throw.
        p.function.body->ReIdInstr();Snapshot before{p};
        x64::CfgLiveRangeSplitStats stats;
        REQUIRE_NOTHROW(stats=x64::SplitCfgScalarHomeReloads(p.function,{.allocation_budget=Budget()}));
        REQUIRE(stats.accepted);
        if(variant==0||variant==3||variant==4){REQUIRE_FALSE(stats.changed);before.RequireSame(p);}
        auto code=x64::Translator::PrepareFunction(p.function,x64::HostFeatures::Detect(),nullptr,true,true,true);
        REQUIRE_NOTHROW(code->Translate());REQUIRE(code->CfgPlan().Validate());
        if(variant<=2) {
            Machine actual{7,2},expected{7,2};expected.Model();
            if(variant==0)expected.Put(976,flags_seed);
            else expected.state->uniform_buffer_begin[976]=u8(variant==1);
            REQUIRE(code->getCode<HaltReason(*)(backend::State*)>()(actual.state)==HaltReason::CallHost);
            REQUIRE(actual.bytes==expected.bytes);
        }
    }
}
