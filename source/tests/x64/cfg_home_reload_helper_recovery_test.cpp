#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <xmmintrin.h>
#include <iterator>
#include <unordered_set>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
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




namespace {
constexpr u64 replacement_flags=0xa17bcdef23456789ULL;
constexpr u64 helper_mutation=0x53504c495448454cULL,helper_return=0x9988776655443322ULL;
struct HelperProbe { backend::State* state{};u64 calls{},observed{}; };
__attribute__((noinline)) u64 ClobberAfterHotCopy(u64 probe_pointer,u64 address,u64) {
    auto& probe=*reinterpret_cast<HelperProbe*>(probe_pointer);
    ++probe.calls;probe.observed=probe.state->host_cpu_flags;
    probe.state->host_cpu_flags=replacement_flags;
    std::memcpy(probe.state->uniform_buffer_begin+992,&helper_mutation,8);
    // Real SysV caller-saved clobbers; each live assigned scalar still owns
    // its original callee-saved register/home. Returning calls preserve RBX.
    asm volatile("pcmpeqd %%xmm0,%%xmm0\n\tpcmpeqd %%xmm1,%%xmm1\n\t"
                 "pcmpeqd %%xmm8,%%xmm8\n\tpcmpeqd %%xmm15,%%xmm15\n\t"
                 "movabs $0xabcddcba12344321,%%r10\n\txor %%r11d,%%r11d"
                 :::"xmm0","xmm1","xmm8","xmm15","r10","r11","cc","memory");
    // On PROT_NONE this abandons a helper with a temporary RBX. Production
    // recovery must reload the already committed packed word from State.
    u64 result;
    asm volatile("mov %%rbx,%%r11\n\tmovabs $0xdeadbeef98765432,%%rbx\n\t"
                 "mov (%%rsi),%%rax\n\tmov %%r11,%%rbx"
                 :"=a"(result):"S"(address):"r11","memory");
    return result;
}
void HelperFixture(Program& p) {
    Fixture(p,target_reads,false,false,true);
    ir::Inst* latch{};u32 labels{};
    for(auto& inst:p.function.body->GetInstList())if(inst.GetOp()==O::BindLabel&&++labels==3){latch=&inst;break;}
    REQUIRE(latch);
    std::unordered_set<ir::Inst*> original;
    for(auto& inst:p.function.body->GetInstList())original.insert(&inst);
    auto probe=p.Load(80),address=p.Load(88);
    auto result=p.as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&ClobberAfterHotCopy)}},probe,address,probe).SetType(T::U64);
    p.Store(1000,result);
    auto after=p.as.TestFlags(ir::Flags::NZCV).SetType(T::U8);
    p.as.StoreUniform(ir::Uniform{984,T::U8},after);
    std::vector<ir::Inst*> appended;
    for(auto& inst:p.function.body->GetInstList())if(!original.contains(&inst))appended.push_back(&inst);
    auto anchor=std::next(p.function.body->GetInstList().iterator_to(*latch));
    REQUIRE(anchor!=p.function.body->GetInstList().end());auto* before=&*anchor;
    for(auto* inst:appended){p.function.body->RemoveInst(inst);p.function.body->InsertBefore(inst,before);}
    p.function.body->ReIdInstr();
}
std::unique_ptr<x64::Translator> CompileHelper(Program& p,const Config& config,bool split,bool resident) {
    auto options=x64::FunctionPreparationOptions{};options.scalar_home_reload_split=split;
    auto code=x64::Translator::PrepareFunction(p.function,x64::HostFeatures::Detect(),&config,true,true,resident,options);
    REQUIRE(code->CfgPlan().Validate());REQUIRE(code->FlagsResident()==resident);
    REQUIRE_FALSE(code->CfgPlan().Register(p.source.Def()));REQUIRE(code->FramePlan().Home(p.source.Def()));
    REQUIRE(code->FramePlan().Decision(p.source.Def())==x64::SparseHomeDecision::Retained);
    if(split) {
        // Mandatory actual witness, never a SKIP fallback to a weaker case.
        REQUIRE(code->HomeReloadSplitStats().changed);REQUIRE_FALSE(code->HomeReloadSplitStats().rolled_back);
        REQUIRE(code->HomeReloadSplitStats().final_weighted_home_reads<code->HomeReloadSplitStats().trial.weighted_home_reads_before);
        u32 copies{};
        for(auto* inst:code->CfgPlan().Instructions())if(inst->GetOp()==O::ZeroExtend64) {
            ++copies;REQUIRE(code->CfgPlan().Register(inst));REQUIRE_FALSE(code->FramePlan().Home(inst));
            REQUIRE(code->FramePlan().Decision(inst)==x64::SparseHomeDecision::RegisterOnly);
            for(const auto& block:code->CfgPlan().Blocks()) {
                REQUIRE(std::find(block.live_in.begin(),block.live_in.end(),inst)==block.live_in.end());
                REQUIRE(std::find(block.live_out.begin(),block.live_out.end(),inst)==block.live_out.end());
            }
        }
        REQUIRE(copies==1);
    }
    code->Translate();REQUIRE(code->Statistics().helper_call_sites>0);return code;
}
void HelperModel(Machine& expected,bool accessible,bool runtime_boundary) {
    const auto source=expected.Get(0)+17,trips=expected.Get(8);
    const u64 completed=accessible?trips:1;
    for(u64 trip=0;trip<completed;++trip) {
        for(u32 stage=0;stage<repetitions;++stage)for(u32 lane=0;lane<3;++lane)
            expected.Put(128+128*lane+8*stage,expected.Get(16+8*lane)+lane+1+31+stage);
        for(u32 i=0;i<target_reads;++i)expected.Put(640+8*i,source+71+i);
        expected.Put(992,helper_mutation);expected.state->host_cpu_flags=replacement_flags;
        if(accessible){expected.Put(1000,helper_return);expected.state->uniform_buffer_begin[984]=1;expected.Put(8,expected.Get(8)-1);}
    }
    expected.state->uniform_buffer_begin[976]=1;
    if(accessible)expected.Put(928,source);
    expected.state->current_loc=expected.state->prev_loc=ir::Location{0x1000};
    expected.state->halt_reason=runtime_boundary?HaltReason::None:HaltReason::CallHost;
    expected.state->spill_area[backend::kRiscvRecoveryPcSlot]=0;
    expected.state->spill_area[backend::kRiscvRecoveryFrameSlot]=0;
    expected.state->spill_area[backend::kRiscvMemoryOwnedSlot]=0;
}
// Publish the exact already-proved FunctionPlan Translator bytes into the
// real Module arena. TranslateIR(Block) would recompile without this owned
// lowered CFG/split policy, so it cannot serve as this feature's witness.
void PublishPrepared(const std::shared_ptr<backend::Module>& module,Program& p,const x64::Translator& code) {
    p.function.body->SetEndLocation(ir::Location{0x1001});REQUIRE(module->Push(p.function.body.get()));
    auto [id,buffer]=module->AllocCodeCache(code.getSize());REQUIRE(id!=backend::INVALID_CACHE_ID);
    std::memcpy(buffer.rw_data,code.getCode(),code.getSize());buffer.Flush();
    auto& cached=p.function.body->GetJitCache();cached.cache_id=id;cached.offset_in=buffer.offset;
    cached.cache_size=buffer.size;cached.jit_state=backend::JitState::Cached;
    module->AddFaultEntry(buffer.exec_data,buffer.exec_data+buffer.size,0x1000,buffer.exec_data,buffer.exec_data+code.RecoveryOffset());
    module->RetainCodeIR(buffer.exec_data,p.function.body.get());
    module->GetAddressSpace().PushCodeCache(ir::Location{0x1000},buffer.exec_data);
    REQUIRE(module->GetAddressSpace().GetCodeCache(ir::Location{0x1000})==buffer.exec_data);
}
} // namespace

