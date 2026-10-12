#include <array>
#include <cstring>
#include <new>
#include <vector>
#include <sys/mman.h>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/backend/x64/cfg_uniform_dead_store_pc.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/frontend/arm64/arm64_frontend.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
constexpr u32 extent = 512;
constexpr u64 flags_seed = 0xfedcba9876543219ULL;
Config Configuration() {
    return {.loc_start = 0,.loc_end = 0x10000,.enable_jit = true,.backend_isa = kX86_64,
            .uniform_buffer_size = extent,.static_program = true,.stack_alignment = 16};
}
struct Program {
    backend::FunctionPlan function;
    ir::Assembler as;
    Program() : as((function.body = new ir::Block(ir::Location{0x1000})).get()) {}
    ir::Value Load(u32 at) { return as.LoadUniform(ir::Uniform{at,T::U64}).SetType(T::U64); }
    ir::Inst* Store(u32 at, ir::Value value) { return as.AppendInst(O::StoreUniform,ir::Uniform{at,T::U64},value.SetCastType(T::U64)); }
    ir::Inst* PC(u32 amount = 4) { return as.AppendInst(O::AdvancePC,ir::Imm{amount}); }
    void Finish() { function.body->SetTerminal(ir::terminal::ReturnToHost{}); }
    void Exit() {
        auto* marker = as.AppendInst(O::Nop); function.stops.insert(marker);
        function.exits.emplace(marker,ir::terminal::ReturnToHost{}); Finish();
    }
    x64::CfgUniformDeadStorePcStats Pass(bool enabled) {
        return x64::EliminateOverwrittenUniformStoresAcrossPC(function,Configuration(),x64::ExecutionMode::Ordinary,{.enabled = enabled});
    }
};
std::vector<ir::Inst*> Members(Program& p) {
    std::vector<ir::Inst*> values; for (auto& inst : p.function.body->GetInstList()) values.push_back(&inst); return values;
}
struct Machine {
    alignas(backend::State) std::array<u8,sizeof(backend::State)+extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() { for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i*37+13); state->host_cpu_flags = flags_seed; }
    ~Machine() { state->~State(); }
    void Put(u32 at, u64 value) { std::memcpy(state->uniform_buffer_begin+at,&value,8); }
    void Complete(u64 pc = 0x1000, HaltReason reason = HaltReason::CallHost) {
        state->current_loc = ir::Location{pc}; state->prev_loc = ir::Location{0x1000}; state->halt_reason = reason;
    }
};
struct PoisonCall : Xbyak::CodeGenerator {
    explicit PoisonCall(x64::Translator& code) : Xbyak::CodeGenerator(4096) {
        const u32 bytes = (code.FrameSize()+4096+15)&~u32{15};
        mov(rdx,rdi); mov(r11,rsp); sub(rsp,bytes); mov(rdi,rsp); mov(rcx,bytes/8);
        mov(rax,0xa5e39b7142c68df0ULL); cld(); rep(); stosq(); mov(rsp,r11); mov(rdi,rdx);
        mov(rax,reinterpret_cast<u64>(code.getCode())); jmp(rax);
    }
    void Run(Machine& machine, HaltReason reason = HaltReason::CallHost) {
        const auto mxcsr = _mm_getcsr(); REQUIRE(getCode<HaltReason (*)(backend::State*)>()(machine.state) == reason);
        REQUIRE(_mm_getcsr() == mxcsr);
    }
};
std::unique_ptr<x64::Translator> Compile(Program& p, const Config& config, bool cfg = true) {
    auto code = x64::Translator::PrepareFunction(p.function,{},&config,true,cfg,false,
        {.uniform_forwarding = false,.state_promotion = false});
    code->Translate(); if (cfg) REQUIRE(code->CfgPlan().Validate()); return code;
}
u64 Observe(u64 address, u64, u64) {
    u64 value; std::memcpy(&value,reinterpret_cast<void*>(address),8);
    const auto* state = reinterpret_cast<const backend::State*>(reinterpret_cast<const u8*>(address)-128-backend::state_offset_uniform_buffer);
    return value ^ state->current_loc.Value();
}
} // namespace

