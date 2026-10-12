#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <new>
#include <stdexcept>
#include <sys/mman.h>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/riscv64/memory_protocol.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
constexpr u32 extent = 1024;
constexpr u64 initial_flags = 0xfedcba9876543219ULL;
constexpr std::array types{T::V8, T::V16, T::V32, T::V64, T::V128};
using Bytes = std::array<u8, 16>;
struct Program {
    backend::FunctionPlan function;
    ir::Assembler as;
    Program() : as((function.body = new ir::Block(ir::Location{0x1000})).get()) {}
    ir::Value Load(u32 offset, T type = T::V128) { return as.LoadUniform(ir::Uniform{offset, T::V128}).SetType(type); }
    ir::Value Scalar(u32 offset) { return as.LoadUniform(ir::Uniform{offset, T::U64}).SetType(T::U64); }
    ir::Value Xor(ir::Value a, ir::Value b, T type = T::V128) { return as.VecXor(a, b).SetType(type); }
    void Store(u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, T::V128}, value.SetCastType(T::V128)); }
    ir::Inst* Label() { auto* label = new ir::Inst(O::BindLabel); label->SetArgs(ir::Value{label}); function.body->AppendInst(label); return label; }
    ir::Value Jump() { return as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)}); }
    void Target(ir::Value branch, ir::Inst* label, bool stop = false) { function.targets[branch.Def()] = label; if (stop) function.stops.insert(branch.Def()); }
    void Finish() { function.body->SetTerminal(ir::terminal::ReturnToHost{}); }
    x64::CfgRegisterPlan Graph() { return x64::CfgRegisterPlan::ValidateGraph(function.body.get(), function); }
};
void Inputs(ir::Value phi, ir::Value first, ir::Value second) { ir::Params inputs; inputs.Push(first); inputs.Push(second); phi.Def()->SetArg(0, inputs); }
Config Configuration() {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true, .backend_isa = kX86_64,
            .uniform_buffer_size = extent, .static_program = true, .global_opts = Optimizations::None, .stack_alignment = 16};
}
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() { for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 13); state->host_cpu_flags = initial_flags; }
    ~Machine() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    void Put(u32 offset, const Bytes& value) { std::memcpy(state->uniform_buffer_begin + offset, value.data(), 16); }
    Bytes Get(u32 offset) const { Bytes value; std::memcpy(value.data(), state->uniform_buffer_begin + offset, 16); return value; }
    void Complete() { state->current_loc = ir::Location{0x1000}; state->prev_loc = ir::Location{0x1000}; state->halt_reason = HaltReason::CallHost; }
};
Bytes Xor(const Bytes& a, const Bytes& b) { Bytes c; for (u32 i = 0; i < 16; ++i) c[i] = a[i] ^ b[i]; return c; }
Bytes Binary(const Bytes& a, const Bytes& b, O op, u32 bits) {
    Bytes c{};
    if (op == O::Vec4Add || op == O::Vec4Sub) bits = 32;
    if (op == O::VecAdd || op == O::Vec4Add || op == O::VecSub || op == O::Vec4Sub) {
        const u32 bytes = bits / 8;
        for (u32 i = 0; i < 16; i += bytes) {
            u64 left{}, right{}; std::memcpy(&left, a.data() + i, bytes); std::memcpy(&right, b.data() + i, bytes);
            const u64 lane = op == O::VecAdd || op == O::Vec4Add ? left + right : left - right;
            std::memcpy(c.data() + i, &lane, bytes);
        }
    } else for (u32 i = 0; i < 16; ++i) {
        if (op == O::Vec4And || op == O::VecAnd) c[i] = a[i] & b[i];
        else if (op == O::Vec4Or || op == O::VecOr) c[i] = a[i] | b[i];
        else if (op == O::VecAndNot) c[i] = a[i] & ~b[i];
        else c[i] = a[i] ^ b[i];
    }
    return c;
}
void Run(x64::Translator& code, Machine& machine) {
    const auto mxcsr = _mm_getcsr();
    REQUIRE(code.getCode<HaltReason (*)(backend::State*)>()(machine.state) == HaltReason::CallHost);
    REQUIRE(_mm_getcsr() == mxcsr);
}
auto Compile(Program& p, const Config& config, bool vector, bool cfg = true, bool flags = true) {
    auto result = x64::Translator::PrepareFunction(p.function, {}, &config, true, cfg, flags,
        {.vector_cfg_allocation = vector});
    result->Translate(); return result;
}
__attribute__((noinline)) u64 ClobberHighXmm(u64 a, u64 b, u64 c) {
    asm volatile("pxor %%xmm8, %%xmm8\n\tpxor %%xmm9, %%xmm9\n\t"
                 "pxor %%xmm10, %%xmm10\n\tpxor %%xmm11, %%xmm11\n\t"
                 "pxor %%xmm12, %%xmm12\n\tpxor %%xmm13, %%xmm13\n\t"
                 "pxor %%xmm14, %%xmm14\n\tpxor %%xmm15, %%xmm15"
                 ::: "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    return a ^ b ^ c;
}
} // namespace

