#include <array>
#include <cstring>
#include <set>
#include <sys/mman.h>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/x64/sparse_frame_plan.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/backend/x64/flags_usage.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace swift::runtime::backend::x64 {
struct TransferStatisticsProbe {
    static u32 Home(const Translator& translator, ir::Inst* inst) { return translator.Home(inst); }
};
}
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
using D = x64::SparseHomeDecision;
constexpr u32 extent = 1024;
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() { for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 13); state->host_cpu_flags = 0xfedcba9876543219ULL; }
    ~Machine() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
};
backend::FunctionPlan Plan() { backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000}); return plan; }
ir::Value Imm(ir::Assembler& as, u64 value) { return as.LoadImm(ir::Imm{value}).SetType(T::U64); }
ir::Value Load(ir::Assembler& as, u32 offset, T type = T::U64) { return as.LoadUniform(ir::Uniform{offset, type}).SetType(type); }
void Store(ir::Assembler& as, u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
Config Configuration() { return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true, .backend_isa = kX86_64, .uniform_buffer_size = extent, .static_program = true, .stack_alignment = 16}; }
void Run(x64::Translator& code, Machine& state) {
    const auto mxcsr = _mm_getcsr(); REQUIRE(code.getCode<HaltReason (*)(backend::State*)>()(state.state) == HaltReason::CallHost); REQUIRE(_mm_getcsr() == mxcsr);
}
void Same(const Machine& a, const Machine& b) { REQUIRE(std::memcmp(a.bytes.data(), b.bytes.data(), a.bytes.size()) == 0); }
void Inventory(const x64::Translator& translator, backend::FunctionPlan& plan) {
    const auto& frame = translator.FramePlan(); REQUIRE(frame.Enabled()); std::set<u32> offsets; u32 retained{}, control{}, registers{}, unused{}, rematerialized{};
    std::unordered_set<ir::Inst*> flag_producers;
    for (auto& inst : plan.body->GetInstList()) if (inst.GetOp() == O::SaveFlags || inst.GetOp() == O::BranchOnlyFlags) {
        auto* producer = inst.GetArg<ir::Value>(0).Def();
        if (x64::ClassifyProducerFlags(producer->GetOp(), producer->ReturnType(), inst.GetArg<ir::Flags>(1)).needs_metadata)
            flag_producers.insert(producer);
    }
    REQUIRE(frame.Validate(plan.body.get(), &plan, translator.CfgPlan(), flag_producers, frame.RematerializationEnabled()));
    for (const auto& inst : plan.body->GetInstList()) {
        const auto home = frame.Home(&inst);
        switch (frame.Decision(&inst)) {
            case D::Retained:
                REQUIRE(home.has_value()); REQUIRE(*home % 32 == 0); offsets.insert(*home); ++retained; break;
            case D::Control: REQUIRE_FALSE(home); ++control; break;
            case D::RegisterOnly: REQUIRE_FALSE(home); REQUIRE(translator.CfgPlan().Register(&inst)); REQUIRE_FALSE(translator.CfgPlan().RequiresCanonicalHome(&inst)); ++registers; break;
            case D::UnusedScalar: REQUIRE_FALSE(home); ++unused; break;
            case D::Rematerialized: REQUIRE_FALSE(home); REQUIRE(frame.RematerializedValue(&inst)); REQUIRE_FALSE(translator.CfgPlan().Register(&inst)); ++rematerialized; break;
        }
        REQUIRE(frame.RematerializedValue(&inst).has_value() == (frame.Decision(&inst) == D::Rematerialized));
    }
    u32 shared{};
    for (auto offset : offsets) {
        std::vector<const ir::Inst*> owners;
        for (const auto& inst : plan.body->GetInstList()) if (frame.Home(&inst) == offset) owners.push_back(&inst);
        if (owners.size() >= 2) ++shared;
        for (size_t i = 0; i < owners.size(); ++i) for (size_t j = 0; j < i; ++j)
            REQUIRE_FALSE(translator.CfgPlan().Interferes(owners[i], owners[j]));
    }
    const auto& stats = frame.Statistics(); REQUIRE(stats.retained_homes == retained); REQUIRE(stats.home_count == offsets.size());
    REQUIRE(stats.reused_scalar_homes == retained - offsets.size()); REQUIRE(stats.shared_scalar_home_slots == shared);
    REQUIRE(stats.omitted_control == control); REQUIRE(stats.omitted_register == registers); REQUIRE(stats.omitted_unused == unused); REQUIRE(stats.home_bytes == offsets.size() * 32);
    REQUIRE(stats.omitted_rematerialized == rematerialized);
    REQUIRE(translator.FrameSize() >= stats.home_bytes + 24);
}
u64 Identity(u64 first, u64, u64) { asm volatile("xor %%r11d, %%r11d" ::: "r11", "cc"); return first; }
ir::Inst* Label(backend::FunctionPlan& plan) {
    auto* label = new ir::Inst(O::BindLabel); label->SetArgs(ir::Value{label}); plan.body->AppendInst(label); return label;
}
auto SpilledPlan(backend::FunctionPlan& plan) {
    auto registers = x64::CfgRegisterPlan::Build(plan.body.get(), plan, {.physical_registers = {}});
    REQUIRE(registers.Enabled()); REQUIRE(registers.Validate()); return registers;
}
auto Frame(backend::FunctionPlan& plan, const x64::CfgRegisterPlan& registers,
           const std::unordered_set<ir::Inst*>& flags = {}, bool rematerialization = false) {
    // Pool tests retain their original representation; rematerialization tests
    // explicitly enable the independent final representation policy.
    auto frame = x64::SparseFramePlan::Build(plan.body.get(), &plan, registers, flags, rematerialization);
    REQUIRE(frame.Enabled()); REQUIRE(frame.Validate(plan.body.get(), &plan, registers, flags, rematerialization));
    REQUIRE(frame.RematerializationEnabled() == rematerialization);
    REQUIRE(frame.Statistics().home_bytes == frame.Statistics().home_count * 32);
    REQUIRE(frame.Statistics().reused_scalar_homes == frame.Statistics().retained_homes - frame.Statistics().home_count);
    return frame;
}
} // namespace