TEST_CASE("cross PC store elimination is disabled by default and ordinary x64 JIT only", "[x64][uniform-dead-store-pc][policy]") {
    const auto config = Configuration();
    for (u32 route = 0; route < 4; ++route) {
        Program p; auto a = p.Load(0), b = p.Load(8); auto* first = p.Store(128,a); p.PC(); auto* final = p.Store(128,b); p.Finish();
        const auto before = Members(p); const auto uses_a = a.Def()->GetUses(false), uses_b = b.Def()->GetUses(false);
        auto selected = config; if (route == 2) selected.enable_jit = false; if (route == 3) selected.backend_isa = kArm64;
        const auto stats = x64::EliminateOverwrittenUniformStoresAcrossPC(p.function,selected,
            route == 1 ? x64::ExecutionMode::SingleInstruction : x64::ExecutionMode::Ordinary,{.enabled = route != 0});
        REQUIRE(stats.attempted == (route != 0)); REQUIRE_FALSE(stats.accepted); REQUIRE(stats.stores_removed == 0);
        REQUIRE(stats.rejection == (route == 0 ? x64::CfgUniformDeadStorePcRejection::Disabled : x64::CfgUniformDeadStorePcRejection::UnsupportedMode));
        REQUIRE(Members(p) == before); REQUIRE(first->GetOp() == O::StoreUniform); REQUIRE(final->GetOp() == O::StoreUniform);
        REQUIRE(a.Def()->GetUses(false) == uses_a); REQUIRE(b.Def()->GetUses(false) == uses_b);
    }
    Program p; auto value = p.Load(0); p.Store(128,value); p.PC(); p.Store(128,value); p.Finish();
    REQUIRE_FALSE(x64::EliminateOverwrittenUniformStoresAcrossPC(p.function,config,x64::ExecutionMode::Ordinary).attempted);
}

TEST_CASE("cross PC native stores preserve exact PC markers final publication and complete State", "[x64][uniform-dead-store-pc][native]") {
    const auto config = Configuration(); Program off, on; std::array<ir::Inst*,3> pcs; ir::Inst* first{}; ir::Inst* final{}; ir::Inst* source{};
    auto build = [&](Program& p, bool record) {
        auto a = p.Load(0), b = p.Load(8); auto* old = p.Store(128,a); auto* pc1 = p.PC(4); auto* pc2 = p.PC(8);
        auto changed = p.as.Neg(b).SetType(T::U64); auto* last = p.Store(128,changed); auto* pc3 = p.PC(4); p.Exit();
        if (record) { first = old; final = last; source = a.Def(); pcs = {pc1,pc2,pc3}; }
    };
    build(off,false); build(on,true); const auto members = Members(on); const auto uses = source->GetUses(false);
    const std::array ids{pcs[0]->Id(),pcs[1]->Id(),pcs[2]->Id()};
    const std::array amounts{pcs[0]->GetArg<ir::Imm>(0).Get(),pcs[1]->GetArg<ir::Imm>(0).Get(),pcs[2]->GetArg<ir::Imm>(0).Get()};
    REQUIRE_FALSE(off.Pass(false).attempted); const auto stats = on.Pass(true);
    REQUIRE(stats.accepted); REQUIRE(stats.stores_removed == 1); REQUIRE(stats.removals_crossing_pc == 1); REQUIRE(stats.pure_advance_pc_sites == 3);
    REQUIRE(Members(on) == members); REQUIRE(first->GetOp() == O::Nop); REQUIRE(final->GetOp() == O::StoreUniform);
    REQUIRE(source->GetUses(false) == uses-1);
    for (u32 i = 0; i < pcs.size(); ++i) { REQUIRE(pcs[i]->GetOp() == O::AdvancePC); REQUIRE(pcs[i]->Id() == ids[i]); REQUIRE(pcs[i]->GetArg<ir::Imm>(0).Get() == amounts[i]); }
    auto a = Compile(off,config), b = Compile(on,config); REQUIRE(a->Statistics().At(x64::TransferRegion::Uniform).scalar_store_sites == 2);
    REQUIRE(b->Statistics().At(x64::TransferRegion::Uniform).scalar_store_sites == 1); REQUIRE(b->getSize() < a->getSize()); PoisonCall old_call{*a}, new_call{*b};
    for (u64 seed : {u64{0},UINT64_MAX,u64{0x8123456789abcdef}}) {
        const u64 value = seed ^ 0x13579bdf2468ace0ULL; Machine baseline, actual, expected;
        for (auto* machine : {&baseline,&actual,&expected}) { machine->Put(0,seed); machine->Put(8,value); }
        expected.Put(128,0-value); expected.Complete(0x1010); old_call.Run(baseline); new_call.Run(actual);
        REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
    }
}