TEST_CASE("vector CFG owns independent interference and deterministic pressure decisions", "[x64][vector-cfg][plan]") {
    Program p; std::array<ir::Value, 12> inputs;
    for (u32 i = 0; i < inputs.size(); ++i) inputs[i] = p.Load(i * 16);
    auto combined = p.Xor(inputs[0], inputs[1]);
    for (u32 i = 0; i < inputs.size(); ++i) p.Store(256 + i * 16, inputs[i]);
    p.Store(512, combined); p.Finish();
    const auto scalar = x64::CfgRegisterPlan::Build(p.function.body.get(), p.function);
    REQUIRE(scalar.Validate()); REQUIRE_FALSE(scalar.Interferes(inputs[0].Def(), inputs[1].Def()));
    const auto graph = p.Graph(); REQUIRE(graph.Enabled()); REQUIRE_FALSE(graph.HasInterference());
    const auto text = p.function.body->ToString();
    for (const std::vector<unsigned> budget : {std::vector<unsigned>{}, {8}, {15, 8}, {8, 9, 10, 11, 12, 13, 14, 15}}) {
        const auto a = x64::VectorRegisterPlan::Build(graph, &p.function, {budget});
        const auto b = x64::VectorRegisterPlan::Build(graph, &p.function, {budget});
        REQUIRE(a.Validate()); REQUIRE(b.Validate()); REQUIRE(a.Interferes(inputs[0].Def(), inputs[1].Def()));
        u32 assigned{};
        for (auto value : inputs) { REQUIRE(a.Register(value.Def()) == b.Register(value.Def())); assigned += a.Register(value.Def()).has_value(); }
        REQUIRE(assigned <= budget.size()); REQUIRE(a.retains_all_canonical_homes);
        REQUIRE(p.function.body->ToString() == text);
    }
    for (const std::vector<unsigned> invalid : {std::vector<unsigned>{7}, {16}, {8, 8}})
        REQUIRE_FALSE(x64::VectorRegisterPlan::Build(graph, &p.function, {invalid}).Enabled());
}

TEST_CASE("vector CFG preserves complete containers across every narrow producer and raw view", "[x64][vector-cfg][container][native]") {
    const auto config = Configuration();
    for (auto producer : types) for (auto view : types) for (bool cfg : {false, true}) {
        Program p; auto a = p.Load(0, producer), b = p.Load(16, producer);
        auto first = p.Xor(a.SetCastType(view), b.SetCastType(view), producer);
        auto alias = p.as.BitCast(first.SetCastType(view)).SetType(producer);
        auto last = p.Xor(alias.SetCastType(view), b.SetCastType(view), producer);
        p.Store(128, first); p.Store(144, alias); p.Store(160, last); p.Store(176, a); p.Finish();
        auto off = Compile(p, config, false, cfg), on = Compile(p, config, true, cfg);
        REQUIRE(on->VectorPlan().Validate()); REQUIRE(on->Statistics().vector_cfg_cache_read_sites > 0);
        REQUIRE(on->VectorCallFrameBytes() == on->VectorPlan().UsedRegisters().size() * 16);
        for (u32 sample = 0; sample < 8; ++sample) {
            Machine baseline, actual, expected; Bytes left, right;
            for (u32 i = 0; i < 16; ++i) { left[i] = u8(i * 43 + sample); right[i] = u8(i * 7 + 3 * sample); }
            for (auto* machine : {&baseline, &actual, &expected}) { machine->Put(0, left); machine->Put(16, right); }
            expected.Put(128, Xor(left, right)); expected.Put(144, Xor(left, right)); expected.Put(160, left); expected.Put(176, left); expected.Complete();
            Run(*off, baseline); Run(*on, actual); REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
        }
    }
}

TEST_CASE("vector CFG duplicate edge Phi selects the occurrence and captures full raw containers", "[x64][vector-cfg][phi][diamond]") {
    const auto config = Configuration();
    for (auto type : types) {
        Program p; auto a = p.Load(0, type), b = p.Load(16, type), condition = p.Scalar(32);
        auto branch = p.as.Goto(ir::BOOL{condition}); auto* join = p.Label();
        auto phi = p.as.AddPhi(ir::Params{}).SetType(type); Inputs(phi, a, b); p.Target(branch, join);
        p.Store(128, phi); p.Store(144, a); p.Store(160, b); p.Finish();
        const auto graph = p.Graph(); REQUIRE(graph.Enabled());
        ir::Inst* taken{}; ir::Inst* fallthrough{};
        for (const auto& edge : graph.Edges()) for (const auto& copy : edge.copies) if (copy.destination == phi.Def()) {
            REQUIRE(copy.source.IsValue()); (edge.taken ? taken : fallthrough) = copy.source.value.Def();
        }
        REQUIRE(taken); REQUIRE(fallthrough); REQUIRE(taken != fallthrough);
        auto off = Compile(p, config, false), on = Compile(p, config, true);
        REQUIRE(on->Statistics().vector_cfg_phi_publication_sites == 2);
        for (u64 c : {u64{0}, u64{1}}) {
            Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) machine->Put(32, c);
            const auto selected = c ? taken : fallthrough;
            expected.Put(128, expected.Get(selected == a.Def() ? 0 : 16)); expected.Put(144, expected.Get(0)); expected.Put(160, expected.Get(16)); expected.Complete();
            Run(*off, baseline); Run(*on, actual); REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
        }
    }
}