TEST_CASE("scalar home pool respects binary read write phases and later live uses", "[x64][frame][sparse][reuse][phase]") {
    for (auto op : {O::Add, O::Sub, O::Adc, O::Sbb, O::And, O::Or, O::Xor, O::AndNot}) {
        for (bool later : {false, true}) {
            auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto input = Load(as, 0);
            auto* output = as.AppendInst(op, input, ir::Operand{ir::Imm{u64{7}}}); output->SetReturn(T::U64);
            Store(as, 8, ir::Value{output}); if (later) Store(as, 16, input);
            plan.body->SetTerminal(ir::terminal::ReturnToHost{});
            const auto registers = SpilledPlan(plan); const auto frame = Frame(plan, registers);
            REQUIRE(registers.Interferes(input.Def(), output) == later);
            REQUIRE((frame.Home(input.Def()) == frame.Home(output)) == !later);
            REQUIRE(registers.CanReuseBinaryInput(output, input.Def()) == !later);
            REQUIRE(frame.Statistics().reused_scalar_homes == (later ? 0 : 1));
            REQUIRE(frame.Statistics().shared_scalar_home_slots == (later ? 0 : 1));
        }
    }
}

TEST_CASE("scalar home pool checks every historical owner across mutually exclusive paths", "[x64][frame][sparse][reuse][diamond]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto condition = Load(as, 0);
    auto choose = as.Goto(ir::BOOL{condition}); auto a = Load(as, 8);
    auto to_a = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
    auto* b_label = Label(plan); auto b = Load(as, 16); Store(as, 24, b);
    auto to_end = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
    auto* a_label = Label(plan); auto c = Load(as, 32); Store(as, 40, a); Store(as, 48, c);
    auto* end = Label(plan); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
    plan.targets[choose.Def()] = b_label; plan.targets[to_a.Def()] = a_label; plan.stops.insert(to_a.Def());
    plan.targets[to_end.Def()] = end; plan.stops.insert(to_end.Def());
    const auto registers = SpilledPlan(plan); const auto frame = Frame(plan, registers);
    REQUIRE_FALSE(registers.Interferes(a.Def(), b.Def())); REQUIRE_FALSE(registers.Interferes(b.Def(), c.Def()));
    REQUIRE(registers.Interferes(a.Def(), c.Def()));
    REQUIRE(frame.Home(a.Def()) == frame.Home(b.Def())); REQUIRE(frame.Home(a.Def()) != frame.Home(c.Def()));
    // Checking only the most recent owner B would wrongly let C overwrite A.
    REQUIRE(frame.Statistics().reused_scalar_homes > 0);
}