TEST_CASE("decoded A64 W and X overwrites eliminate one cross PC store without changing complete State", "[x64][uniform-dead-store-pc][decoded][native]") {
    class CodeMemory final : public MemoryInterface {
    public:
        std::array<u32,3> words;
        explicit CodeMemory(bool wide, bool alias = false) : words{wide ? 0x91000420u : 0x11000420u,
            alias ? (wide ? 0x91000800u : 0x11000800u) : (wide ? 0x91000840u : 0x11000840u),0x14000001u} {}
        // ADD X0,X1,#1 / ADD X0,X2,#2 / B 0x100c; W variants zero-extend.
        bool Read(void* destination, size_t address, size_t size) override {
            if (address < 0x1000 || address-0x1000 > sizeof(words) || size > sizeof(words)-(address-0x1000)) return false;
            std::memcpy(destination,reinterpret_cast<u8*>(words.data())+address-0x1000,size); return true;
        }
        bool Write(void*, size_t, size_t) override { return false; }
        void* GetPointer(void* pointer) override {
            const auto at = reinterpret_cast<size_t>(pointer);
            return at >= 0x1000 && at-0x1000 < sizeof(words) ? reinterpret_cast<u8*>(words.data())+at-0x1000 : nullptr;
        }
    };
    const auto config = Configuration();
    for (bool wide : {false,true}) for (bool cfg : {false,true}) {
        Program off, on; CodeMemory first{wide}, second{wide};
        swift::arm64::A64Decoder{0x1000,&first,&off.as}.Decode(); swift::arm64::A64Decoder{0x1000,&second,&on.as}.Decode();
        REQUIRE_FALSE(off.Pass(false).attempted); const auto stats = on.Pass(true);
        REQUIRE(stats.accepted); REQUIRE(stats.stores_removed == 1); REQUIRE(stats.removals_crossing_pc == 1);
        auto a = Compile(off,config,cfg), b = Compile(on,config,cfg); PoisonCall old_call{*a}, new_call{*b};
        REQUIRE(a->Statistics().At(x64::TransferRegion::Uniform).scalar_store_sites == 2);
        REQUIRE(b->Statistics().At(x64::TransferRegion::Uniform).scalar_store_sites == 1);
        REQUIRE(a->Statistics().At(x64::TransferRegion::Uniform).scalar_load_sites == b->Statistics().At(x64::TransferRegion::Uniform).scalar_load_sites);
        for (u64 seed : {u64{0},UINT64_MAX,u64{0x7fffffff},u64{0x8123456789abcdef}}) {
            Machine baseline, actual, expected; const auto input = seed ^ 0x12345678deadbeefULL;
            for (auto* machine : {&baseline,&actual,&expected}) { machine->Put(8,seed); machine->Put(16,input); }
            expected.Put(0,wide ? input+2 : u64{u32(input)+u32{2}}); expected.Complete(0x100c,HaltReason::None);
            old_call.Run(baseline,HaltReason::None); new_call.Run(actual,HaltReason::None);
            REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
        }
    }
    for (bool wide : {false,true}) {
        Program p; CodeMemory memory{wide,true}; swift::arm64::A64Decoder{0x1000,&memory,&p.as}.Decode();
        const auto stats = p.Pass(true);
        // ADD X/W0,X/W1,#1; ADD X/W0,X/W0,#2 observes the first store.
        REQUIRE(stats.accepted); REQUIRE(stats.stores_removed == 0); REQUIRE(stats.load_kills > 0);
    }
}