TEST_CASE("vector CFG every eligible binary lowering preserves full lanes and source snapshots", "[x64][vector-cfg][binary][native]") {
    const auto config = Configuration();
    constexpr std::array operations{O::Vec4And, O::VecAnd, O::Vec4Or, O::VecOr, O::VecXor, O::VecAndNot,
        O::Vec4Add, O::VecAdd, O::Vec4Sub, O::VecSub};
    for (auto op : operations) for (u32 bits : {8u, 16u, 32u, 64u}) {
        if (op != O::VecAdd && op != O::VecSub && bits != 32) continue;
        Program p; auto a = p.Load(0, T::V8), b = p.Load(16, T::V16);
        auto* inst = op == O::VecAdd || op == O::VecSub ? p.as.AppendInst(op, a, b, ir::Imm{u8(bits)}) : p.as.AppendInst(op, a, b);
        auto out = ir::Value{inst}.SetType(T::V8); p.Store(128, out); p.Store(144, a); p.Store(160, b); p.Finish();
        auto off = Compile(p, config, false), on = Compile(p, config, true);
        REQUIRE(on->VectorPlan().Register(inst));
        for (u32 sample = 0; sample < 8; ++sample) {
            Machine baseline, actual, expected; Bytes left, right;
            for (u32 i = 0; i < 16; ++i) { left[i] = u8(0xf7 + i * 31 + sample); right[i] = u8(0xf3 + i * 13 + sample); }
            for (auto* machine : {&baseline, &actual, &expected}) { machine->Put(0, left); machine->Put(16, right); }
            expected.Put(128, Binary(left, right, op, bits)); expected.Put(144, left); expected.Put(160, right); expected.Complete();
            Run(*off, baseline); Run(*on, actual); REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
        }
    }
}

TEST_CASE("vector CFG loop backedge Phi swaps capture every source before publication", "[x64][vector-cfg][phi][loop]") {
    const auto config = Configuration();
    Program p; auto a = p.Load(0), b = p.Load(16), trips = p.Scalar(32), entry = p.Jump(); auto* header = p.Label();
    auto first = p.as.AddPhi(ir::Params{}).SetType(T::V128), second = p.as.AddPhi(ir::Params{}).SetType(T::V128);
    auto count = p.as.AddPhi(ir::Params{}).SetType(T::U64);
    auto remaining = p.as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    auto back = p.as.Goto(ir::BOOL{p.as.TestNotZero(remaining).SetType(T::U8)});
    p.Target(entry, header, true); p.Target(back, header); Inputs(first, a, second); Inputs(second, b, first); Inputs(count, trips, remaining);
    p.Store(128, first); p.Store(144, second); p.Store(160, a); p.Store(176, b); p.Finish();
    auto off = Compile(p, config, false), on = Compile(p, config, true);
    REQUIRE(on->VectorPlan().Interferes(first.Def(), second.Def()));
    REQUIRE(on->Statistics().vector_cfg_phi_publication_sites == 4);
    for (u64 n : {u64{1}, u64{2}, u64{3}, u64{20}, u64{101}}) {
        Machine baseline, actual, expected;
        for (auto* machine : {&baseline, &actual, &expected}) machine->Put(32, n);
        expected.Put(128, expected.Get(n % 2 ? 0 : 16)); expected.Put(144, expected.Get(n % 2 ? 16 : 0));
        expected.Put(160, expected.Get(0)); expected.Put(176, expected.Get(16)); expected.Complete();
        Run(*off, baseline); Run(*on, actual); REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
    }
}

TEST_CASE("vector CFG central ABI preserves every live color over direct and wrapped helper calls", "[x64][vector-cfg][helper][native]") {
    const auto config = Configuration();
    for (bool cfg : {false, true}) {
        Program p; std::array<ir::Value, 8> values;
        for (u32 i = 0; i < values.size(); ++i) values[i] = p.Load(i * 16);
        auto a = p.as.LoadImm(ir::Imm{u64{0x12345678}}).SetType(T::U64), b = p.as.LoadImm(ir::Imm{u64{0xabcdef01}}).SetType(T::U64);
        auto direct = p.as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&ClobberHighXmm)}}, a, b, a).SetType(T::U64);
        auto wrapped = ir::Value{p.as.AppendInst(O::Cpuid, a, b, ir::Imm{u64{7}}, ir::Imm{reinterpret_cast<u64>(&ClobberHighXmm)})}.SetType(T::U64);
        p.as.StoreUniform(ir::Uniform{512, T::U64}, direct); p.as.StoreUniform(ir::Uniform{520, T::U64}, wrapped);
        for (u32 i = 0; i < values.size(); ++i) p.Store(256 + i * 16, values[i]); p.Finish();
        auto off = Compile(p, config, false, cfg), on = Compile(p, config, true, cfg);
        REQUIRE(on->VectorPlan().UsedRegisters().size() == 8); REQUIRE(on->VectorCallFrameBytes() == 128);
        REQUIRE(on->Statistics().vector_cfg_helper_save_sites == 16); REQUIRE(on->Statistics().vector_cfg_helper_restore_sites == 16);
        Machine baseline, actual, expected;
        for (u32 i = 0; i < values.size(); ++i) expected.Put(256 + i * 16, expected.Get(i * 16));
        expected.Put(512, u64{0xabcdef01}); expected.Put(520, u64{0x12345678} ^ u64{0xabcdef01} ^ 7); expected.Complete();
        Run(*off, baseline); Run(*on, actual); REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
    }
}

TEST_CASE("vector CFG leaves direct-home and helper-mutated outputs unassigned", "[x64][vector-cfg][eligibility]") {
    Program p; auto direct = p.as.VecLoadConst(ir::Imm{UINT64_MAX}, ir::Imm{u64{42}}).SetType(T::V128);
    auto narrow = p.as.LoadUniform(ir::Uniform{0, T::V64}).SetType(T::V64);
    auto source = p.Load(16);
    auto rounded = p.as.VecFRoundInt(source, source, ir::Imm{u8{32}}, ir::Imm{u8{0}}, ir::Imm{u8{0}}).SetType(T::V128);
    auto fused = p.as.VecFMulAdd(source, source, source, ir::Imm{u8{32}}, ir::Imm{u8{0}}).SetType(T::V128);
    p.Store(128, direct); p.Store(144, narrow); p.Store(160, rounded); p.Store(176, fused);
    p.Finish(); const auto graph = p.Graph(); REQUIRE(graph.Enabled());
    const auto plan = x64::VectorRegisterPlan::Build(graph, &p.function); REQUIRE(plan.Validate());
    REQUIRE_FALSE(plan.Register(direct.Def())); REQUIRE_FALSE(plan.Register(narrow.Def()));
    REQUIRE_FALSE(plan.Register(rounded.Def())); REQUIRE_FALSE(plan.Register(fused.Def()));
}