TEST_CASE("scalar home pool preserves loop live values and unique Phi destinations", "[x64][frame][sparse][reuse][loop][phi]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto invariant = Load(as, 0), seed = Imm(as, 4);
    auto enter = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
    auto* header = Label(plan); auto phi = as.AddPhi(ir::Params{}).SetType(T::U64);
    auto next = as.Sub(phi, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    Store(as, 8, invariant); Store(as, 16, next); auto back = as.Goto(as.TestNotZero(next).SetType(T::U8));
    ir::Params inputs; inputs.Push(seed); inputs.Push(next); phi.Def()->SetArg(0, inputs);
    plan.targets[enter.Def()] = header; plan.stops.insert(enter.Def()); plan.targets[back.Def()] = header;
    Store(as, 24, invariant); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
    const auto registers = SpilledPlan(plan); const auto frame = Frame(plan, registers);
    REQUIRE(registers.Interferes(invariant.Def(), next.Def())); REQUIRE(frame.Home(invariant.Def()) != frame.Home(next.Def()));
    for (const auto& inst : plan.body->GetInstList()) if (&inst != phi.Def() && frame.Home(&inst))
        REQUIRE(frame.Home(&inst) != frame.Home(phi.Def()));
}

TEST_CASE("scalar home pool excludes raw late flags and unreachable retained definitions", "[x64][frame][sparse][reuse][exclusions]") {
    for (u32 decoration = 0; decoration < 3; ++decoration) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto source = Load(as, 0);
        auto output = as.Add(source, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        if (decoration == 0) output.Def()->SetArg(4, ir::Value{source.Def()}.SetCastType(T::V128));
        else if (decoration == 1) output.Def()->SetArg(4, ir::Lambda{source});
        else { ir::Params params; params.Push(source); output.Def()->SetArg(4, params); }
        Store(as, 8, output); auto after = Load(as, 16); Store(as, 24, after);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto registers = SpilledPlan(plan); const auto frame = Frame(plan, registers);
        REQUIRE(registers.RequiresFullContainer(source.Def())); REQUIRE(registers.Interferes(source.Def(), output.Def()));
        for (const auto& inst : plan.body->GetInstList()) if (&inst != source.Def() && frame.Home(&inst))
            REQUIRE(frame.Home(&inst) != frame.Home(source.Def()));
    }
    SECTION("native flags metadata keeps an independent whole body home") {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto sum = as.Add(Load(as, 0), ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64);
        Store(as, 8, sum); auto other = Load(as, 16); Store(as, 24, other); as.SaveFlags(sum, ir::Flags::CV);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{}); const auto registers = SpilledPlan(plan);
        const std::unordered_set<ir::Inst*> flags{sum.Def()}; const auto frame = Frame(plan, registers, flags);
        REQUIRE_FALSE(registers.RequiresFullContainer(sum.Def()));
        for (const auto& inst : plan.body->GetInstList()) if (&inst != sum.Def() && frame.Home(&inst))
            REQUIRE(frame.Home(&inst) != frame.Home(sum.Def()));
        REQUIRE_FALSE(frame.Validate(plan.body.get(), &plan, registers, {}, false));
    }
    SECTION("unreachable and zero use retained producers cannot enter the pool") {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto skip = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
        auto dead = Load(as, 0); Store(as, 8, dead); auto* label = Label(plan);
        auto live = Load(as, 16); Store(as, 24, live); plan.targets[skip.Def()] = label; plan.stops.insert(skip.Def());
        // BitCast is outside the scalar Write inventory even when unused.
        auto unused = as.BitCast(live).SetType(T::U64);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{}); const auto registers = SpilledPlan(plan); const auto frame = Frame(plan, registers);
        REQUIRE(frame.Home(dead.Def())); REQUIRE(frame.Home(unused.Def()));
        for (auto pinned : {dead.Def(), unused.Def()}) for (const auto& inst : plan.body->GetInstList())
            if (&inst != pinned && frame.Home(&inst)) REQUIRE(frame.Home(&inst) != frame.Home(pinned));
    }
}

TEST_CASE("scalar home validation rejects changed eligibility and newly conflicting owners", "[x64][frame][sparse][reuse][validation]") {
    for (bool raw : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto input = Load(as, 0);
        auto output = as.Add(input, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64); Store(as, 8, output);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto original = SpilledPlan(plan); const auto frame = Frame(plan, original);
        REQUIRE(frame.Home(input.Def()) == frame.Home(output.Def()));
        if (raw) output.Def()->SetArg(4, ir::Value{input.Def()}.SetCastType(T::V128));
        else Store(as, 16, input);
        const auto current = SpilledPlan(plan);
        REQUIRE_FALSE(frame.Validate(plan.body.get(), &plan, current, {}, false));
        const auto rebuilt = Frame(plan, current);
        REQUIRE(rebuilt.Home(input.Def()) != rebuilt.Home(output.Def()));
    }
}