TEST_CASE("accepted hot copy preserves full State across returning helper clobbers", "[x64][cfg][home-reload][helper][execution]") {
    Config config{.backend_isa=kX86_64,.uniform_buffer_size=extent,.stack_alignment=16};
    for(bool resident:{false,true}) {
        Program off,on;HelperFixture(off);HelperFixture(on);
        auto a=CompileHelper(off,config,false,resident),b=CompileHelper(on,config,true,resident);
        REQUIRE(b->Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_load_sites<a->Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_load_sites);
        for(u64 seed:{u64{0},UINT64_MAX,u64{0x123456789abcdef0}})for(u64 trips:{u64{1},u64{2},u64{7}}) {
            for(auto* code:{a.get(),b.get()}) {
                Machine actual{seed,trips},expected{seed,trips};u64 address_value=helper_return;
                HelperProbe probe{.state=actual.state};actual.Put(80,reinterpret_cast<u64>(&probe));actual.Put(88,reinterpret_cast<u64>(&address_value));
                std::memcpy(expected.bytes.data(),actual.bytes.data(),actual.bytes.size());HelperModel(expected,true,false);
                const auto mxcsr=_mm_getcsr();REQUIRE(code->getCode<HaltReason(*)(backend::State*)>()(actual.state)==HaltReason::CallHost);
                REQUIRE(_mm_getcsr()==mxcsr);REQUIRE(probe.calls==trips);REQUIRE(probe.observed==(trips==1?flags_seed:replacement_flags));
                REQUIRE(actual.bytes==expected.bytes);
            }
        }
    }
}

