#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sys/mman.h>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
constexpr u32 extent = 1024;
constexpr u64 initial_flags = 0xfedcba9876543219ULL;
struct Program {
    backend::FunctionPlan function;
    ir::Assembler as;
    Program() : as((function.body = new ir::Block(ir::Location{0x1000})).get()) {}
    ir::Inst* Label() {
        auto* label = new ir::Inst(O::BindLabel); label->SetArgs(ir::Value{label}); function.body->AppendInst(label); return label;
    }
    ir::Value Load(u32 offset) { return as.LoadUniform(ir::Uniform{offset, T::U64}).SetType(T::U64); }
    ir::Value Add(ir::Value a, ir::Value b) { return as.Add(a, ir::Operand{b}).SetType(T::U64); }
    ir::Value Branch(ir::Value value) { return as.Goto(ir::BOOL{value}); }
    ir::Value Jump() { return Branch(as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)); }
    void Target(ir::Value branch, ir::Inst* label, bool stop = false) {
        function.targets[branch.Def()] = label; if (stop) function.stops.insert(branch.Def());
    }
    void Store(u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
    void Finish() { function.body->SetTerminal(ir::terminal::ReturnToHost{}); }
    x64::CfgRegisterPlan Plan(bool weighted, u64 budget = 8 * 1024 * 1024, std::vector<unsigned> regs = {3, 5, 15}) {
        return x64::CfgRegisterPlan::Build(function.body.get(), function,
            {.physical_registers = std::move(regs), .interference_storage_budget_bytes = budget, .loop_weighted_costs = weighted});
    }
};
const x64::CfgScalarAllocation& Allocation(const x64::CfgRegisterPlan& plan, ir::Value value) {
    for (const auto& allocation : plan.Allocations()) if (allocation.producer == value.Def()) return allocation;
    FAIL("missing scalar allocation"); return plan.Allocations().front();
}
u32 BlockOf(const x64::CfgRegisterPlan& plan, ir::Inst* inst) {
    const auto position = std::find(plan.Instructions().begin(), plan.Instructions().end(), inst);
    REQUIRE(position != plan.Instructions().end()); const u32 index = position - plan.Instructions().begin();
    for (u32 b = 0; b < plan.Blocks().size(); ++b) if (index >= plan.Blocks()[b].begin && index < plan.Blocks()[b].end) return b;
    FAIL("instruction has no block"); return 0;
}
void Same(const x64::CfgRegisterPlan& a, const x64::CfgRegisterPlan& b) {
    REQUIRE(a.Enabled()); REQUIRE(a.Validate()); REQUIRE(b.Validate()); REQUIRE(a.Instructions() == b.Instructions());
    REQUIRE(a.LoopWeightedCostsEnabled() == b.LoopWeightedCostsEnabled()); REQUIRE(a.PhysicalRegisters() == b.PhysicalRegisters());
    REQUIRE(a.ConstructionStats().natural_loops == b.ConstructionStats().natural_loops);
    REQUIRE(a.ConstructionStats().max_loop_depth == b.ConstructionStats().max_loop_depth);
    REQUIRE(a.Blocks().size() == b.Blocks().size()); REQUIRE(a.Edges().size() == b.Edges().size());
    for (u32 i = 0; i < a.Blocks().size(); ++i) {
        const auto& x = a.Blocks()[i]; const auto& y = b.Blocks()[i];
        REQUIRE(x.begin == y.begin); REQUIRE(x.end == y.end); REQUIRE(x.reachable == y.reachable);
        REQUIRE(x.loop_depth == y.loop_depth); REQUIRE(x.loop_weight == y.loop_weight);
        REQUIRE(x.incoming_edges == y.incoming_edges); REQUIRE(x.outgoing_edges == y.outgoing_edges);
        REQUIRE(x.live_in == y.live_in); REQUIRE(x.live_out == y.live_out);
        for (u32 j = 0; j < a.Blocks().size(); ++j) REQUIRE(a.DominatesBlock(i, j) == b.DominatesBlock(i, j));
    }
    for (u32 i = 0; i < a.Edges().size(); ++i) {
        const auto& x = a.Edges()[i]; const auto& y = b.Edges()[i];
        REQUIRE(x.from == y.from); REQUIRE(x.to == y.to); REQUIRE(x.source == y.source);
        REQUIRE(x.taken == y.taken); REQUIRE(x.host_entry == y.host_entry); REQUIRE(x.live == y.live); REQUIRE(x.copies.size() == y.copies.size());
        for (u32 j = 0; j < x.copies.size(); ++j) {
            REQUIRE(x.copies[j].destination == y.copies[j].destination); REQUIRE(x.copies[j].source.type == y.copies[j].source.type);
            if (x.copies[j].source.IsValue()) REQUIRE(x.copies[j].source.value == y.copies[j].source.value);
        }
    }
    REQUIRE(a.Allocations().size() == b.Allocations().size());
    for (u32 i = 0; i < a.Allocations().size(); ++i) {
        const auto& x = a.Allocations()[i]; const auto& y = b.Allocations()[i];
        REQUIRE(x.producer == y.producer); REQUIRE(x.slot == y.slot); REQUIRE(x.uses == y.uses);
        REQUIRE(x.scalar_reads == y.scalar_reads); REQUIRE(x.estimated_benefit == y.estimated_benefit);
        REQUIRE(x.weighted_reads == y.weighted_reads); REQUIRE(x.weighted_benefit == y.weighted_benefit);
        REQUIRE(x.requires_canonical_home == y.requires_canonical_home); REQUIRE(x.requires_full_container == y.requires_full_container);
        for (auto* other : a.Instructions()) {
            REQUIRE(a.Interferes(x.producer, other) == b.Interferes(x.producer, other));
            REQUIRE(a.CanReuseBinaryInput(x.producer, other) == b.CanReuseBinaryInput(x.producer, other));
        }
    }
}
void Repeat(Program& p, const x64::CfgRegisterPlan& plan, bool weighted, std::vector<unsigned> regs = {3, 5, 15}) {
    const auto sparse = p.Plan(weighted, 0, regs); Same(plan, sparse);
    REQUIRE(plan.ConstructionStats().dense_used); REQUIRE(sparse.ConstructionStats().sparse_fallback);
    for (u32 i = 0; i < 3; ++i) Same(plan, p.Plan(weighted, 8 * 1024 * 1024, regs));
}
void Unweighted(const x64::CfgRegisterPlan& plan) {
    REQUIRE(plan.Validate()); REQUIRE_FALSE(plan.LoopWeightedCostsEnabled());
    REQUIRE(plan.ConstructionStats().natural_loops == 0); REQUIRE(plan.ConstructionStats().max_loop_depth == 0);
    for (const auto& b : plan.Blocks()) { REQUIRE(b.loop_depth == 0); REQUIRE(b.loop_weight == 1); }
    for (const auto& a : plan.Allocations()) { REQUIRE(a.weighted_reads == a.scalar_reads); REQUIRE(a.weighted_benefit == a.estimated_benefit); }
}
void Inputs(ir::Value phi, ir::Value initial, ir::Value next) { ir::Params params; params.Push(initial); params.Push(next); phi.Def()->SetArg(0, params); }
u64 Helper(u64 a, u64 b, u64 c) {
    asm volatile("xor %%r8d, %%r8d\n\txor %%r9d, %%r9d\n\txor %%r10d, %%r10d\n\txor %%r11d, %%r11d" ::: "r8", "r9", "r10", "r11", "cc");
    return a + b + c;
}
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() { for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 13); state->host_cpu_flags = initial_flags; }
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
    void Run(Machine& machine) { const auto saved = _mm_getcsr(); REQUIRE(getCode<HaltReason (*)(backend::State*)>()(machine.state) == HaltReason::CallHost); REQUIRE(_mm_getcsr() == saved); }
};
u64 AddFlags(u64 a, u64 b) {
    const u64 result = a + b; const bool overflow = ((~(a ^ b) & (a ^ result)) >> 63) != 0;
    const u64 mask = (u64{15} << 28) | (u64{1} << 26) | 255;
    return (initial_flags & ~mask) | (result & (u64{1} << 63) ? u64{1} << 31 : 0) | (u64(result == 0) << 30) |
        (u64(result < a) << 29) | (u64(overflow) << 28) | (u64(bool((a ^ b ^ result) & 16)) << 26) | (result & 255);
}
void Initialize(Machine& machine, u64 seed, u64 trips, u64 hot) {
    machine.Put(8, seed); machine.Put(16, trips); machine.Put(24, hot);
    for (u32 i = 0; i < 4; ++i) machine.Put(32 + i * 8, seed + i * 29);
}
ir::Value NativeLoop(Program& p) {
    auto seed = p.Load(8), trips = p.Load(16), hot = p.Load(24); std::array<ir::Value, 4> pressure;
    for (u32 i = 0; i < pressure.size(); ++i) pressure[i] = p.Load(32 + i * 8);
    for (u32 repeat = 0; repeat < 8; ++repeat) for (u32 i = 0; i < pressure.size(); ++i) p.Store(768 + i * 8, pressure[i]);
    auto phi = p.as.AddPhi(ir::Params{}).SetType(T::U64), count = p.as.AddPhi(ir::Params{}).SetType(T::U64);
    auto returned = p.as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, phi, hot, phi).SetType(T::U64); p.Store(128, returned);
    auto next = p.Add(phi, hot); p.as.SaveFlags(next, ir::Flags::All);
    auto remaining = p.as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64); auto back = p.Branch(p.as.TestNotZero(remaining));
    p.as.BindLabel(back); auto* header = &p.function.body->GetInstList().back();
    p.function.body->RemoveInst(header); p.function.body->InsertBefore(header, phi.Def());
    Inputs(phi, seed, next); Inputs(count, trips, remaining);
    p.Store(136, phi); p.Store(144, next); p.Store(152, remaining); p.Store(160, p.as.GetFlags(next, ir::Flags::All).SetType(T::U64));
    for (u32 i = 0; i < pressure.size(); ++i) p.Store(768 + i * 8, pressure[i]);
    return next;
}
u64 LoopModel(Machine& machine, u64 seed, u64 trips, u64 hot) {
    const u64 previous = seed + (trips - 1) * hot, next = previous + hot;
    machine.Put(128, previous * 2 + hot); machine.Put(136, previous); machine.Put(144, next); machine.Put(152, 0);
    const u64 flags = AddFlags(previous, hot); machine.Put(160, flags);
    for (u32 i = 0; i < 4; ++i) machine.Put(768 + i * 8, seed + i * 29);
    machine.state->host_cpu_flags = flags; return next;
}
Config Configuration() { return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true, .backend_isa = kX86_64, .uniform_buffer_size = extent, .static_program = true, .stack_alignment = 16}; }
} // namespace