TEST_CASE("scalar rematerialization normalizes the producer bit pattern and preserves allocation", "[x64][frame][sparse][rematerialization][width]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64, T::S8, T::S16, T::S32, T::S64}) {
        for (u64 bits : {u64{0}, u64{0x87654321fedcba98}, u64{UINT64_MAX}}) {
            auto plan = Plan(); ir::Assembler as{plan.body.get()};
            // Immediate payload width deliberately differs from the return.
            auto value = as.LoadImm(ir::Imm{bits}).SetType(type); Store(as, 0, value);
            plan.body->SetTerminal(ir::terminal::ReturnToHost{});
            const auto registers = SpilledPlan(plan);
            const auto off = Frame(plan, registers, {}, false), on = Frame(plan, registers, {}, true);
            const u32 width = ir::GetValueSizeByte(type) * 8;
            const u64 expected = width == 64 ? bits : bits & ((u64{1} << width) - 1);
            REQUIRE(registers.RequiresCanonicalHome(value.Def())); REQUIRE_FALSE(registers.RequiresFullContainer(value.Def()));
            REQUIRE(off.Decision(value.Def()) == D::Retained); REQUIRE(off.Home(value.Def())); REQUIRE_FALSE(off.RematerializedValue(value.Def()));
            REQUIRE(on.Decision(value.Def()) == D::Rematerialized); REQUIRE_FALSE(on.Home(value.Def()));
            REQUIRE(on.RematerializedValue(value.Def()).has_value()); REQUIRE(*on.RematerializedValue(value.Def()) == expected);
            REQUIRE(on.Statistics().omitted_rematerialized == 1); REQUIRE(off.Statistics().omitted_rematerialized == 0);
            REQUIRE(on.Statistics().home_count == 0); REQUIRE(off.Statistics().home_count == 1);
            REQUIRE_FALSE(on.Validate(plan.body.get(), &plan, registers, {}, false));
            REQUIRE_FALSE(off.Validate(plan.body.get(), &plan, registers, {}, true));
        }
    }
    for (auto immediate : {ir::Imm{s8{-1}}, ir::Imm{s16{-1}}, ir::Imm{s32{-1}}, ir::Imm{s64{-1}}}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()};
        auto value = as.LoadImm(immediate).SetType(T::S64); Store(as, 0, value);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto registers = SpilledPlan(plan); const auto frame = Frame(plan, registers, {}, true);
        // Imm::Get follows payload width; widening a signed producer does not
        // change the historical EmitLoadImm/Write zero-extended bit pattern.
        REQUIRE(frame.RematerializedValue(value.Def()) == immediate.Get());
    }
    x64::SparseFramePlan disabled; REQUIRE_FALSE(disabled.RematerializationEnabled());
    REQUIRE_FALSE(disabled.RematerializedValue(nullptr));
}

TEST_CASE("scalar rematerialization rejects every populated extra physical argument", "[x64][frame][sparse][rematerialization][extras]") {
    for (u32 slot = 1; slot < ir::Inst::max_args; ++slot) for (u32 kind = 0; kind < 4; ++kind) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto source = Load(as, 0); auto value = Imm(as, 7);
        if (kind == 0) value.Def()->SetArg(slot, source);
        else if (kind == 1) value.Def()->SetArg(slot, ir::Lambda{source});
        else if (kind == 2) { ir::Params params; params.Push(source); value.Def()->SetArg(slot, params); }
        else value.Def()->SetArg(slot, ir::Imm{u64{9}});
        Store(as, 8, value); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto registers = SpilledPlan(plan); const auto frame = Frame(plan, registers, {}, true);
        REQUIRE(frame.Decision(value.Def()) == D::Retained); REQUIRE(frame.Home(value.Def()));
        REQUIRE_FALSE(frame.RematerializedValue(value.Def())); REQUIRE(frame.Statistics().omitted_rematerialized == 0);
    }
}