TEST_CASE("vector CFG raw scalar views and terminal selectors extend vector lifetime", "[x64][vector-cfg][lifetime]") {
    Program p; auto a = p.Load(0), b = p.Load(16), next = p.Xor(a, b);
    p.Store(128, a); p.Store(144, next);
    // This raw scalar access still names a complete vector SSA definition.
    auto raw = p.as.GetOperand(ir::Operand{a.SetCastType(T::U64)}).SetType(T::U64);
    p.as.StoreUniform(ir::Uniform{176, T::U64}, raw);
    p.function.body->SetTerminal(ir::terminal::If{ir::BOOL{a.SetCastType(T::U8)},
        ir::terminal::ReturnToHost{}, ir::terminal::ReturnToHost{}});
    auto graph = p.Graph(); REQUIRE(graph.Enabled());
    auto plan = x64::VectorRegisterPlan::Build(graph, &p.function); REQUIRE(plan.Validate());
    REQUIRE(plan.Interferes(a.Def(), next.Def()));
    REQUIRE(std::find(plan.Blocks().back().live_out.begin(), plan.Blocks().back().live_out.end(), a.Def()) != plan.Blocks().back().live_out.end());
}

TEST_CASE("vector CFG defaults off and SingleInstruction disables all vector colors", "[x64][vector-cfg][policy]") {
    Program p; p.Store(128, p.Load(0)); p.Finish(); const auto config = Configuration();
    x64::Translator ordinary(p.function.body.get(), {}, &config); REQUIRE_FALSE(ordinary.VectorCfgAllocationEnabled());
    x64::Translator single(p.function.body.get(), {}, &config, nullptr, false, x64::ExecutionMode::SingleInstruction,
        false, false, true, true, true, true, true, true, true, false, true);
    REQUIRE_FALSE(single.VectorCfgAllocationEnabled()); REQUIRE_FALSE(single.VectorPlan().Enabled()); REQUIRE(single.VectorCallFrameBytes() == 0);
    x64::VectorRegisterOptions invalid; invalid.physical_registers = {7}; invalid.max_analysis_nodes = 0;
    // Raw requested arguments cannot bypass the effective Ordinary-only policy.
    x64::Translator strict(p.function.body.get(), {}, &config, nullptr, false, x64::ExecutionMode::SingleInstruction,
        false, false, true, true, true, true, true, true, true, true, true, invalid);
    strict.Translate(); REQUIRE_FALSE(strict.NativeCfChainsEnabled()); REQUIRE(strict.CarryPlan().ChainCount() == 0);
    REQUIRE_FALSE(strict.VectorCfgAllocationEnabled()); REQUIRE_FALSE(strict.VectorPlan().Enabled());
    REQUIRE(strict.VectorPlan().Rejection() == x64::VectorPlanRejection::Disabled);
    REQUIRE(strict.VectorPlan().AnalysisStats().nodes == 0); REQUIRE(strict.VectorCallFrameBytes() == 0);
    REQUIRE_FALSE(strict.Statistics().vector_cfg_requested); REQUIRE_FALSE(strict.Statistics().vector_cfg_plan_enabled);
    REQUIRE(strict.Statistics().vector_cfg_cache_read_sites == 0); REQUIRE(strict.Statistics().vector_cfg_helper_save_sites == 0);
}

TEST_CASE("vector CFG analysis budgets reject atomically with distinct diagnostics", "[x64][vector-cfg][budget]") {
    Program p; std::array<ir::Value, 12> values;
    for (u32 i = 0; i < values.size(); ++i) values[i] = p.Load(i * 16);
    auto sum = p.Xor(values[0], values[1]);
    for (u32 i = 0; i < values.size(); ++i) p.Store(256 + i * 16, values[i]);
    p.Store(512, sum); p.Finish(); const auto text = p.function.body->ToString();
    const auto graph = p.Graph(); REQUIRE(graph.Enabled());
    const auto ample = x64::VectorRegisterPlan::Build(graph, &p.function); REQUIRE(ample.Validate());
    REQUIRE(ample.Rejection() == x64::VectorPlanRejection::None);
    const auto stats = ample.AnalysisStats();
    REQUIRE(stats.nodes > 1); REQUIRE(stats.work > 1); REQUIRE(stats.pair_work > 1);
    REQUIRE(stats.interference_storage_entries > 2);
    // Exact measured inclusive limits preserve the original coloring.
    x64::VectorRegisterOptions exact;
    exact.max_analysis_nodes = stats.nodes; exact.max_analysis_work = stats.work;
    exact.max_pair_work = stats.pair_work; exact.max_interference_storage_entries = stats.interference_storage_entries;
    const auto bounded = x64::VectorRegisterPlan::Build(graph, &p.function, exact); REQUIRE(bounded.Validate());
    for (auto value : values) REQUIRE(bounded.Register(value.Def()) == ample.Register(value.Def()));
    for (auto kind : {x64::VectorAnalysisLimit::Nodes, x64::VectorAnalysisLimit::Work,
                      x64::VectorAnalysisLimit::PairWork, x64::VectorAnalysisLimit::InterferenceStorage}) {
        for (u64 tiny : {u64{0}, u64{1}}) {
            x64::VectorRegisterOptions options;
            if (kind == x64::VectorAnalysisLimit::Nodes) options.max_analysis_nodes = tiny;
            if (kind == x64::VectorAnalysisLimit::Work) options.max_analysis_work = tiny;
            if (kind == x64::VectorAnalysisLimit::PairWork) options.max_pair_work = tiny;
            if (kind == x64::VectorAnalysisLimit::InterferenceStorage) options.max_interference_storage_entries = tiny;
            const auto rejected = x64::VectorRegisterPlan::Build(graph, &p.function, options);
            REQUIRE_FALSE(rejected.Enabled()); REQUIRE_FALSE(rejected.Validate());
            REQUIRE(rejected.Rejection() == x64::VectorPlanRejection::AnalysisBudget);
            REQUIRE(rejected.AnalysisStats().exhausted == kind);
            REQUIRE(rejected.Allocations().empty()); REQUIRE(rejected.Blocks().empty());
            REQUIRE(rejected.Edges().empty()); REQUIRE(rejected.UsedRegisters().empty());
            REQUIRE_FALSE(rejected.Register(values[0].Def()));
            REQUIRE(p.function.body->ToString() == text); REQUIRE(graph.MatchesBody(p.function.body.get()));
        }
    }
    auto invalid = exact; invalid.physical_registers = {8, 8}; invalid.max_analysis_nodes = 0;
    const auto bad_registers = x64::VectorRegisterPlan::Build(graph, &p.function, invalid);
    REQUIRE(bad_registers.Rejection() == x64::VectorPlanRejection::InvalidRegisterBudget);
    REQUIRE(bad_registers.AnalysisStats().exhausted == x64::VectorAnalysisLimit::None);
    const auto bad_graph = x64::VectorRegisterPlan::Build(x64::CfgRegisterPlan{}, &p.function, invalid);
    REQUIRE(bad_graph.Rejection() == x64::VectorPlanRejection::InvalidGraph);
}