TEST_CASE("CFG natural loop costs select hot reads over more cold static reads", "[x64][cfg-plan][loop-weight][allocation]") {
    Program p; auto cold = p.Load(0), hot = p.Load(8);
    for (u32 i = 0; i < 8; ++i) p.Store(128, cold);
    auto* header = p.Label(); p.Store(136, hot); p.Store(144, hot); auto back = p.Branch(p.Load(16));
    p.Store(152, cold); p.Store(160, hot); p.Target(back, header); p.Finish();
    auto off = p.Plan(false, 8 * 1024 * 1024, {5}), on = p.Plan(true, 8 * 1024 * 1024, {5}); Unweighted(off);
    REQUIRE(on.LoopWeightedCostsEnabled()); REQUIRE(on.Interferes(cold.Def(), hot.Def()));
    REQUIRE(Allocation(off, cold).scalar_reads == 9); REQUIRE(Allocation(off, hot).scalar_reads == 3);
    REQUIRE(Allocation(on, cold).weighted_reads == 9); REQUIRE(Allocation(on, hot).weighted_reads == 17);
    REQUIRE(Allocation(on, cold).weighted_benefit == 9); REQUIRE(Allocation(on, hot).weighted_benefit == 17);
    REQUIRE(off.Register(cold.Def()) == 5); REQUIRE_FALSE(off.Register(hot.Def()));
    REQUIRE(on.Register(hot.Def()) == 5); REQUIRE_FALSE(on.Register(cold.Def()));
    REQUIRE(on.ConstructionStats().natural_loops == 1); REQUIRE(on.ConstructionStats().max_loop_depth == 1);
    Repeat(p, off, false, {5}); Repeat(p, on, true, {5});
}