TEST_CASE("scalar rematerialization retains raw opaque vector and explicit flags homes", "[x64][frame][sparse][rematerialization][exclusions]") {
    for (u32 kind = 0; kind < 4; ++kind) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto value = Imm(as, 0x123456789abcdef0);
        auto output = as.Add(value, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        if (kind == 0) output.Def()->SetArg(4, ir::Value{value.Def()}.SetCastType(T::V128));
        else if (kind == 1) output.Def()->SetArg(4, ir::Lambda{value});
        else if (kind == 2) { ir::Params params; params.Push(value); output.Def()->SetArg(4, params); }
        else as.BitCast(value).SetType(T::U64);
        Store(as, 8, output); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto registers = SpilledPlan(plan); const auto frame = Frame(plan, registers, {}, true);
        REQUIRE(registers.RequiresFullContainer(value.Def())); REQUIRE(frame.Decision(value.Def()) == D::Retained);
        REQUIRE(frame.Home(value.Def())); REQUIRE_FALSE(frame.RematerializedValue(value.Def()));
    }
    SECTION("explicit flag authority excludes even an otherwise pure immediate") {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto value = Imm(as, 9); Store(as, 0, value);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{}); const auto registers = SpilledPlan(plan);
        const std::unordered_set<ir::Inst*> flags{value.Def()}; const auto frame = Frame(plan, registers, flags, true);
        REQUIRE(frame.Decision(value.Def()) == D::Retained); REQUIRE(frame.Home(value.Def())); REQUIRE_FALSE(frame.RematerializedValue(value.Def()));
        REQUIRE_FALSE(frame.Validate(plan.body.get(), &plan, registers, {}, true));
    }
    SECTION("vector return is not a scalar reconstruction") {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto value = as.LoadImm(ir::Imm{u64{9}}).SetType(T::V128);
        Store(as, 0, value); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto registers = SpilledPlan(plan); const auto frame = Frame(plan, registers, {}, true);
        REQUIRE(frame.Decision(value.Def()) == D::Retained); REQUIRE(frame.Home(value.Def())); REQUIRE_FALSE(frame.RematerializedValue(value.Def()));
    }
}

TEST_CASE("scalar rematerialization requires reachable unassigned used producers", "[x64][frame][sparse][rematerialization][authority]") {
    SECTION("colored immediate keeps its existing representation") {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto value = Imm(as, 7); Store(as, 0, value);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto registers = x64::CfgRegisterPlan::Build(plan.body.get(), plan); REQUIRE(registers.Register(value.Def()));
        const auto off = Frame(plan, registers, {}, false), on = Frame(plan, registers, {}, true);
        REQUIRE(on.Decision(value.Def()) == D::RegisterOnly); REQUIRE(on.Decision(value.Def()) == off.Decision(value.Def()));
        REQUIRE_FALSE(on.RematerializedValue(value.Def())); REQUIRE(on.Statistics().home_bytes == off.Statistics().home_bytes);
    }
    SECTION("unreachable used immediate and unused immediate cannot rematerialize") {
        auto plan = Plan(); ir::Assembler as{plan.body.get()};
        auto skip = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
        auto dead = Imm(as, 11); Store(as, 0, dead); auto* label = Label(plan); auto unused = Imm(as, 13);
        plan.targets[skip.Def()] = label; plan.stops.insert(skip.Def()); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto registers = SpilledPlan(plan); const auto frame = Frame(plan, registers, {}, true);
        REQUIRE(frame.Decision(dead.Def()) == D::Retained); REQUIRE(frame.Home(dead.Def())); REQUIRE_FALSE(frame.RematerializedValue(dead.Def()));
        REQUIRE(frame.Decision(unused.Def()) == D::UnusedScalar); REQUIRE_FALSE(frame.RematerializedValue(unused.Def()));
    }
    SECTION("audited scalar terminal selector uses reconstruction") {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto value = as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8);
        plan.body->SetTerminal(ir::terminal::If{ir::BOOL{value}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}});
        const auto registers = SpilledPlan(plan); const auto frame = Frame(plan, registers, {}, true);
        REQUIRE_FALSE(registers.RequiresFullContainer(value.Def())); REQUIRE(frame.Decision(value.Def()) == D::Rematerialized);
        REQUIRE(frame.RematerializedValue(value.Def()) == u64{1});
    }
    SECTION("LoadImm with a non immediate physical primary is retained") {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto source = Load(as, 0); auto value = Imm(as, 7);
        value.Def()->SetArg(0, source); Store(as, 8, value); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto registers = SpilledPlan(plan); const auto frame = Frame(plan, registers, {}, true);
        REQUIRE(frame.Decision(value.Def()) == D::Retained); REQUIRE(frame.Home(value.Def())); REQUIRE_FALSE(frame.RematerializedValue(value.Def()));
    }
}