TEST_CASE("accepted split Runtime restores abandoned helper frames through NONE RW NONE", "[x64][cfg][home-reload][helper][fault][runtime]") {
    struct Mapping {size_t size=size_t(getpagesize());void* page=mmap(nullptr,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        ~Mapping(){if(page!=MAP_FAILED)munmap(page,size);}} mapping;REQUIRE(mapping.page!=MAP_FAILED);
    for(bool resident:{false,true})for(bool split:{false,true}) {
        Config config{.loc_start=0,.loc_end=0x10000,.enable_jit=true,.backend_isa=kX86_64,
            .uniform_buffer_size=extent,.static_program=true,.stack_alignment=16};
        backend::AddressSpace space{config};backend::ModuleConfig policy;policy.read_only=true;
        auto module=space.MapModule(0x1000,0x2000,policy);Runtime runtime{&space};Program p;HelperFixture(p);
        auto code=CompileHelper(p,config,split,resident);PublishPrepared(module,p,*code);
        auto* state=runtime.GetState();state->pt=nullptr;state->guest_addr_limit=state->guest_addr_mask=UINT64_MAX;
        state->interp_range_check=nullptr;state->interp_range_check_ctx=nullptr;
        u64 total_calls{};
        for(bool accessible:{false,true,false}) {
            REQUIRE(mprotect(mapping.page,mapping.size,accessible?PROT_READ|PROT_WRITE:PROT_NONE)==0);
            if(accessible)std::memcpy(mapping.page,&helper_return,8);
            Machine initial{0x123456789abcdef0,2},expected{0,1};HelperProbe probe{.state=state};
            initial.Put(80,reinterpret_cast<u64>(&probe));initial.Put(88,reinterpret_cast<u64>(mapping.page));
            std::memcpy(runtime.GetUniformBuffer().data(),initial.state->uniform_buffer_begin,extent);
            state->host_cpu_flags=flags_seed;runtime.SetLocation(0x1000);
            std::memcpy(expected.bytes.data(),state,expected.bytes.size());HelperModel(expected,accessible,true);
            const auto mxcsr=_mm_getcsr();REQUIRE(runtime.Run()==(accessible?HaltReason::CallHost:HaltReason::PageFatal));
            REQUIRE(_mm_getcsr()==mxcsr);REQUIRE(probe.calls==(accessible?2:1));total_calls+=probe.calls;
            REQUIRE(probe.observed==(accessible?replacement_flags:flags_seed));
            REQUIRE(std::memcmp(state,expected.bytes.data(),expected.bytes.size())==0);
            REQUIRE(state->spill_area[backend::kRiscvRecoveryPcSlot]==0);REQUIRE(state->spill_area[backend::kRiscvRecoveryFrameSlot]==0);
            REQUIRE(state->spill_area[backend::kRiscvMemoryOwnedSlot]==0);
        }
        REQUIRE(total_calls==4);
    }
}