TEST_CASE("cross PC proof fences observers aliases faults metadata and opaque PC shapes", "[x64][uniform-dead-store-pc][fence]") {
    for (u32 shape = 0; shape < 17; ++shape) {
        INFO("proof boundary shape " << shape);
        Program p; auto a = p.Load(0), b = p.Load(8); auto* first = p.Store(128,a); auto* pc = p.PC();
        if (shape == 0) p.as.LoadUniform(ir::Uniform{132,T::U16}).SetType(T::U64);
        if (shape == 1) p.as.LoadMemory(ir::Operand{a}).SetType(T::U64);
        if (shape == 2) p.as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Observe)}},a,b,a).SetType(T::U64);
        if (shape == 3) p.as.GetLocation().SetType(T::U64);
        if (shape == 4) p.as.SetLocation(ir::Lambda{ir::Imm{u64{0x2000}}});
        if (shape == 5) p.as.TestFlags(ir::Flags::Carry).SetType(T::U8);
        if (shape == 6) { auto* marker = p.as.AppendInst(O::Nop); p.function.halt_checks.insert(marker); }
        if (shape == 7) { auto* label = new ir::Inst(O::BindLabel); label->SetArgs(ir::Value{label}); p.function.body->AppendInst(label); p.function.locations[label] = ir::Location{0x1004}; }
        if (shape == 8) pc->SetArg(1,a);
        if (shape == 9) pc->SetArg(0,ir::Imm{u16{4}});
        if (shape == 10) pc->SetArg(0,ir::Imm{u32{2}});
        if (shape == 11) p.as.AppendInst(O::Nop)->SetArg(4,a);
        if (shape == 12) p.as.Div(a,ir::Operand{ir::Imm{u64{2}}}).SetType(T::U64);
        if (shape == 13) p.as.LoadUniform(ir::Uniform{120,T::V128}).SetType(T::U8);
        if (shape == 14) p.as.AppendInst(O::StoreUniform,ir::Uniform{132,T::U64},a.SetCastType(T::U16));
        if (shape == 15) {
            auto branch = p.as.Goto(ir::BOOL{p.as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
            auto* label = new ir::Inst(O::BindLabel); label->SetArgs(ir::Value{label}); p.function.body->AppendInst(label);
            p.function.targets[branch.Def()] = label; p.function.stops.insert(branch.Def());
        }
        if (shape == 16) {
            auto* binary = p.as.AppendInst(O::Add,a,ir::Operand{ir::Imm{u64{1}}}); binary->SetReturn(T::U64);
            binary->SetArg(1,ir::Operand::Op{ir::OperandOp::None,1});
        }
        auto* final = p.Store(128,b); p.Finish(); const auto before = Members(p); const auto stats = p.Pass(true);
        REQUIRE(stats.accepted); REQUIRE(stats.stores_removed == 0);
        if (shape == 0 || shape == 13) REQUIRE(stats.load_kills > 0);
        else if (shape == 14) REQUIRE(stats.overlapping_store_kills > 0);
        else REQUIRE(stats.fences > 0);
        REQUIRE(Members(p) == before);
        REQUIRE(first->GetOp() == O::StoreUniform); REQUIRE(final->GetOp() == O::StoreUniform);
    }
}

TEST_CASE("cross PC native returning helper observes retained architectural value and published PC", "[x64][uniform-dead-store-pc][helper][native]") {
    const auto config = Configuration(); Program off, on;
    auto build = [&](Program& p) {
        auto a = p.Load(0), b = p.Load(8); auto address = p.as.GetUniformAddress(ir::Imm{u32{128}}).SetType(T::U64);
        p.Store(128,a); p.PC(); auto seen = p.as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Observe)}},address,a,b).SetType(T::U64);
        p.Store(144,seen); p.Store(128,b); p.Finish();
    };
    build(off); build(on); const auto stats = on.Pass(true); REQUIRE(stats.accepted); REQUIRE(stats.stores_removed == 0);
    auto a = Compile(off,config), b = Compile(on,config); PoisonCall old_call{*a}, new_call{*b};
    for (u64 seed : {u64{0},UINT64_MAX,u64{0x8123456789abcdef}}) {
        const auto last = seed ^ 0x13579bdf2468ace0ULL; Machine baseline, actual, expected;
        for (auto* machine : {&baseline,&actual,&expected}) { machine->Put(0,seed); machine->Put(8,last); }
        expected.Put(128,last); expected.Put(144,seed ^ u64{0x1004}); expected.Complete(0x1004); old_call.Run(baseline); new_call.Run(actual);
        REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
    }
}

