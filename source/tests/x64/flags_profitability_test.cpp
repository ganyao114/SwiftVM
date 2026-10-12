#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/x64/translator.h"
#include "runtime/backend/x64/flags_profitability.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
using F = ir::Flags;
constexpr u32 extent = 1024;
constexpr u64 initial_flags = 0xfedcba9876543219ULL;
constexpr u64 flag_mask = (u64{15} << 28) | (u64{1} << 26) | 255;
struct Program {
    backend::FunctionPlan function;
    ir::Assembler as;
    Program() : as((function.body = new ir::Block(ir::Location{0x1000})).get()) {}
    ir::Inst* Label() {
        auto* label = new ir::Inst(O::BindLabel); label->SetArgs(ir::Value{label});
        function.body->AppendInst(label); return label;
    }
    ir::Value Load(u32 offset) { return as.LoadUniform(ir::Uniform{offset, T::U64}).SetType(T::U64); }
    void Store(u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, T::U64}, value.SetCastType(T::U64)); }
    ir::Value Branch(ir::Value value) { return as.Goto(ir::BOOL{value}); }
    ir::Value Jump() { return Branch(as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)); }
    void Target(ir::Value branch, ir::Inst* label, bool stop = false) {
        function.targets[branch.Def()] = label; if (stop) function.stops.insert(branch.Def());
    }
    void Finish() { function.body->SetTerminal(ir::terminal::ReturnToHost{}); }
    x64::FlagsResidencyDecision Decide(bool weighted = true) {
        return x64::SelectFlagsResidency(function.body.get(), function, {.loop_weighted_costs = weighted});
    }
};
void Inputs(ir::Value phi, ir::Value initial, ir::Value next) {
    ir::Params params; params.Push(initial); params.Push(next); phi.Def()->SetArg(0, params);
}
Config Configuration() {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true, .backend_isa = kX86_64,
            .uniform_buffer_size = extent, .static_program = true, .stack_alignment = 16};
}
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() {
        for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 13);
        state->host_cpu_flags = initial_flags;
    }
    ~Machine() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
};
struct PoisonCall : Xbyak::CodeGenerator {
    explicit PoisonCall(x64::Translator& code) : Xbyak::CodeGenerator(4096) {
        const u32 bytes = (code.FrameSize() + 4096 + 15) & ~u32{15};
        mov(rdx, rdi); mov(r11, rsp); sub(rsp, bytes); mov(rdi, rsp); mov(rcx, bytes / 8);
        mov(rax, 0xa5e39b7142c68df0ULL); cld(); rep(); stosq(); mov(rsp, r11); mov(rdi, rdx);
        mov(rax, reinterpret_cast<u64>(code.getCode())); jmp(rax);
    }
    void Run(Machine& machine) {
        const auto saved = _mm_getcsr();
        REQUIRE(getCode<HaltReason (*)(backend::State*)>()(machine.state) == HaltReason::CallHost);
        REQUIRE(_mm_getcsr() == saved);
    }
};
u64 Arithmetic(u64 prior, bool subtract, u64 lhs, u64 rhs, u64& result) {
    const unsigned __int128 a = lhs, b = rhs, carry = (prior >> 29) & 1;
    const auto extra = subtract ? 1 - carry : carry;
    const auto full = subtract ? a - b - extra : a + b + extra; result = u64(full);
    const auto signed_value = [](u64 value) -> __int128 {
        return value & (u64{1} << 63) ? __int128(value) - (__int128{1} << 64) : __int128(value);
    };
    const auto total = subtract ? signed_value(lhs) - signed_value(rhs) - __int128(extra)
                               : signed_value(lhs) + signed_value(rhs) + __int128(extra);
    return (prior & ~flag_mask) | (u64(bool(result >> 63)) << 31) | (u64(result == 0) << 30) |
        (u64(subtract ? a >= b + extra : (full >> 64) != 0) << 29) |
        (u64(total < -(__int128{1} << 63) || total >= (__int128{1} << 63)) << 28) |
        (u64(bool((lhs ^ rhs ^ result) & 16)) << 26) | (result & 255);
}
bool Greater(u64 flags) {
    return !(flags & (u64{1} << 30)) && bool(flags & (u64{1} << 31)) == bool(flags & (u64{1} << 28));
}
struct Request {
    backend::State* state{};
    u64 replacement{};
    std::vector<u64> observed;
};
u64 MutateFlags(u64 pointer, u64 a, u64 b) {
    auto& request = *reinterpret_cast<Request*>(pointer);
    request.observed.push_back(request.state->host_cpu_flags);
    request.state->host_cpu_flags = request.replacement;
    asm volatile("xor %%r8d, %%r8d\n\txor %%r9d, %%r9d\n\txor %%r10d, %%r10d\n\txor %%r11d, %%r11d"
                 ::: "r8", "r9", "r10", "r11", "cc");
    return a + b;
}
void NativeLoop(Program& p) {
    auto pointer = p.Load(0), seed = p.Load(8), trips = p.Load(16), hot = p.Load(24);
    auto* header = p.Label();
    auto acc = p.as.AddPhi(ir::Params{}).SetType(T::U64);
    auto count = p.as.AddPhi(ir::Params{}).SetType(T::U64);
    auto returned = p.as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&MutateFlags)}}, pointer, acc, hot).SetType(T::U64);
    p.Store(128, returned);
    auto added = p.as.Adc(acc, ir::Operand{hot}).SetType(T::U64); p.as.SaveFlags(added, F::All);
    auto next = p.as.Sbb(added, ir::Operand{ir::Imm{u64{3}}}).SetType(T::U64); p.as.SaveFlags(next, F::All);
    p.Store(136, added); p.Store(144, next);
    p.Store(152, p.as.CondSet(ir::Cond::GT).SetType(T::U8)); p.Store(160, p.as.CondSet(ir::Cond::LE).SetType(T::U8));
    auto remaining = p.as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    auto back = p.Branch(p.as.TestNotZero(remaining).SetType(T::U8)); p.Target(back, header);
    Inputs(acc, seed, next); Inputs(count, trips, remaining); p.Store(168, remaining);
    p.Store(176, p.as.GetFlags(next, F::All).SetType(T::U64)); p.Finish();
}
void Initialize(Machine& machine, Request& request, u64 seed, u64 trips, u64 hot) {
    machine.Put(0, reinterpret_cast<u64>(&request)); machine.Put(8, seed); machine.Put(16, trips); machine.Put(24, hot);
}
std::vector<u64> Model(Machine& model, const Request& request, u64 seed, u64 trips, u64 hot) {
    u64 acc = seed, prior = initial_flags; std::vector<u64> seen;
    for (u64 i = 0; i < trips; ++i) {
        seen.push_back(prior); model.Put(128, acc + hot); u64 added{}, next{};
        prior = Arithmetic(request.replacement, false, acc, hot, added);
        prior = Arithmetic(prior, true, added, 3, next); acc = next;
        model.Put(136, added); model.Put(144, next); model.Put(152, Greater(prior)); model.Put(160, !Greater(prior));
    }
    model.Put(168, 0); model.Put(176, prior); model.state->host_cpu_flags = prior;
    model.state->current_loc = ir::Location{0x1000}; model.state->prev_loc = ir::Location{0x1000};
    model.state->halt_reason = HaltReason::CallHost; return seen;
}
} // namespace