TEST_CASE("vector CFG set propagation spends bounded work on fixed point passes", "[x64][vector-cfg][budget][loop]") {
    Program p; auto a = p.Load(0), b = p.Load(16), trips = p.Scalar(32), entry = p.Jump(); auto* header = p.Label();
    auto first = p.as.AddPhi(ir::Params{}).SetType(T::V128), second = p.as.AddPhi(ir::Params{}).SetType(T::V128);
    auto count = p.as.AddPhi(ir::Params{}).SetType(T::U64);
    auto remaining = p.as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    auto back = p.as.Goto(ir::BOOL{p.as.TestNotZero(remaining).SetType(T::U8)});
    p.Target(entry, header, true); p.Target(back, header); Inputs(first, a, second); Inputs(second, b, first); Inputs(count, trips, remaining);
    p.Store(128, first); p.Store(144, second); p.Store(160, a); p.Finish();
    const auto text = p.function.body->ToString(); const auto graph = p.Graph(); REQUIRE(graph.Enabled());
    const auto ample = x64::VectorRegisterPlan::Build(graph, &p.function); REQUIRE(ample.Validate());
    REQUIRE(ample.AnalysisStats().fixed_point_passes >= 2);
    x64::VectorRegisterOptions options;
    // Complete all input/edge work, then allow the pass entry but no block work.
    options.max_analysis_work = ample.AnalysisStats().work_before_fixed_point + 1;
    const auto rejected = x64::VectorRegisterPlan::Build(graph, &p.function, options);
    REQUIRE(rejected.Rejection() == x64::VectorPlanRejection::AnalysisBudget);
    REQUIRE(rejected.AnalysisStats().exhausted == x64::VectorAnalysisLimit::Work);
    REQUIRE(rejected.AnalysisStats().fixed_point_passes == 1);
    REQUIRE(rejected.AnalysisStats().pair_work == 0); REQUIRE(rejected.Allocations().empty());
    REQUIRE(p.function.body->ToString() == text);
}