TEST_CASE("CFG self loops merge latches once and weight Phi source reads on incoming edges", "[x64][cfg-plan][loop-weight][phi]") {
    SECTION("self loop Phi entry read is cold and backedge read is hot") {
        Program p; auto seed = p.Load(0), entry = p.Jump(); auto* header = p.Label();
        auto phi = p.as.AddPhi(ir::Params{}).SetType(T::U64); auto next = p.as.Add(phi, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        auto back = p.Branch(p.Load(8)); p.Store(16, next); Inputs(phi, seed, next); p.Target(entry, header, true); p.Target(back, header); p.Finish();
        auto on = p.Plan(true); REQUIRE(on.Validate()); REQUIRE(on.ConstructionStats().natural_loops == 1);
        REQUIRE(on.Blocks()[BlockOf(on, header)].loop_depth == 1); REQUIRE(on.Blocks()[BlockOf(on, header)].loop_weight == 8);
        REQUIRE(Allocation(on, seed).scalar_reads == 1); REQUIRE(Allocation(on, seed).weighted_reads == 1);
        REQUIRE(Allocation(on, phi).scalar_reads == 1); REQUIRE(Allocation(on, phi).weighted_reads == 8);
        REQUIRE(Allocation(on, next).scalar_reads == 2); REQUIRE(Allocation(on, next).weighted_reads == 9);
        REQUIRE(Allocation(on, next).weighted_benefit == 9); Repeat(p, on, true); Unweighted(p.Plan(false));
        auto graph = x64::CfgRegisterPlan::ValidateGraph(p.function.body.get(), p.function); REQUIRE(graph.Enabled()); REQUIRE_FALSE(graph.LoopWeightedCostsEnabled());
        REQUIRE(graph.ConstructionStats().natural_loops == 0); for (auto b : graph.Blocks()) { REQUIRE(b.loop_depth == 0); REQUIRE(b.loop_weight == 1); }
    }
    SECTION("two latches share one natural loop header") {
        Program p; auto entry = p.Jump(); auto* header = p.Label(); auto choice = p.Branch(p.Load(0));
        auto first = p.Jump(); auto* second_label = p.Label(); auto second = p.Branch(p.Load(8)); p.Store(16, p.Load(24));
        p.Target(entry, header, true); p.Target(choice, second_label); p.Target(first, header, true); p.Target(second, header); p.Finish();
        auto on = p.Plan(true); REQUIRE(on.Validate()); REQUIRE(on.ConstructionStats().natural_loops == 1); REQUIRE(on.ConstructionStats().max_loop_depth == 1);
        for (auto* member : {header, first.Def(), second.Def()}) { const auto& b = on.Blocks()[BlockOf(on, member)]; REQUIRE(b.loop_depth == 1); REQUIRE(b.loop_weight == 8); }
        REQUIRE(on.Blocks().back().loop_depth == 0); Repeat(p, on, true);
    }
}

TEST_CASE("CFG nested natural loops cap weights while preserving distinct headers", "[x64][cfg-plan][loop-weight][nested]") {
    Program p; auto entry = p.Jump(); std::array<ir::Inst*, 6> headers;
    for (auto& header : headers) header = p.Label();
    std::array<ir::Value, 6> latches; for (u32 i = 0; i < 6; ++i) latches[i] = p.Branch(p.Load(i * 8));
    p.Target(entry, headers[0], true); for (u32 i = 0; i < 6; ++i) p.Target(latches[i], headers[5 - i]); p.Store(64, p.Load(56)); p.Finish();
    auto on = p.Plan(true); REQUIRE(on.Validate()); REQUIRE(on.ConstructionStats().natural_loops == 6);
    REQUIRE(on.ConstructionStats().max_loop_depth == 6);
    for (u32 i = 0; i < headers.size(); ++i) {
        const auto& b = on.Blocks()[BlockOf(on, headers[i])]; REQUIRE(b.loop_depth == i + 1);
        u32 weight = 1; for (u32 depth = 0; depth < std::min(i + 1, 4u); ++depth) weight *= 8; REQUIRE(b.loop_weight == weight);
    }
    REQUIRE(on.Blocks().back().loop_depth == 0); Repeat(p, on, true); Unweighted(p.Plan(false));
}

TEST_CASE("CFG unreachable cycles and irreducible two entry cycles keep conservative unit cost", "[x64][cfg-plan][loop-weight][conservative]") {
    SECTION("unreachable self loop") {
        Program p; auto entry = p.Jump(); auto* dead = p.Label(); auto back = p.Branch(p.Load(0)); auto* exit = p.Label(); p.Store(8, p.Load(16));
        p.Target(entry, exit, true); p.Target(back, dead); p.Finish(); auto on = p.Plan(true);
        REQUIRE(on.Validate()); REQUIRE_FALSE(on.Blocks()[BlockOf(on, dead)].reachable); REQUIRE(on.ConstructionStats().natural_loops == 0);
        for (auto b : on.Blocks()) { REQUIRE(b.loop_depth == 0); REQUIRE(b.loop_weight == 1); } Repeat(p, on, true);
    }
    SECTION("reachable irreducible cycle with two external entries") {
        Program p; auto entry = p.Branch(p.Load(0)); auto* left = p.Label(); auto left_jump = p.Jump();
        auto* right = p.Label(); auto right_back = p.Branch(p.Load(8)); p.Store(16, p.Load(24));
        p.Target(entry, right); p.Target(left_jump, right, true); p.Target(right_back, left); p.Finish(); auto on = p.Plan(true);
        REQUIRE(on.Validate()); REQUIRE(on.Blocks()[BlockOf(on, left)].reachable); REQUIRE(on.Blocks()[BlockOf(on, right)].reachable);
        REQUIRE_FALSE(on.DominatesBlock(BlockOf(on, left), BlockOf(on, right))); REQUIRE_FALSE(on.DominatesBlock(BlockOf(on, right), BlockOf(on, left)));
        REQUIRE(on.ConstructionStats().natural_loops == 0); REQUIRE(on.ConstructionStats().max_loop_depth == 0);
        for (auto b : on.Blocks()) { REQUIRE(b.loop_depth == 0); REQUIRE(b.loop_weight == 1); }
        for (auto a : on.Allocations()) { REQUIRE(a.weighted_reads == a.scalar_reads); REQUIRE(a.weighted_benefit == a.estimated_benefit); } Repeat(p, on, true);
    }
}

TEST_CASE("CFG loop costs charge hot publication when raw uses retain canonical storage", "[x64][cfg-plan][loop-weight][raw]") {
    Program p; auto* header = p.Label(); auto source = p.Load(0);
    auto raw = p.as.BitCast(ir::Value{source.Def()}.SetCastType(T::V128)).SetType(T::V128); p.Store(320, raw);
    p.Store(128, source); p.Store(136, source); auto back = p.Branch(p.Load(16)); p.Store(144, source); p.Target(back, header); p.Finish();
    auto on = p.Plan(true), off = p.Plan(false); REQUIRE(on.Validate()); Unweighted(off);
    const auto& hot = Allocation(on, source); REQUIRE(hot.requires_full_container); REQUIRE(hot.requires_canonical_home);
    REQUIRE(hot.scalar_reads == 3); REQUIRE(hot.estimated_benefit == 2); REQUIRE(hot.weighted_reads == 17); REQUIRE(hot.weighted_benefit == 9);
    REQUIRE(on.Blocks()[BlockOf(on, source.Def())].loop_weight == 8); Repeat(p, on, true); Repeat(p, off, false);
}

TEST_CASE("prepared loop weighted allocations preserve full native state flags Phi and helpers", "[x64][loop-weight][native][phi][helper][flags]") {
    for (bool flags : {false, true}) for (bool forwarding : {false, true}) {
        const auto config = Configuration(); Program p; NativeLoop(p); p.Finish();
        auto off = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, flags, {.uniform_forwarding = forwarding, .loop_weighted_costs = false}); off->Translate();
        auto on = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, flags, {.uniform_forwarding = forwarding, .loop_weighted_costs = true}); on->Translate();
        REQUIRE_FALSE(off->LoopWeightedCostsEnabled()); REQUIRE(on->LoopWeightedCostsEnabled());
        REQUIRE(off->CfgPlan().Instructions() == on->CfgPlan().Instructions()); REQUIRE(off->CfgPlan().PhysicalRegisters() == on->CfgPlan().PhysicalRegisters());
        Unweighted(off->CfgPlan()); REQUIRE(on->CfgPlan().ConstructionStats().natural_loops == 1); REQUIRE(on->Statistics().helper_call_sites > 0); REQUIRE(on->Statistics().phi_value_copy_sites > 0);
        REQUIRE(off->UniformForwardStats().loads_forwarded == 0); REQUIRE(on->UniformForwardStats().loads_forwarded == 0);
        REQUIRE_FALSE(off->PreparationStats().reused_final_allocation); REQUIRE_FALSE(on->PreparationStats().reused_final_allocation);
        REQUIRE(off->PreparationStats().full_allocation_builds == 1); REQUIRE(on->PreparationStats().full_allocation_builds == 1);
        PoisonCall old_call{*off}, new_call{*on};
        for (u64 seed : {u64{0}, UINT64_MAX - 5, u64{0x7ffffffffffffffa}, u64{0x8123456789abcdef}}) for (u64 trips : {u64{1}, u64{2}, u64{7}}) {
            CAPTURE(flags, forwarding, seed, trips); constexpr u64 hot = 17; Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) Initialize(*machine, seed, trips, hot); LoopModel(expected, seed, trips, hot);
            expected.state->current_loc = ir::Location{0x1000}; expected.state->prev_loc = ir::Location{0x1000}; expected.state->halt_reason = HaltReason::CallHost;
            old_call.Run(baseline); new_call.Run(actual); REQUIRE(std::memcmp(baseline.bytes.data(), actual.bytes.data(), actual.bytes.size()) == 0);
            REQUIRE(std::memcmp(actual.bytes.data(), expected.bytes.data(), expected.bytes.size()) == 0);
        }
    }
}