TEST_CASE("flags profitability leaves no-effect and constant conditions unreserved", "[x64][flags-profit][no-flags]") {
    for (F request : {F::None, F::Positive, static_cast<F>(u64{1} << 61)}) {
        Program p; p.as.ClearFlags(request); p.Store(128, p.as.TestFlags(request).SetType(T::U8));
        p.Store(136, p.as.TestNotFlags(request).SetType(T::U8)); p.Store(144, p.as.CondSet(ir::Cond::AL).SetType(T::U8));
        p.Store(152, p.as.CondSet(ir::Cond::NV).SetType(T::U8)); p.Finish();
        const auto text = p.function.body->ToString(); const auto decision = p.Decide();
        REQUIRE_FALSE(decision.selected); REQUIRE(decision.reason == x64::FlagsResidencyReason::NoFlags);
        REQUIRE(decision.weighted_flag_reads == 0); REQUIRE(decision.weighted_flag_writes == 0);
        REQUIRE(decision.weighted_carry_reads == 0); REQUIRE(p.function.body->ToString() == text);
        const auto config = Configuration();
        auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, true, {.profitable_flags_residency = true}); code->Translate();
        REQUIRE_FALSE(code->FlagsResident()); REQUIRE(code->CfgPlan().PhysicalRegisters() == std::vector<unsigned>{3, 5, 15});
        Machine actual, expected; expected.Put(128, 0); expected.Put(136, 1); expected.Put(144, 1); expected.Put(152, 1);
        expected.state->current_loc = ir::Location{0x1000}; expected.state->prev_loc = ir::Location{0x1000}; expected.state->halt_reason = HaltReason::CallHost;
        PoisonCall call{*code}; call.Run(actual); REQUIRE(actual.bytes == expected.bytes);
    }
}