TEST_CASE("vector budget fallback keeps scalar CF and guest flags while matching complete State", "[x64][vector-cfg][budget][native]") {
    const auto config = Configuration();
    for (bool cfg : {false, true}) for (bool resident : {false, true}) {
        Program p; auto a = p.Load(0, T::V8), b = p.Load(16, T::V16), result = p.Xor(a, b, T::V8);
        auto source = p.Scalar(32);
        auto first = ir::Value{p.as.AppendInst(O::Add, source, ir::Operand{ir::Imm{u8{1}}})}.SetType(T::U64);
        p.as.AppendInst(O::SaveFlags, first, ir::Flags::NZCV);
        auto second = ir::Value{p.as.AppendInst(O::Adc, first, ir::Operand{ir::Imm{u8{1}}})}.SetType(T::U64);
        p.as.AppendInst(O::SaveFlags, second, ir::Flags::NZCV);
        p.as.StoreUniform(ir::Uniform{176, T::U64}, second);
        p.Store(128, result); p.Store(144, a); p.Store(160, b); p.Finish();
        const auto compile = [&](bool requested, x64::VectorRegisterOptions budget = {}) {
            auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, cfg, resident,
                {.native_cf_chains = true, .vector_cfg_allocation = requested, .vector_register_options = budget});
            code->Translate(); return code;
        };
        auto off = compile(false), ample = compile(true); REQUIRE(ample->VectorPlan().Enabled());
        REQUIRE(ample->Statistics().vector_cfg_cache_read_sites > 0);
        for (auto kind : {x64::VectorAnalysisLimit::Nodes, x64::VectorAnalysisLimit::Work,
                          x64::VectorAnalysisLimit::PairWork, x64::VectorAnalysisLimit::InterferenceStorage}) {
            for (u64 tiny : {u64{0}, u64{1}}) {
                x64::VectorRegisterOptions budget;
                if (kind == x64::VectorAnalysisLimit::Nodes) budget.max_analysis_nodes = tiny;
                if (kind == x64::VectorAnalysisLimit::Work) budget.max_analysis_work = tiny;
                if (kind == x64::VectorAnalysisLimit::PairWork) budget.max_pair_work = tiny;
                if (kind == x64::VectorAnalysisLimit::InterferenceStorage) budget.max_interference_storage_entries = tiny;
                auto fallback = compile(true, budget);
                REQUIRE(fallback->VectorCfgAllocationEnabled()); REQUIRE_FALSE(fallback->VectorPlan().Enabled());
                REQUIRE(fallback->VectorPlan().Rejection() == x64::VectorPlanRejection::AnalysisBudget);
                REQUIRE(fallback->VectorPlan().AnalysisStats().exhausted == kind);
                REQUIRE(fallback->Statistics().vector_cfg_requested); REQUIRE_FALSE(fallback->Statistics().vector_cfg_plan_enabled);
                REQUIRE(fallback->Statistics().vector_cfg_rejection_reason == static_cast<u32>(x64::VectorPlanRejection::AnalysisBudget));
                REQUIRE(fallback->Statistics().vector_cfg_budget_limit == static_cast<u32>(kind));
                REQUIRE(fallback->VectorCallFrameBytes() == 0); REQUIRE(fallback->FrameSize() == off->FrameSize());
                REQUIRE(fallback->Statistics().vector_cfg_cache_read_sites == 0);
                REQUIRE(fallback->Statistics().vector_cfg_cache_write_sites == 0);
                REQUIRE(fallback->FlagsResident() == resident); REQUIRE(fallback->CfgPlan().Enabled() == cfg);
                REQUIRE(fallback->NativeCfChainsEnabled()); REQUIRE(fallback->CarryPlan().ChainCount() == 1);
                REQUIRE(fallback->Statistics().native_cf_forward_sites == 1);
                REQUIRE(fallback->Statistics().native_cf_forward_sites == off->Statistics().native_cf_forward_sites);
                REQUIRE(fallback->CfgPlan().Allocations().size() == off->CfgPlan().Allocations().size());
                for (u32 i = 0; i < fallback->CfgPlan().Allocations().size(); ++i) {
                    REQUIRE(fallback->CfgPlan().Allocations()[i].producer == off->CfgPlan().Allocations()[i].producer);
                    REQUIRE(fallback->CfgPlan().Allocations()[i].slot == off->CfgPlan().Allocations()[i].slot);
                }
                for (u64 input : {u64{0}, UINT64_MAX, u64{0x7ffffffffffffffe}}) {
                    Machine baseline, actual, cached, expected;
                    for (auto* machine : {&baseline, &actual, &cached, &expected}) machine->Put(32, input);
                    expected.Put(128, Xor(expected.Get(0), expected.Get(16)));
                    expected.Put(144, expected.Get(0)); expected.Put(160, expected.Get(16));
                    const u64 value = input == UINT64_MAX ? 2 : input + 2;
                    expected.Put(176, value);
                    const u64 nzcv = (value >> 63 ? u64{1} << 31 : 0) | (value == 0 ? u64{1} << 30 : 0) |
                                     (input == u64{0x7ffffffffffffffe} ? u64{1} << 28 : 0);
                    expected.state->host_cpu_flags = (initial_flags & ~u64{0xf0000000}) | nzcv; expected.Complete();
                    Run(*off, baseline); Run(*fallback, actual); Run(*ample, cached);
                    REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes); REQUIRE(cached.bytes == expected.bytes);
                }
            }
        }
        auto invalid = x64::VectorRegisterOptions{}; invalid.physical_registers = {7}; invalid.max_analysis_nodes = 0;
        REQUIRE_THROWS_AS(compile(true, invalid), std::runtime_error);
    }
}

TEST_CASE("vector budget fallback preserves the existing flat legacy forwarding path", "[x64][vector-cfg][budget][legacy]") {
    Program p; auto a = p.Load(0), alias = p.Xor(a, a); p.Store(128, alias); p.Finish();
    const auto config = Configuration(); x64::VectorRegisterOptions budget; budget.max_analysis_nodes = 0;
    auto compile = [&](bool requested, x64::VectorRegisterOptions options = {}) {
        auto code = std::make_unique<x64::Translator>(p.function.body.get(), x64::HostFeatures{}, &config, nullptr,
            false, x64::ExecutionMode::Ordinary, false, false, true, true, true, true, true, true, true, false, requested, options);
        code->Translate(); return code;
    };
    auto off = compile(false), fallback = compile(true, budget), ample = compile(true);
    REQUIRE(off->Statistics().vector_forward_write_sites > 0);
    REQUIRE(fallback->Statistics().vector_forward_write_sites == off->Statistics().vector_forward_write_sites);
    REQUIRE(fallback->Statistics().vector_forward_read_sites == off->Statistics().vector_forward_read_sites);
    REQUIRE(fallback->FrameSize() == off->FrameSize()); REQUIRE_FALSE(fallback->VectorPlan().Enabled());
    REQUIRE(ample->VectorPlan().Enabled()); REQUIRE(ample->Statistics().vector_cfg_legacy_home_retentions > 0);
    Machine baseline, actual, cached, expected; expected.Put(128, Bytes{}); expected.Complete();
    Run(*off, baseline); Run(*fallback, actual); Run(*ample, cached);
    REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes); REQUIRE(cached.bytes == expected.bytes);
}