TEST_CASE("scalar rematerialization validation recomputes normalized values against fresh authority", "[x64][frame][sparse][rematerialization][validation]") {
    for (bool extra : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto value = as.LoadImm(ir::Imm{u64{0x1234}}).SetType(T::U8);
        Store(as, 0, value); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        const auto original = SpilledPlan(plan); const auto frame = Frame(plan, original, {}, true);
        REQUIRE(frame.RematerializedValue(value.Def()) == u64{0x34});
        if (extra) value.Def()->SetArg(4, ir::Imm{u64{3}});
        else value.Def()->SetArg(0, ir::Imm{u64{0x1235}});
        // Caller rebuilds the complete allocation for changed final IR.
        const auto current = SpilledPlan(plan); REQUIRE_FALSE(frame.Validate(plan.body.get(), &plan, current, {}, true));
        const auto rebuilt = Frame(plan, current, {}, true);
        if (extra) { REQUIRE_FALSE(rebuilt.RematerializedValue(value.Def())); REQUIRE(rebuilt.Home(value.Def())); }
        else REQUIRE(rebuilt.RematerializedValue(value.Def()) == u64{0x35});
    }
}

TEST_CASE("sparse frame removes control holes and assigned scalar homes without slot reuse", "[x64][frame][sparse]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto value = Load(as, 0); auto* control = as.AppendInst(O::Nop);
    for (u32 i = 0; i < 24; ++i) { as.AppendInst(O::Nop); value = as.Add(value, ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64); Store(as, 8, value); }
    auto unused = Imm(as, 0x1234); plan.body->SetTerminal(ir::terminal::ReturnToHost{}); const auto config = Configuration();
    x64::Translator legacy{plan.body.get(), {}, &config, nullptr, true}; legacy.Translate();
    x64::Translator sparse{plan.body.get(), {}, &config, nullptr, true, x64::ExecutionMode::Ordinary, true, true}; sparse.Translate(); Inventory(sparse, plan);
    REQUIRE_FALSE(legacy.FramePlan().Enabled()); REQUIRE(sparse.FrameSize() < legacy.FrameSize());
    REQUIRE(sparse.FramePlan().Statistics().omitted_control >= 49); REQUIRE(sparse.FramePlan().Statistics().omitted_register > 0);
    REQUIRE(sparse.FramePlan().Decision(unused.Def()) == D::UnusedScalar);
    REQUIRE_THROWS_AS(x64::TransferStatisticsProbe::Home(sparse, control), std::runtime_error);
    REQUIRE_THROWS_AS(x64::TransferStatisticsProbe::Home(sparse, unused.Def()), std::runtime_error);
    ir::Inst foreign{O::Nop}; REQUIRE_FALSE(sparse.FramePlan().Home(&foreign)); REQUIRE_THROWS_AS(x64::TransferStatisticsProbe::Home(sparse, &foreign), std::runtime_error);
    Machine baseline, actual; baseline.Put(0, UINT64_MAX); actual.Put(0, UINT64_MAX); Run(legacy, baseline); Run(sparse, actual); Same(baseline, actual);
}

TEST_CASE("sparse frame retains native producer flags metadata despite assigned result", "[x64][frame][sparse][flags]") {
    for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto value = Load(as, 0);
        auto sum = as.Add(value, ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64); Store(as, 8, sum); as.SaveFlags(sum, ir::Flags::NZCV | ir::Flags::AuxiliaryCarry);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{}); const auto config = Configuration();
        x64::Translator legacy{plan.body.get(), {}, &config, &plan, true}; legacy.Translate();
        x64::Translator sparse{plan.body.get(), {}, &config, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; sparse.Translate(); Inventory(sparse, plan);
        REQUIRE(sparse.CfgPlan().Register(sum.Def())); REQUIRE(sparse.FramePlan().Decision(sum.Def()) == D::Retained);
        const auto home = sparse.FramePlan().Home(sum.Def()); REQUIRE(home); REQUIRE(*home + 16 + 8 <= sparse.FramePlan().Statistics().home_bytes);
        REQUIRE(sparse.FrameSize() < legacy.FrameSize());
        Machine baseline, actual; baseline.Put(0, UINT64_MAX); actual.Put(0, UINT64_MAX); Run(legacy, baseline); Run(sparse, actual); Same(baseline, actual);
    }
}