TEST_CASE("flags profitability counts two-read conditions carry BT and helper boundaries", "[x64][flags-profit][cost]") {
    Program p; auto a = p.Load(0), b = p.Load(8);
    p.Store(128, p.as.Adc(a, ir::Operand{b}).SetType(T::U64));
    p.Store(136, p.as.Sbb(a, ir::Operand{b}).SetType(T::U64));
    p.Store(144, p.as.CondSet(ir::Cond::GT).SetType(T::U8)); p.Store(152, p.as.CondSet(ir::Cond::LE).SetType(T::U8));
    p.as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&MutateFlags)}}, a, b, a).SetType(T::U64); p.Finish();
    const auto decision = p.Decide(false);
    REQUIRE(decision.weighted_flag_reads == 4); REQUIRE(decision.weighted_carry_reads == 2);
    REQUIRE(decision.weighted_flag_writes == 0); REQUIRE(decision.weighted_helper_calls == 1);
    REQUIRE(decision.allocation_builds == 1); REQUIRE(decision.allocation_recolors == 1);
}

TEST_CASE("flags profitability rejects sparse flags under simultaneous scalar pressure", "[x64][flags-profit][pressure]") {
    Program p; std::array<ir::Value, 3> values{p.Load(0), p.Load(8), p.Load(16)};
    for (u32 repeat = 0; repeat < 24; ++repeat) for (u32 i = 0; i < values.size(); ++i) p.Store(128 + i * 8, values[i]);
    p.as.ClearFlags(F::Carry); p.Finish(); const auto text = p.function.body->ToString();
    auto decision = p.Decide(); REQUIRE_FALSE(decision.selected); REQUIRE(decision.reason == x64::FlagsResidencyReason::Unprofitable);
    REQUIRE(decision.scalar_lost_benefit > 0); REQUIRE(decision.weighted_flag_reads == 1); REQUIRE(decision.weighted_flag_writes == 1);
    REQUIRE(p.function.body->ToString() == text);
    const auto config = Configuration();
    auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, true, {.profitable_flags_residency = true});
    REQUIRE_FALSE(code->FlagsResident()); REQUIRE_FALSE(code->FlagsDecision().selected);
    REQUIRE(code->CfgPlan().PhysicalRegisters() == std::vector<unsigned>{3, 5, 15});
    REQUIRE(code->PreparationStats().flags_policy_allocation_builds == 1);
    REQUIRE(code->PreparationStats().flags_policy_allocation_recolors == 1);
    REQUIRE(code->PreparationStats().full_allocation_builds == 0);
    REQUIRE(code->PreparationStats().reused_final_allocation);
}

