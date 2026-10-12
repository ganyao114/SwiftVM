#include <array>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/function_plan.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using T = ir::ValueType;
constexpr u32 extent = 1024;
constexpr u32 lanes = 8;
constexpr u64 initial_flags = 0xfedcba9876543219ULL;
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
backend::FunctionPlan Plan() { backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000}); return plan; }
ir::Value Load(ir::Assembler& as, u32 offset, T type = T::U64) { return as.LoadUniform(ir::Uniform{offset, type}).SetType(type); }
void Store(ir::Assembler& as, u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
void Carry(ir::Assembler& as) { as.SetCarry(as.LoadImm(ir::Imm{u64{1}}).SetType(T::U8)); }
void Run(x64::Translator& code, Machine& machine) {
    REQUIRE(code.getCode<HaltReason (*)(backend::State*)>()(machine.state) == HaltReason::CallHost);
    REQUIRE(machine.state->host_cpu_flags == (initial_flags | (u64{1} << 29)));
}
void Same(const Machine& a, const Machine& b) { REQUIRE(std::memcmp(a.bytes.data(), b.bytes.data(), a.bytes.size()) == 0); }
void Model(const Machine& actual, const Machine& expected) {
    REQUIRE(std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, extent) == 0);
}
u64 ScratchSites(const x64::Translator& code) {
    const auto& counts = code.Statistics().At(x64::TransferRegion::PhiScratch);
    return counts.scalar_load_sites + counts.scalar_store_sites + counts.vector_load_sites + counts.vector_store_sites;
}
void ValidHomes(const x64::Translator& code, backend::FunctionPlan& plan) {
    REQUIRE(code.FramePlan().Enabled()); std::unordered_map<u32, std::vector<const ir::Inst*>> homes;
    for (const auto& inst : plan.body->GetInstList()) if (const auto home = code.FramePlan().Home(&inst)) {
        REQUIRE(*home % 32 == 0);
        for (const auto* owner : homes[*home]) {
            // Phi destinations remain unique for direct copies; scalar
            // sources may share only when their complete CFG lives do not.
            REQUIRE(inst.GetOp() != ir::OpCode::AddPhi);
            REQUIRE(owner->GetOp() != ir::OpCode::AddPhi);
            REQUIRE_FALSE(code.CfgPlan().Interferes(&inst, owner));
        }
        homes[*home].push_back(&inst);
    }
}
void LoopLabel(ir::Assembler& as, backend::FunctionPlan& plan, ir::Value edge, ir::Value first) {
    as.BindLabel(edge); auto* label = &plan.body->GetInstList().back();
    plan.body->RemoveInst(label); plan.body->InsertBefore(label, first.Def());
}
void Inputs(ir::Value destination, ir::Value initial, ir::Value backedge) {
    ir::Params inputs; inputs.Push(initial); inputs.Push(backedge); destination.Def()->SetArg(0, inputs);
}
} // namespace