TEST_CASE("cross PC removals retain full State at real ordinary Runtime page faults", "[x64][uniform-dead-store-pc][production][fault]") {
    struct Mapping { void* page = mmap(nullptr,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); ~Mapping() { if (page != MAP_FAILED) munmap(page,4096); } } mapping;
    REQUIRE(mapping.page != MAP_FAILED);
    for (bool enabled : {false,true}) {
        const auto config = Configuration(); backend::AddressSpace space{config}; backend::ModuleConfig policy; policy.read_only = true;
        policy.feature_overrides.Set(FeatureId::x64_cfg_allocation,true); auto module = space.MapModule(0x1000,0x2000,policy); Runtime runtime{&space}; Program p;
        auto a = p.Load(8), b = p.Load(16), before = p.as.Neg(a).SetType(T::U64), after = p.as.Neg(b).SetType(T::U64);
        p.Store(128,a); p.PC(); p.Store(128,before); p.PC();
        auto memory = p.as.LoadMemory(ir::Operand{p.Load(0)}).SetType(T::U64); p.Store(136,memory);
        p.Store(128,b); p.PC(); p.Store(128,after); p.PC(); p.Store(144,memory);
        p.as.SetLocation(ir::Lambda{ir::Imm{u64{0x1010}}}); p.Finish(); const auto stats = p.Pass(enabled);
        REQUIRE(stats.stores_removed == (enabled ? 2 : 0)); REQUIRE(stats.removals_crossing_pc == (enabled ? 2 : 0));
        REQUIRE(module->Push(p.function.body.get())); REQUIRE(backend::TranslateIR(module,p.function.body) != nullptr);
        auto* state = runtime.GetState(); state->pt = nullptr; state->guest_addr_limit = UINT64_MAX; state->guest_addr_mask = UINT64_MAX;
        state->interp_range_check = nullptr; state->interp_range_check_ctx = nullptr;
        for (bool accessible : {false,true,false}) {
            REQUIRE(mprotect(mapping.page,4096,accessible ? PROT_READ|PROT_WRITE : PROT_NONE) == 0);
            constexpr u64 loaded = 0x9988776655443322ULL, av = 0x8123456789abcdefULL, bv = 0x13579bdf2468ace0ULL;
            if (accessible) std::memcpy(mapping.page,&loaded,8);
            Machine initial; initial.Put(0,reinterpret_cast<u64>(mapping.page)); initial.Put(8,av); initial.Put(16,bv);
            std::memcpy(runtime.GetUniformBuffer().data(),initial.state->uniform_buffer_begin,extent); state->host_cpu_flags = flags_seed; runtime.SetLocation(0x1000);
            Machine expected; std::memcpy(expected.bytes.data(),state,expected.bytes.size()); expected.Put(128,0-av);
            if (accessible) { expected.Put(128,0-bv); expected.Put(136,loaded); expected.Put(144,loaded); }
            expected.Complete(accessible ? 0x1010 : 0x1008,HaltReason::None);
            expected.state->spill_area[backend::kRiscvRecoveryPcSlot] = 0; expected.state->spill_area[backend::kRiscvRecoveryFrameSlot] = 0; expected.state->spill_area[backend::kRiscvMemoryOwnedSlot] = 0;
            const auto mxcsr = _mm_getcsr(); REQUIRE(runtime.Run() == (accessible ? HaltReason::CallHost : HaltReason::PageFatal)); REQUIRE(_mm_getcsr() == mxcsr);
            REQUIRE(std::memcmp(state,expected.bytes.data(),expected.bytes.size()) == 0);
        }
    }
}

TEST_CASE("cross PC late invalid accesses and budget rejection retain exact IR and use counters", "[x64][uniform-dead-store-pc][transaction]") {
    const auto config = Configuration();
    for (u32 shape = 0; shape < 2; ++shape) {
        Program p; auto a = p.Load(0), b = p.Load(8); auto* first = p.Store(128,a); p.PC(); auto* final = p.Store(128,b);
        if (shape == 0) p.Store(extent,a); p.Finish(); const auto original = Members(p); const auto text = p.function.body->ToString();
        const auto au = a.Def()->GetUses(false), bu = b.Def()->GetUses(false);
        const auto stats = x64::EliminateOverwrittenUniformStoresAcrossPC(p.function,config,x64::ExecutionMode::Ordinary,
            {.enabled = true,.max_candidates = shape == 1 ? 0u : 4096u});
        REQUIRE_FALSE(stats.accepted); REQUIRE(stats.stores_removed == 0); REQUIRE(Members(p) == original); REQUIRE(p.function.body->ToString() == text);
        REQUIRE(first->GetOp() == O::StoreUniform); REQUIRE(final->GetOp() == O::StoreUniform);
        REQUIRE(a.Def()->GetUses(false) == au); REQUIRE(b.Def()->GetUses(false) == bu);
    }
}