TEST_CASE("flags profitability weights reachable natural loops and ignores unreachable flags", "[x64][flags-profit][loop][reachability]") {
    SECTION("same single clear becomes profitable in a natural loop") {
        Program p; auto* header = p.Label(); p.as.ClearFlags(F::Carry);
        auto back = p.Branch(p.Load(0)); p.Target(back, header); p.Finish();
        const auto off = p.Decide(false), on = p.Decide(true);
        REQUIRE(off.weighted_flag_reads == 1); REQUIRE(off.weighted_flag_writes == 1); REQUIRE_FALSE(off.selected);
        REQUIRE(on.weighted_flag_reads == 8); REQUIRE(on.weighted_flag_writes == 8); REQUIRE(on.selected);
        REQUIRE(on.reason == x64::FlagsResidencyReason::Profitable);
    }
    SECTION("stopping unconditional edge skips physical unreachable flag operations") {
        Program p; auto skip = p.Jump(); for (u32 i = 0; i < 16; ++i) p.as.ClearFlags(F::All);
        auto* exit = p.Label(); p.Target(skip, exit, true); p.Finish();
        auto decision = p.Decide(); REQUIRE_FALSE(decision.selected); REQUIRE(decision.reason == x64::FlagsResidencyReason::NoFlags);
        REQUIRE(decision.weighted_flag_reads == 0); REQUIRE(decision.weighted_flag_writes == 0);
    }
}

TEST_CASE("flags profitability unsupported lowerings fail closed without changing CFG", "[x64][flags-profit][fallback]") {
    Program p; auto vector = p.as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
    p.as.VecXor(vector, vector).SetType(T::V128); for (u32 i = 0; i < 16; ++i) p.as.ClearFlags(F::Carry); p.Finish();
    const auto text = p.function.body->ToString(); auto graph = x64::CfgRegisterPlan::ValidateGraph(p.function.body.get(), p.function);
    REQUIRE(graph.Enabled()); auto decision = p.Decide(); REQUIRE_FALSE(decision.selected);
    REQUIRE(decision.reason == x64::FlagsResidencyReason::Unsupported); REQUIRE(p.function.body->ToString() == text);
    REQUIRE(x64::CfgRegisterPlan::ValidateGraph(p.function.body.get(), p.function).Instructions() == graph.Instructions());
}

TEST_CASE("flags profitability honors terminal dispatch precedence and invalid metadata", "[x64][flags-profit][terminal][validation]") {
    SECTION("a stopping terminal marker suppresses unreachable physical flag operations") {
        Program p; p.as.Nop(); auto* exit = &p.function.body->GetInstList().back();
        p.function.exits.emplace(exit, ir::terminal::Condition{ir::Cond::AL,
            ir::terminal::ReturnToHost{}, ir::terminal::ReturnToHost{}});
        p.function.stops.insert(exit);
        for (u32 i = 0; i < 8; ++i) p.as.ClearFlags(F::All); p.Finish();
        const auto decision = p.Decide(); REQUIRE_FALSE(decision.selected);
        REQUIRE(decision.reason == x64::FlagsResidencyReason::NoFlags);
        REQUIRE(decision.weighted_flag_reads == 0); REQUIRE(decision.weighted_flag_writes == 0);
    }
    SECTION("a terminal GT condition counts its actual two reads") {
        Program p; p.function.body->SetTerminal(ir::terminal::Condition{ir::Cond::GT,
            ir::terminal::ReturnToHost{}, ir::terminal::ReturnToHost{}});
        auto decision = p.Decide(); REQUIRE(decision.weighted_flag_reads == 2);
        REQUIRE(decision.weighted_flag_writes == 0); REQUIRE_FALSE(decision.selected);
    }
    SECTION("missing branch target is rejected without mutating the body") {
        Program p; p.Jump(); p.as.ClearFlags(F::Carry); p.Finish();
        const auto text = p.function.body->ToString(); const auto decision = p.Decide();
        REQUIRE_FALSE(decision.selected); REQUIRE(decision.reason == x64::FlagsResidencyReason::InvalidPlan);
        REQUIRE(decision.allocation_builds == 1); REQUIRE(decision.allocation_recolors == 0);
        REQUIRE(p.function.body->ToString() == text);
    }
}