TEST_CASE("direct spilled entry Phi copies normalize immediate widths", "[x64][phi][transfer]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool flags : {false, true}) {
        CAPTURE(type, flags); auto plan = Plan(); ir::Assembler as{plan.body.get()};
        std::array<ir::Value, lanes> phis;
        for (u32 i = 0; i < lanes; ++i) {
            ir::Params inputs; inputs.Push(ir::Imm{UINT64_MAX - i}); phis[i] = as.AddPhi(inputs).SetType(type);
        }
        Carry(as);
        for (u32 i = 0; i < lanes; ++i) Store(as, 128 + i * 16, phis[i].SetCastType(T::V128));
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator legacy{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; legacy.Translate();
        x64::Translator direct{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; direct.Translate();
        ValidHomes(direct, plan); REQUIRE(direct.FlagsResident() == flags);
        REQUIRE(legacy.Statistics().direct_phi_copy_sites == 0);
        REQUIRE(direct.Statistics().direct_phi_copy_sites > 0);
        REQUIRE(ScratchSites(direct) < ScratchSites(legacy));
        REQUIRE(direct.Statistics().At(x64::TransferRegion::PhiScratch).scalar_store_sites < legacy.Statistics().At(x64::TransferRegion::PhiScratch).scalar_store_sites);
        Machine baseline, actual, expected;
        const u32 bits = ir::GetValueSizeByte(type) * 8; const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        for (u32 i = 0; i < lanes; ++i) { expected.Put(128 + i * 16, (UINT64_MAX - i) & mask); expected.Put(136 + i * 16, 0); }
        Run(legacy, baseline); Run(direct, actual); Same(baseline, actual); Model(actual, expected);
    }
}

TEST_CASE("direct spilled Phi diamond preserves simultaneous register and home sources", "[x64][phi][transfer][diamond]") {
    for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto selected = Load(as, 0, T::U8);
        std::array<ir::Value, lanes> first, second, phis;
        for (u32 i = 0; i < lanes; ++i) first[i] = Load(as, 16 + i * 8);
        auto skip = as.Goto(ir::BOOL{selected});
        for (u32 i = 0; i < lanes; ++i) second[i] = as.Add(first[i], ir::Operand{ir::Imm{u64{17 + i}}}).SetType(T::U64);
        as.BindLabel(skip);
        for (u32 i = 0; i < lanes; ++i) { ir::Params inputs; inputs.Push(first[i]); inputs.Push(second[i]); phis[i] = as.AddPhi(inputs).SetType(T::U64); }
        Carry(as);
        for (u32 i = 0; i < lanes; ++i) Store(as, 256 + i * 16, phis[i].SetCastType(T::V128));
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator legacy{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; legacy.Translate();
        x64::Translator direct{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; direct.Translate();
        ValidHomes(direct, plan); REQUIRE(direct.Statistics().direct_phi_copy_sites > 0);
        REQUIRE(ScratchSites(direct) < ScratchSites(legacy));
        REQUIRE(direct.Statistics().phi_value_copy_sites == legacy.Statistics().phi_value_copy_sites);
        REQUIRE(direct.Statistics().At(x64::TransferRegion::PhiScratch).scalar_store_sites < legacy.Statistics().At(x64::TransferRegion::PhiScratch).scalar_store_sites);
        u32 registers{}, spilled{}; for (auto value : first) { if (direct.CfgPlan().Register(value.Def())) ++registers; else ++spilled; }
        REQUIRE(registers > 0); REQUIRE(spilled > 0);
        for (u64 selector : {u64{0}, u64{1}}) for (u64 seed : {u64{0}, UINT64_MAX - 23, u64{0x8123456789abcdef}}) {
            CAPTURE(flags, selector, seed); Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) {
                machine->Put(0, selector); for (u32 i = 0; i < lanes; ++i) machine->Put(16 + i * 8, seed + i * 29);
            }
            for (u32 i = 0; i < lanes; ++i) { expected.Put(256 + i * 16, seed + i * 29 + (selector ? 0 : 17 + i)); expected.Put(264 + i * 16, 0); }
            Run(legacy, baseline); Run(direct, actual); Same(baseline, actual); Model(actual, expected);
        }
    }
}

TEST_CASE("direct Phi duplicate structural edges retain both incoming transfers", "[x64][phi][transfer][duplicate-edge]") {
    for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto selector = Load(as, 0, T::U8);
        std::array<ir::Value, lanes> seeds;
        for (u32 i = 0; i < lanes; ++i) seeds[i] = Load(as, 16 + i * 8);
        auto edge = as.Goto(ir::BOOL{selector}); auto* join = as.AppendInst(ir::OpCode::BindLabel);
        join->SetArg(0, ir::Value{join}); plan.targets[edge.Def()] = join;
        std::array<ir::Value, lanes> phis;
        for (u32 i = 0; i < lanes; ++i) { ir::Params inputs; inputs.Push(seeds[i]); inputs.Push(seeds[i]); phis[i] = as.AddPhi(inputs).SetType(T::U64); }
        Carry(as); for (u32 i = 0; i < lanes; ++i) Store(as, 256 + i * 16, phis[i].SetCastType(T::V128));
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator legacy{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; legacy.Translate();
        x64::Translator direct{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; direct.Translate();
        REQUIRE(direct.CfgPlan().Enabled()); ValidHomes(direct, plan);
        REQUIRE(direct.Statistics().phi_value_copy_sites == 2 * lanes); REQUIRE(direct.Statistics().direct_phi_copy_sites > 0);
        for (u64 selector_value : {u64{0}, u64{1}}) {
            Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) { machine->Put(0, selector_value); for (u32 i = 0; i < lanes; ++i) machine->Put(16 + i * 8, UINT64_MAX - i * 131); }
            for (u32 i = 0; i < lanes; ++i) { expected.Put(256 + i * 16, UINT64_MAX - i * 131); expected.Put(264 + i * 16, 0); }
            Run(legacy, baseline); Run(direct, actual); Same(baseline, actual); Model(actual, expected);
        }
    }
}

TEST_CASE("Phi swap three way cycle and self copies retain parallel snapshots under pressure", "[x64][phi][transfer][cycle]") {
    for (u32 cycle : {u32{1}, u32{2}, u32{3}}) for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto trips = Load(as, 0); std::array<ir::Value, lanes> seeds, phis;
        for (u32 i = 0; i < lanes; ++i) seeds[i] = Load(as, 16 + i * 8);
        for (auto& phi : phis) phi = as.AddPhi(ir::Params{}).SetType(T::U64);
        auto count = as.AddPhi(ir::Params{}).SetType(T::U64); Carry(as);
        auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64); auto edge = as.Goto(as.TestNotZero(next)); LoopLabel(as, plan, edge, phis.front());
        for (u32 i = 0; i < lanes; ++i) Inputs(phis[i], seeds[i], phis[i < cycle ? (i + 1) % cycle : i]);
        Inputs(count, trips, next);
        for (u32 i = 0; i < lanes; ++i) Store(as, 256 + i * 16, phis[i].SetCastType(T::V128)); Store(as, 400, next);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator legacy{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; legacy.Translate();
        x64::Translator direct{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; direct.Translate();
        ValidHomes(direct, plan); REQUIRE(direct.Statistics().direct_phi_copy_sites > 0);
        REQUIRE(direct.Statistics().direct_phi_copy_sites < direct.Statistics().phi_value_copy_sites);
        REQUIRE(direct.Statistics().At(x64::TransferRegion::PhiScratch).scalar_store_sites >= 2 * lanes);
        for (u64 iterations : {u64{1}, u64{2}, u64{3}, u64{4}, u64{11}}) {
            CAPTURE(cycle, flags, iterations); Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) { machine->Put(0, iterations); for (u32 i = 0; i < lanes; ++i) machine->Put(16 + i * 8, 0xf123456789abc000ULL + i * 37); }
            for (u32 i = 0; i < lanes; ++i) { const u32 source = i < cycle ? (i + iterations - 1) % cycle : i; expected.Put(256 + i * 16, 0xf123456789abc000ULL + source * 37); expected.Put(264 + i * 16, 0); }
            expected.Put(400, 0); Run(legacy, baseline); Run(direct, actual); Same(baseline, actual); Model(actual, expected);
        }
    }
}