TEST_CASE("sparse frame retains independent scratch homes for VOID memory and MemoryCopy", "[x64][frame][sparse][memory]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto address = Load(as, 0), source = Load(as, 8), value = Load(as, 16);
    auto* store = as.AppendInst(O::StoreMemory, ir::Operand{address}, value);
    auto* copy = as.AppendInst(O::MemoryCopy, ir::Lambda{address}, ir::Lambda{source}, ir::Imm{u64{8}});
    auto loaded = as.LoadMemory(ir::Operand{address}).SetType(T::U64); Store(as, 24, loaded); plan.body->SetTerminal(ir::terminal::ReturnToHost{}); const auto config = Configuration();
    x64::Translator sparse{plan.body.get(), {}, &config, &plan, true, x64::ExecutionMode::Ordinary, true}; sparse.Translate(); Inventory(sparse, plan);
    for (auto* inst : {store, copy, loaded.Def()}) { REQUIRE(sparse.FramePlan().Decision(inst) == D::Retained); REQUIRE(sparse.FramePlan().Home(inst)); }
    REQUIRE(sparse.FramePlan().Home(store) != sparse.FramePlan().Home(copy));
    REQUIRE(*sparse.FramePlan().Home(store) + 24 + 8 <= sparse.FramePlan().Statistics().home_bytes);
    REQUIRE(*sparse.FramePlan().Home(copy) + 8 + 8 <= sparse.FramePlan().Statistics().home_bytes);
    std::array<u64, 2> memory{0x1122334455667788ULL, 0x8877665544332211ULL}; Machine actual;
    actual.Put(0, reinterpret_cast<u64>(&memory[0])); actual.Put(8, reinterpret_cast<u64>(&memory[1])); actual.Put(16, 0x123456789abcdef0ULL);
    Run(sparse, actual); REQUIRE(memory[0] == memory[1]);
}

TEST_CASE("sparse frame retains pair and QC producer metadata with separate pseudo homes", "[x64][frame][sparse][pseudo]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto high = Imm(as, 0), low = Load(as, 0), divisor = Imm(as, 3);
    auto quotient = as.Div128(high, low, divisor, ir::Imm{u64{0}}).SetType(T::U64); auto remainder = as.Div128Remainder(quotient).SetType(T::U64);
    Store(as, 8, quotient); Store(as, 16, remainder);
    auto input = Load(as, 32, T::V128), other = Load(as, 48, T::V128), accumulator = Load(as, 64, T::V128);
    auto result = as.VecSatMulHigh(input, other, accumulator, ir::Imm{u32{16}}, ir::Imm{u32{32}}).SetType(T::V128);
    auto qc = as.VecSatMulQC(result).SetType(T::U8); Store(as, 80, result); Store(as, 96, qc);
    plan.body->SetTerminal(ir::terminal::ReturnToHost{}); const auto config = Configuration();
    x64::Translator legacy{plan.body.get(), {}, &config, &plan}; legacy.Translate();
    x64::Translator sparse{plan.body.get(), {}, &config, &plan, true, x64::ExecutionMode::Ordinary, true}; sparse.Translate(); Inventory(sparse, plan);
    for (auto* inst : {quotient.Def(), remainder.Def(), result.Def(), qc.Def()}) { REQUIRE(sparse.FramePlan().Decision(inst) == D::Retained); REQUIRE(sparse.FramePlan().Home(inst)); }
    REQUIRE(sparse.FramePlan().Home(quotient.Def()) != sparse.FramePlan().Home(remainder.Def()));
    REQUIRE(sparse.FramePlan().Home(result.Def()) != sparse.FramePlan().Home(qc.Def()));
    REQUIRE(*sparse.FramePlan().Home(result.Def()) + 16 + 8 <= sparse.FramePlan().Statistics().home_bytes);
    Machine baseline, actual; baseline.Put(0, UINT64_MAX); actual.Put(0, UINT64_MAX); Run(legacy, baseline); Run(sparse, actual); Same(baseline, actual);
}

TEST_CASE("sparse frame keeps Phi edge scratch while register values survive helpers", "[x64][frame][sparse][phi][helper]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto phi = as.AddPhi(ir::Params{}).SetType(T::U64);
    auto output = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Identity)}}, phi, phi, phi).SetType(T::U64); Store(as, 8, output);
    auto next = as.Sub(phi, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64); auto back = as.Goto(as.TestNotZero(next).SetType(T::U8)); as.BindLabel(back);
    auto* label = &plan.body->GetInstList().back(); plan.body->RemoveInst(label); plan.body->InsertBefore(label, phi.Def());
    ir::Params inputs; inputs.Push(ir::Imm{u64{3}}); inputs.Push(next); phi.Def()->SetArg(0, inputs);
    Store(as, 16, phi); plan.body->SetTerminal(ir::terminal::ReturnToHost{}); const auto config = Configuration();
    x64::Translator legacy{plan.body.get(), {}, &config, &plan, true}; legacy.Translate();
    x64::Translator sparse{plan.body.get(), {}, &config, &plan, true, x64::ExecutionMode::Ordinary, true}; sparse.Translate(); Inventory(sparse, plan);
    REQUIRE(sparse.FrameSize() >= sparse.FramePlan().Statistics().home_bytes + 16 + 24); REQUIRE(sparse.FrameSize() < legacy.FrameSize());
    REQUIRE(sparse.CfgPlan().Register(output.Def())); REQUIRE(sparse.FramePlan().Decision(output.Def()) == D::RegisterOnly);
    Machine baseline, actual; Run(legacy, baseline); Run(sparse, actual); Same(baseline, actual);
}