TEST_CASE("flags profitability does not add mutually exclusive nested terminal savings", "[x64][flags-profit][terminal][branches]") {
    for (auto root : {ir::Cond::AL, ir::Cond::EQ}) {
        Program p;
        p.function.body->SetTerminal(ir::terminal::Condition{root,
            ir::terminal::Condition{ir::Cond::GT,
                ir::terminal::Condition{ir::Cond::LE, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToHost{}},
                ir::terminal::ReturnToHost{}}, ir::terminal::ReturnToHost{}});
        const auto decision = p.Decide(); REQUIRE_FALSE(decision.selected);
        REQUIRE(decision.weighted_flag_reads == (root == ir::Cond::AL ? 0 : 1));
        REQUIRE(decision.weighted_exit_boundaries == 4); REQUIRE(decision.weighted_helper_calls == 0);
    }
    Program p; auto selector = p.Load(0);
    Vector<ir::terminal::Switch::Case> cases;
    cases.push_back({ir::Imm{u64{1}}, ir::terminal::Condition{ir::Cond::GT, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToHost{}}});
    p.function.body->SetTerminal(ir::terminal::Switch{selector, cases});
    const auto decision = p.Decide(); REQUIRE_FALSE(decision.selected);
    REQUIRE(decision.weighted_flag_reads == 0); REQUIRE(decision.weighted_exit_boundaries == 3);
    Program check;
    ir::Terminal continuation = ir::terminal::ReturnToHost{};
    for (u32 i = 0; i < 8; ++i)
        continuation = ir::terminal::Condition{ir::Cond::GT, continuation, ir::terminal::ReturnToHost{}};
    check.function.body->SetTerminal(ir::terminal::CheckHalt{continuation});
    const auto early = check.Decide(); REQUIRE_FALSE(early.selected);
    REQUIRE(early.reason == x64::FlagsResidencyReason::NoFlags);
    REQUIRE(early.weighted_flag_reads == 0); REQUIRE(early.weighted_flag_writes == 0);
    REQUIRE(early.weighted_carry_reads == 0); REQUIRE(early.weighted_exit_boundaries == 10);
}

TEST_CASE("prepared flags profitability preserves full State Phi carry helpers and poisoned homes", "[x64][flags-profit][native][phi][helper]") {
    const auto config = Configuration();
    for (bool forwarding : {false, true}) {
        Program ordinary, forced, adaptive; NativeLoop(ordinary); NativeLoop(forced); NativeLoop(adaptive);
        auto off = x64::Translator::PrepareFunction(ordinary.function, {}, &config, true, true, false, {.uniform_forwarding = forwarding});
        auto on = x64::Translator::PrepareFunction(forced.function, {}, &config, true, true, true, {.uniform_forwarding = forwarding});
        auto selected = x64::Translator::PrepareFunction(adaptive.function, {}, &config, true, true, true,
            {.uniform_forwarding = forwarding, .profitable_flags_residency = true});
        off->Translate(); on->Translate(); selected->Translate();
        REQUIRE_FALSE(off->FlagsResident()); REQUIRE(on->FlagsResident()); REQUIRE(selected->FlagsResident());
        REQUIRE(selected->FlagsDecision().selected); REQUIRE(selected->PreparationStats().flags_policy_allocation_builds == 1);
        REQUIRE(selected->PreparationStats().flags_policy_allocation_recolors == 1);
        REQUIRE(selected->PreparationStats().full_allocation_builds == (forwarding ? 1 : 0));
        REQUIRE(selected->PreparationStats().reused_final_allocation == !forwarding);
        REQUIRE(selected->CfgPlan().PhysicalRegisters() == std::vector<unsigned>{5, 15});
        REQUIRE(selected->Statistics().helper_call_sites > 0); REQUIRE(selected->Statistics().phi_value_copy_sites > 0);
        REQUIRE(off->Statistics().flags_state_carry_read_sites == 2); REQUIRE(off->Statistics().flags_resident_carry_read_sites == 0);
        for (const auto* resident : {on.get(), selected.get()}) {
            REQUIRE(resident->Statistics().flags_state_carry_read_sites == 0);
            REQUIRE(resident->Statistics().flags_resident_carry_read_sites == 2);
        }
        PoisonCall off_call{*off}, on_call{*on}, selected_call{*selected};
        for (u64 seed : {u64{0}, UINT64_MAX - 5, u64{0x7ffffffffffffffa}, u64{0x8123456789abcdef}})
            for (u64 trips : {u64{1}, u64{2}, u64{7}}) for (u64 carry : {u64{0}, u64{1}}) {
                CAPTURE(forwarding, seed, trips, carry); constexpr u64 hot = 17;
                Request request; request.replacement = (initial_flags & ~(u64{1} << 29)) | (carry << 29);
                Machine baseline, resident, actual, expected;
                for (auto* machine : {&baseline, &resident, &actual, &expected}) Initialize(*machine, request, seed, trips, hot);
                const auto observations = Model(expected, request, seed, trips, hot);
                for (auto [call, machine] : std::array<std::pair<PoisonCall*, Machine*>, 3>{{{&off_call, &baseline}, {&on_call, &resident}, {&selected_call, &actual}}}) {
                    request.state = machine->state; request.observed.clear(); call->Run(*machine);
                    REQUIRE(request.observed == observations); REQUIRE(machine->bytes == expected.bytes);
                }
            }
    }
}