TEST_CASE("mixed masked scalar and raw vector Phi copies capture old homes before publication", "[x64][phi][transfer][raw][mask]") {
    for (auto narrow : {T::U8, T::U32}) for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto trips = Load(as, 0);
        auto seed_a = Load(as, 16); auto seed_b = Load(as, 24); auto seed_raw = Load(as, 32, T::V128);
        auto a = as.AddPhi(ir::Params{}).SetType(narrow); auto b = as.AddPhi(ir::Params{}).SetType(T::U64);
        auto raw = as.AddPhi(ir::Params{}).SetType(T::V128); auto self = as.AddPhi(ir::Params{}).SetType(T::V128);
        std::array<ir::Value, lanes> pressure;
        for (auto& phi : pressure) phi = as.AddPhi(ir::Params{}).SetType(T::U64);
        auto count = as.AddPhi(ir::Params{}).SetType(T::U64); Carry(as);
        auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64); auto edge = as.Goto(as.TestNotZero(next)); LoopLabel(as, plan, edge, a);
        Inputs(a, seed_a.SetCastType(narrow), b.SetCastType(narrow)); Inputs(b, seed_b, a.SetCastType(T::U64));
        Inputs(raw, seed_raw, a.SetCastType(T::V128)); Inputs(self, seed_raw, self); Inputs(count, trips, next);
        for (u32 i = 0; i < lanes; ++i) {
            ir::Params inputs; inputs.Push(ir::Imm{u64{77 + i}});
            inputs.Push(i < lanes / 2 ? pressure[i] : b.SetCastType(T::U64)); pressure[i].Def()->SetArg(0, inputs);
        }
        Store(as, 128, a.SetCastType(T::V128)); Store(as, 144, b.SetCastType(T::V128)); Store(as, 160, raw); Store(as, 176, self);
        for (u32 i = 0; i < lanes; ++i) Store(as, 256 + i * 16, pressure[i].SetCastType(T::V128));
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator legacy{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; legacy.Translate();
        x64::Translator direct{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; direct.Translate();
        ValidHomes(direct, plan); REQUIRE(direct.CfgPlan().RequiresCanonicalHome(a.Def()));
        u32 independent_spills{};
        for (u32 i = lanes / 2; i < lanes; ++i) if (!direct.CfgPlan().Register(pressure[i].Def())) {
            REQUIRE(direct.FramePlan().Home(pressure[i].Def())); ++independent_spills;
        }
        REQUIRE(independent_spills > 0);
        REQUIRE(direct.Statistics().At(x64::TransferRegion::PhiScratch).vector_store_sites >= 4);
        REQUIRE(direct.Statistics().direct_phi_copy_sites > 0);
        REQUIRE(direct.Statistics().direct_phi_copy_sites < direct.Statistics().phi_value_copy_sites + direct.Statistics().phi_immediate_copy_sites);
        const u64 mask = narrow == T::U8 ? 255 : UINT32_MAX;
        for (u64 iterations : {u64{1}, u64{2}, u64{3}, u64{4}, u64{11}}) {
            CAPTURE(narrow, flags, iterations); Machine baseline, actual, expected;
            constexpr u64 left = 0xabcde123456789ffULL, right = 0xfedcba98765432a5ULL, raw_low = 0x8877665544332211ULL, raw_high = 0x123456789abcdef0ULL;
            for (auto* machine : {&baseline, &actual, &expected}) { machine->Put(0, iterations); machine->Put(16, left); machine->Put(24, right); machine->Put(32, raw_low); machine->Put(40, raw_high); }
            u64 model_a = left & mask, model_b = right, model_raw = raw_low, model_high = raw_high, old_b{};
            for (u64 step = 1; step < iterations; ++step) { const u64 old_a = model_a; old_b = model_b; model_a = old_b & mask; model_b = old_a; model_raw = old_a; model_high = 0; }
            expected.Put(128, model_a); expected.Put(136, 0); expected.Put(144, model_b); expected.Put(152, 0);
            expected.Put(160, model_raw); expected.Put(168, model_high); expected.Put(176, raw_low); expected.Put(184, raw_high);
            for (u32 i = 0; i < lanes; ++i) { expected.Put(256 + i * 16, i < lanes / 2 || iterations == 1 ? 77 + i : old_b); expected.Put(264 + i * 16, 0); }
            Run(legacy, baseline); Run(direct, actual); Same(baseline, actual); Model(actual, expected);
        }
    }
}