TEST_CASE("prepared uniform forwarding adopts the requested loop weighted final allocation", "[x64][loop-weight][preparation][reuse]") {
    const auto config = Configuration();
    for (bool weighted : {false, true}) {
        Program p; auto source = p.Load(0), duplicate = p.Load(0); auto* header = p.Label(); p.Store(8, duplicate);
        auto back = p.Branch(p.Load(16)); p.Store(24, source); p.Target(back, header); p.Finish();
        auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, false,
            {.uniform_forwarding = true, .loop_weighted_costs = weighted}); code->Translate();
        REQUIRE(code->LoopWeightedCostsEnabled() == weighted); REQUIRE(code->CfgPlan().Validate());
        REQUIRE(duplicate.Def()->GetOp() == O::BitExtract); REQUIRE(code->UniformForwardStats().loads_forwarded == 1);
        REQUIRE(code->PreparationStats().reused_final_allocation); REQUIRE(code->PreparationStats().full_allocation_builds == 1);
        REQUIRE(code->CfgPlan().Register(source.Def()));
        REQUIRE(code->CfgPlan().ConstructionStats().natural_loops == (weighted ? 1 : 0));
    }
}

TEST_CASE("production loop weighted allocation recovers complete state at actual protected page faults", "[x64][loop-weight][production][fault]") {
    struct Mapping { void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); ~Mapping() { if (page != MAP_FAILED) munmap(page, 4096); } } mapping;
    REQUIRE(mapping.page != MAP_FAILED);
    for (bool flags : {false, true}) {
        auto config = Configuration(); backend::AddressSpace space{config}; backend::ModuleConfig policy; policy.read_only = true;
        policy.feature_overrides.Set(FeatureId::x64_cfg_allocation, true); policy.feature_overrides.Set(FeatureId::x64_flags_residency, flags);
        auto module = space.MapModule(0x1000, 0x2000, policy); Runtime runtime{&space}; Program p; auto next = NativeLoop(p);
        p.as.AdvancePC(ir::Imm{u32{4}}); p.Store(168, p.as.LoadMemory(ir::Operand{p.Load(0)}).SetType(T::U64));
        p.Store(176, p.as.Xor(next, ir::Operand{ir::Imm{u64{17}}}).SetType(T::U64)); p.Finish();
        x64::Translator inventory{p.function.body.get(), {}, &config, &p.function, true, x64::ExecutionMode::Ordinary, true, flags}; inventory.Translate();
        REQUIRE(inventory.LoopWeightedCostsEnabled()); REQUIRE(inventory.CfgPlan().ConstructionStats().natural_loops == 1);
        REQUIRE(module->Push(p.function.body.get())); REQUIRE(backend::TranslateIR(module, p.function.body) != nullptr);
        auto* state = runtime.GetState(); state->guest_addr_limit = UINT64_MAX; state->guest_addr_mask = UINT64_MAX; state->interp_range_check = nullptr; state->interp_range_check_ctx = nullptr;
        for (bool accessible : {false, true, false}) {
            REQUIRE(mprotect(mapping.page, 4096, accessible ? PROT_READ | PROT_WRITE : PROT_NONE) == 0);
            constexpr u64 memory = 0x9988776655443322ULL, seed = UINT64_MAX - 5, trips = 7, hot = 17;
            if (accessible) std::memcpy(mapping.page, &memory, 8);
            Machine model; Initialize(model, seed, trips, hot); model.Put(0, reinterpret_cast<u64>(mapping.page));
            std::memcpy(runtime.GetUniformBuffer().data(), model.state->uniform_buffer_begin, extent); state->host_cpu_flags = initial_flags; runtime.SetLocation(0x1000);
            Machine expected; std::memcpy(expected.bytes.data(), state, expected.bytes.size()); const u64 result = LoopModel(expected, seed, trips, hot);
            if (accessible) { expected.Put(168, memory); expected.Put(176, result ^ 17); }
            expected.state->current_loc = ir::Location{0x1004}; expected.state->prev_loc = ir::Location{0x1000}; expected.state->halt_reason = HaltReason::None;
            expected.state->spill_area[backend::kRiscvRecoveryPcSlot] = 0; expected.state->spill_area[backend::kRiscvRecoveryFrameSlot] = 0; expected.state->spill_area[backend::kRiscvMemoryOwnedSlot] = 0;
            const auto saved = _mm_getcsr(); REQUIRE(runtime.Run() == (accessible ? HaltReason::CallHost : HaltReason::PageFatal)); REQUIRE(_mm_getcsr() == saved);
            REQUIRE(std::memcmp(state, expected.bytes.data(), expected.bytes.size()) == 0);
        }
    }
}