TEST_CASE("flags profitability freezes selected budget before forwarding allocation adoption", "[x64][flags-profit][preparation][reuse]") {
    const auto config = Configuration();
    for (bool hot : {false, true}) {
        Program p; auto source = p.Load(0), duplicate = p.Load(0);
        if (hot) for (u32 i = 0; i < 16; ++i) p.as.ClearFlags(F::Carry);
        else p.as.ClearFlags(F::Carry);
        p.Store(128, duplicate); p.Store(136, source); p.Store(144, source); p.Finish();
        auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, true,
            {.uniform_forwarding = true, .profitable_flags_residency = true}); code->Translate();
        REQUIRE(code->FlagsDecision().selected == hot); REQUIRE(code->FlagsResident() == hot);
        REQUIRE(code->CfgPlan().PhysicalRegisters() == (hot ? std::vector<unsigned>{5, 15} : std::vector<unsigned>{3, 5, 15}));
        REQUIRE(duplicate.Def()->GetOp() == O::BitExtract); REQUIRE(code->UniformForwardStats().loads_forwarded == 1);
        REQUIRE(code->PreparationStats().reused_final_allocation); REQUIRE(code->PreparationStats().full_allocation_builds == 1);
        REQUIRE(code->PreparationStats().flags_policy_allocation_builds == 1);
        REQUIRE(code->PreparationStats().flags_policy_allocation_recolors == 1); REQUIRE(code->CfgPlan().Register(source.Def()));
    }
}

TEST_CASE("flags profitability opt-in bypasses unrequested residency and non-CFG allocation", "[x64][flags-profit][compatibility]") {
    const auto config = Configuration();
    for (bool cfg : {false, true}) for (bool requested : {false, true}) {
        Program p; for (u32 i = 0; i < 16; ++i) p.as.ClearFlags(F::Carry); p.Finish();
        auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, cfg, requested, {.profitable_flags_residency = true});
        REQUIRE(code->FlagsResident() == requested);
        REQUIRE(code->PreparationStats().flags_policy_allocation_builds == (cfg && requested ? 1 : 0));
        REQUIRE(code->PreparationStats().flags_policy_allocation_recolors == (cfg && requested ? 1 : 0));
    }
}