TEST_CASE("vector CFG fault recovery retains committed vectors and abandons cache use", "[x64][vector-cfg][fault]") {
    struct Mapping { void* address = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ~Mapping() { if (address != MAP_FAILED) munmap(address, 4096); } } mapping;
    REQUIRE(mapping.address != MAP_FAILED);
    std::array<Bytes, 2> committed{};
    for (u32 enabled = 0; enabled < 2; ++enabled) {
        backend::AddressSpace space{Configuration()}; backend::ModuleConfig module_config;
        module_config.feature_overrides.Set(FeatureId::x64_vector_cfg, enabled);
        auto module = space.MapModule(0x1000, 0x2000, module_config); Runtime runtime{&space}; Program p;
        auto a = p.Load(0), b = p.Load(16), sum = p.Xor(a, b); p.Store(128, sum);
        p.as.AdvancePC(ir::Imm{u32{4}});
        auto pointer = p.as.LoadImm(ir::Imm{reinterpret_cast<u64>(mapping.address)}).SetType(T::U64);
        auto fault = p.as.LoadMemory(ir::Operand{pointer}).SetType(T::U64); p.as.StoreUniform(ir::Uniform{160, T::U64}, fault);
        p.Store(144, sum); p.Finish();
        REQUIRE(module->Push(p.function.body.get())); REQUIRE(backend::TranslateIR(module, p.function.body) != nullptr);
        Bytes left{}, right{}; for (u32 i = 0; i < 16; ++i) { left[i] = u8(i * 3 + 5); right[i] = u8(i * 17 + 9); }
        auto uniform = runtime.GetUniformBuffer(); std::memcpy(uniform.data(), left.data(), 16); std::memcpy(uniform.data() + 16, right.data(), 16);
        std::memset(uniform.data() + 144, 0xa7, 32); runtime.GetState()->host_cpu_flags = initial_flags;
        runtime.GetState()->guest_addr_limit = UINT64_MAX; runtime.SetLocation(0x1000); const auto mxcsr = _mm_getcsr();
        REQUIRE(runtime.Run() == HaltReason::PageFatal); REQUIRE(runtime.GetLocation() == 0x1004); REQUIRE(_mm_getcsr() == mxcsr);
        std::memcpy(committed[enabled].data(), uniform.data() + 128, 16); REQUIRE(committed[enabled] == Xor(left, right));
        for (u32 i = 144; i < 176; ++i) REQUIRE(uniform[i] == 0xa7);
        REQUIRE(runtime.GetState()->host_cpu_flags == initial_flags);
        REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
        REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
    }
    REQUIRE(committed[0] == committed[1]);
}

namespace {
__attribute__((noinline)) u64 ClobberHighXmmSix(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    return ClobberHighXmm(a, b, c) ^ d ^ e ^ f;
}
u64 Low64(const Bytes& bytes) { u64 value; std::memcpy(&value, bytes.data(), 8); return value; }
}

TEST_CASE("vector CFG Params Lambda and delayed metadata alone retain container lifetimes", "[x64][vector-cfg][lifetime][opaque]") {
    // Each route isolates its one late occurrence. No later StoreUniform of a
    // may accidentally mask a missing Params/Lambda/metadata use census.
    for (u32 route = 0; route < 4; ++route) {
        Program p; auto a = p.Load(0, T::V8); p.Store(128, a);
        ir::Value producer;
        if (route == 3)
            producer = p.as.Add(a.SetCastType(T::U64), ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        auto next = p.as.Zero().SetType(T::V128); p.Store(144, next);
        if (route == 1) {
            ir::Params params; for (u32 i = 0; i < 6; ++i) params.Push(a.SetCastType(T::U64));
            p.as.CallDynamic(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&ClobberHighXmmSix)}}, params).SetType(T::U64);
        } else if (route == 2) {
            p.as.CallLambda(ir::Lambda{a.SetCastType(T::U64)}, next.SetCastType(T::U64),
                next.SetCastType(T::U64), next.SetCastType(T::U64)).SetType(T::U64);
        } else if (route == 3) p.as.AppendInst(O::SaveFlags, producer, ir::Flags::NZCV);
        p.Finish(); const auto graph = p.Graph(); REQUIRE(graph.Enabled());
        const auto plan = x64::VectorRegisterPlan::Build(graph, &p.function); REQUIRE(plan.Validate());
        REQUIRE(plan.Register(a.Def())); REQUIRE(plan.Register(next.Def()));
        REQUIRE(plan.Interferes(a.Def(), next.Def()) == (route != 0));
        if (route != 0) REQUIRE(plan.Register(a.Def()) != plan.Register(next.Def()));
    }
}