TEST_CASE("sparse frame rejects incomplete stale and foreign allocation authority", "[x64][frame][sparse][rejection]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()}; Store(as, 8, Load(as, 0)); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
    auto registers = x64::CfgRegisterPlan::Build(plan.body.get(), plan); REQUIRE(registers.Enabled());
    const auto valid = x64::SparseFramePlan::Build(plan.body.get(), &plan, registers, {});
    REQUIRE(valid.Validate(plan.body.get(), &plan, registers, {}));
    const auto topology = x64::CfgRegisterPlan::ValidateGraph(plan.body.get(), plan); REQUIRE(topology.Enabled());
    REQUIRE_FALSE(x64::SparseFramePlan::Build(plan.body.get(), &plan, topology, {}).Enabled());
    REQUIRE_FALSE(valid.Validate(plan.body.get(), &plan, topology, {}));
    auto foreign = Plan(); ir::Assembler other{foreign.body.get()}; Store(other, 8, Load(other, 0)); foreign.body->SetTerminal(ir::terminal::ReturnToHost{});
    REQUIRE_FALSE(x64::SparseFramePlan::Build(foreign.body.get(), &foreign, registers, {}).Enabled());
    REQUIRE_FALSE(x64::SparseFramePlan::Build(plan.body.get(), &foreign, registers, {}).Enabled());
    REQUIRE_FALSE(valid.Validate(foreign.body.get(), &foreign, registers, {}));
    REQUIRE_FALSE(valid.Validate(plan.body.get(), &foreign, registers, {}));
    auto* added = as.AppendInst(O::Nop); const auto rejected = x64::SparseFramePlan::Build(plan.body.get(), &plan, registers, {});
    REQUIRE_FALSE(rejected.Enabled()); REQUIRE_FALSE(rejected.Home(added)); REQUIRE(rejected.Statistics().home_bytes == 0);
    REQUIRE_FALSE(valid.Validate(plan.body.get(), &plan, registers, {}));
}

TEST_CASE("sparse frame actual native fault preserves complete uniforms and recovery ABI", "[x64][frame][sparse][fault]") {
    struct Mapping { void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); ~Mapping() { if (page != MAP_FAILED) munmap(page, 4096); } } mapping;
    REQUIRE(mapping.page != MAP_FAILED);
    for (bool cfg : {false, true}) for (bool flags : {false, true}) {
        const auto config = Configuration(); backend::AddressSpace space{config}; backend::ModuleConfig policy; policy.read_only = true;
        policy.feature_overrides.Set(FeatureId::x64_cfg_allocation, cfg); policy.feature_overrides.Set(FeatureId::x64_flags_residency, flags);
        auto module = space.MapModule(0x1000, 0x2000, policy); Runtime runtime{&space}; auto plan = Plan(); ir::Assembler as{plan.body.get()};
        auto value = as.Add(Load(as, 0), ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64); Store(as, 8, value); as.SaveFlags(value, ir::Flags::NZCV);
        auto load = as.LoadMemory(ir::Operand{Imm(as, reinterpret_cast<u64>(mapping.page))}).SetType(T::U64); Store(as, 16, load); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        REQUIRE(module->Push(plan.body.get())); REQUIRE(backend::TranslateIR(module, plan.body) != nullptr);
        Machine model; model.Put(0, UINT64_MAX); std::memcpy(runtime.GetUniformBuffer().data(), model.state->uniform_buffer_begin, extent);
        runtime.GetState()->host_cpu_flags = model.state->host_cpu_flags; runtime.SetLocation(0x1000); model.Put(8, 6);
        const auto mxcsr = _mm_getcsr(); REQUIRE(runtime.Run() == HaltReason::PageFatal); REQUIRE(_mm_getcsr() == mxcsr);
        REQUIRE(std::memcmp(runtime.GetUniformBuffer().data(), model.state->uniform_buffer_begin, extent) == 0);
        REQUIRE(runtime.GetLocation() == 0x1000); REQUIRE(runtime.GetState()->host_cpu_flags == ((model.state->host_cpu_flags & ~(u64{15} << 28)) | 0x20000000));
        REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0); REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
    }
}