TEST_CASE("resident epilogue commits once before its cleanup helper", "[x64][flags-profit][epilogue]") {
    Program p; for (u32 i = 0; i < 16; ++i) p.as.ClearFlags(F::Carry); p.Finish();
    const auto config = Configuration();
    auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, true); code->Translate();
    REQUIRE(code->FlagsResident()); REQUIRE(code->Statistics().flags_state_load_sites == 2);
    REQUIRE(code->Statistics().flags_state_store_sites == 1);
    REQUIRE(code->Statistics().flags_state_carry_read_sites == 0);
}

TEST_CASE("production profitable flags policy preserves complete State at real protected page faults", "[x64][flags-profit][production][fault]") {
    struct Mapping {
        void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ~Mapping() { if (page != MAP_FAILED) munmap(page, 4096); }
    } mapping;
    REQUIRE(mapping.page != MAP_FAILED);
    for (bool profitable : {false, true}) {
        const auto config = Configuration(); backend::AddressSpace space{config}; backend::ModuleConfig policy;
        policy.read_only = true; policy.feature_overrides.Set(FeatureId::x64_cfg_allocation, true);
        policy.feature_overrides.Set(FeatureId::x64_flags_residency, true);
        policy.feature_overrides.Set(FeatureId::x64_flags_profitability, true);
        auto module = space.MapModule(0x1000, 0x2000, policy); Runtime runtime{&space}; Program p;
        const u32 repeats = profitable ? 16 : 1;
        for (u32 i = 0; i < repeats; ++i) p.as.ClearFlags(F::Carry);
        p.as.AdvancePC(ir::Imm{u32{4}});
        p.Store(128, p.as.LoadMemory(ir::Operand{p.Load(0)}).SetType(T::U64));
        p.Store(136, p.as.GetFlags(p.Load(8), F::All).SetType(T::U64)); p.Finish();
        const auto decision = p.Decide(); REQUIRE(decision.selected == profitable);
        auto inventory = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, true, {.profitable_flags_residency = true});
        inventory->Translate(); REQUIRE(inventory->FlagsResident() == profitable);
        REQUIRE(module->Push(p.function.body.get())); REQUIRE(backend::TranslateIR(module, p.function.body) != nullptr);
        auto* state = runtime.GetState(); state->guest_addr_limit = UINT64_MAX; state->guest_addr_mask = UINT64_MAX;
        state->interp_range_check = nullptr; state->interp_range_check_ctx = nullptr;
        for (bool accessible : {false, true, false}) {
            REQUIRE(mprotect(mapping.page, 4096, accessible ? PROT_READ | PROT_WRITE : PROT_NONE) == 0);
            constexpr u64 memory = 0x9988776655443322ULL;
            if (accessible) std::memcpy(mapping.page, &memory, 8);
            Machine initial; initial.Put(0, reinterpret_cast<u64>(mapping.page));
            std::memcpy(runtime.GetUniformBuffer().data(), initial.state->uniform_buffer_begin, extent);
            state->host_cpu_flags = initial_flags; runtime.SetLocation(0x1000);
            Machine expected; std::memcpy(expected.bytes.data(), state, expected.bytes.size());
            expected.state->host_cpu_flags = initial_flags & ~(u64{1} << 29);
            if (accessible) { expected.Put(128, memory); expected.Put(136, expected.state->host_cpu_flags); }
            expected.state->current_loc = ir::Location{0x1004}; expected.state->prev_loc = ir::Location{0x1000};
            expected.state->halt_reason = HaltReason::None;
            expected.state->spill_area[backend::kRiscvRecoveryPcSlot] = 0;
            expected.state->spill_area[backend::kRiscvRecoveryFrameSlot] = 0;
            expected.state->spill_area[backend::kRiscvMemoryOwnedSlot] = 0;
            const auto saved = _mm_getcsr(); REQUIRE(runtime.Run() == (accessible ? HaltReason::CallHost : HaltReason::PageFatal));
            REQUIRE(_mm_getcsr() == saved); REQUIRE(std::memcmp(state, expected.bytes.data(), expected.bytes.size()) == 0);
        }
    }
}