TEST_CASE("vector CFG dynamic Params and value Lambda calls preserve all eight colors and flags", "[x64][vector-cfg][helper][params][native]") {
    const auto config = Configuration();
    for (bool cfg : {false, true}) for (bool resident : {false, true}) {
        Program p; std::array<ir::Value, 8> values;
        for (u32 i = 0; i < values.size(); ++i) values[i] = p.Load(i * 16, types[i % types.size()]);
        // Both call targets are raw U64 views of complete vector definitions.
        p.as.SetCarry(p.as.LoadImm(ir::Imm{u8{0}}).SetType(T::U8)); // Change seeded guest C and force actual residency selection.
        auto direct = p.as.CallLambda(ir::Lambda{values[0].SetCastType(T::U64)},
            values[2].SetCastType(T::U64), values[3].SetCastType(T::U64), values[4].SetCastType(T::U64)).SetType(T::U64);
        ir::Params params;
        for (u32 i = 2; i < 8; ++i) params.Push(values[i].SetCastType(T::U64));
        auto dynamic = p.as.CallDynamic(ir::Lambda{values[1].SetCastType(T::U64)}, params).SetType(T::U64);
        p.as.StoreUniform(ir::Uniform{512, T::U64}, direct); p.as.StoreUniform(ir::Uniform{520, T::U64}, dynamic);
        for (u32 i = 0; i < values.size(); ++i) p.Store(256 + i * 16, values[i]); p.Finish();
        auto off = Compile(p, config, false, cfg, resident), on = Compile(p, config, true, cfg, resident);
        REQUIRE(on->FlagsResident() == resident); REQUIRE(off->FlagsResident() == resident);
        REQUIRE(on->VectorPlan().UsedRegisters().size() == 8); REQUIRE(on->VectorCallFrameBytes() == 128);
        REQUIRE(on->Statistics().vector_cfg_helper_save_sites == 16); REQUIRE(on->Statistics().vector_cfg_helper_restore_sites == 16);
        REQUIRE(on->Statistics().vector_cfg_cache_read_sites >= 8);
        for (u32 sample = 0; sample < 4; ++sample) {
            Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) {
                machine->Put(0, reinterpret_cast<u64>(&ClobberHighXmm));
                machine->Put(16, reinterpret_cast<u64>(&ClobberHighXmmSix));
                for (u32 i = 2; i < 8; ++i) {
                    Bytes bytes; for (u32 byte = 0; byte < 16; ++byte) bytes[byte] = u8(i * 59 + byte * 17 + sample * 13);
                    machine->Put(i * 16, bytes);
                }
            }
            u64 three{}, six{};
            for (u32 i = 2; i < 8; ++i) { const auto raw = Low64(expected.Get(i * 16)); six ^= raw; if (i < 5) three ^= raw; }
            expected.Put(512, three); expected.Put(520, six);
            for (u32 i = 0; i < 8; ++i) expected.Put(256 + i * 16, expected.Get(i * 16));
            expected.state->host_cpu_flags = initial_flags & ~(u64{1} << 29); expected.Complete();
            Run(*off, baseline); Run(*on, actual); REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
        }
    }
}

TEST_CASE("vector CFG helper output aliases publish final full containers into their own cache", "[x64][vector-cfg][helper][alias][native]") {
    const auto config = Configuration();
    for (u32 bits : {32u, 64u}) for (bool cfg : {false, true}) {
        Program p; auto a = p.Load(0, T::V8), b = p.Load(16, T::V16), c = p.Load(32, T::V64);
        auto rounded = p.as.VecFRoundInt(a, c, ir::Imm{u8(bits)}, ir::Imm{u8{3}}, ir::Imm{u8{0}}).SetType(T::V128);
        auto round_alias = p.as.BitCast(rounded).SetType(T::V8);
        auto fused = p.as.VecFMulAdd(a, b, c, ir::Imm{u8(bits)}, ir::Imm{u8{0}}).SetType(T::V128);
        auto fused_alias = p.as.GetResult(fused, fused).SetType(T::V16);
        auto mixed = p.Xor(round_alias, fused_alias, T::V8);
        p.Store(128, round_alias); p.Store(144, fused_alias); p.Store(160, rounded); p.Store(176, fused);
        p.Store(192, mixed); p.Store(256, a); p.Store(272, b); p.Store(288, c); p.Finish();
        // Compile's {} requested features force both helper fallback routes.
        auto off = Compile(p, config, false, cfg), on = Compile(p, config, true, cfg);
        REQUIRE_FALSE(on->VectorPlan().Register(rounded.Def())); REQUIRE_FALSE(on->VectorPlan().Register(fused.Def()));
        REQUIRE(on->VectorPlan().Register(round_alias.Def())); REQUIRE(on->VectorPlan().Register(fused_alias.Def()));
        REQUIRE(on->Statistics().helper_call_sites == 2 * (128 / bits) + 1); // Lane helpers plus shared EndMemory.
        REQUIRE(on->Statistics().vector_cfg_helper_save_sites > 0); REQUIRE(on->Statistics().vector_cfg_cache_read_sites > 0);
        for (u32 sample = 0; sample < 4; ++sample) {
            Bytes input{}, multiplier{}, addend{}, truncation{}, product{};
            const auto fill = [&]<class Float>(Float) {
                constexpr u32 lanes = 16 / sizeof(Float);
                const std::array<Float, 4> seeds{Float{1.5}, Float{-2.75}, Float{100.25}, Float{-0.5}};
                for (u32 lane = 0; lane < lanes; ++lane) {
                    const Float x = seeds[(lane + sample) % seeds.size()], y = Float{2}, z = Float{1};
                    // All operands/results are exact dyadic values. Independent
                    // host arithmetic cannot hide a SoftFloat/round helper bug.
                    const Float rounded_model = std::trunc(x), fused_model = x * y + z;
                    std::memcpy(input.data() + lane * sizeof(Float), &x, sizeof(Float));
                    std::memcpy(multiplier.data() + lane * sizeof(Float), &y, sizeof(Float));
                    std::memcpy(addend.data() + lane * sizeof(Float), &z, sizeof(Float));
                    std::memcpy(truncation.data() + lane * sizeof(Float), &rounded_model, sizeof(Float));
                    std::memcpy(product.data() + lane * sizeof(Float), &fused_model, sizeof(Float));
                }
            };
            if (bits == 32) fill(float{}); else fill(double{});
            Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) { machine->Put(0, input); machine->Put(16, multiplier); machine->Put(32, addend); }
            expected.Put(128, truncation); expected.Put(144, product); expected.Put(160, truncation); expected.Put(176, product);
            expected.Put(192, Xor(truncation, product)); expected.Put(256, input); expected.Put(272, multiplier); expected.Put(288, addend); expected.Complete();
            Run(*off, baseline); Run(*on, actual); REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
        }
    }
}
