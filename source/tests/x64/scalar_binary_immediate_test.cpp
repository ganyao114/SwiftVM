#include <array>
#include <cstring>
#include <vector>
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
constexpr std::array<T, 8> types{T::U8, T::S8, T::U16, T::S16, T::U32, T::S32, T::U64, T::S64};
constexpr std::array<O, 8> operations{O::Add, O::Sub, O::Adc, O::Sbb, O::And, O::Or, O::Xor, O::AndNot};
constexpr std::array<u64, 14> boundaries{0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xff80, 0x7fffffff, 0x80000000,
    0xffffffff, 0xffffffff80000000ULL, UINT64_MAX, 0x8123456789abcdefULL};
u64 Mask(T type) { const u32 bits = ir::GetValueSizeByte(type) * 8; return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }
bool Arithmetic(O op) { return op == O::Add || op == O::Sub || op == O::Adc || op == O::Sbb; }
u64 Result(O op, u64 a, u64 b, bool carry, T type) {
    a &= Mask(type); b &= Mask(type);
    switch (op) {
        case O::Add: return (a + b) & Mask(type); case O::Sub: return (a - b) & Mask(type);
        case O::Adc: return (a + b + carry) & Mask(type); case O::Sbb: return (a - b - !carry) & Mask(type);
        case O::And: return a & b; case O::Or: return a | b;
        case O::Xor: return a ^ b; case O::AndNot: return a & ~b & Mask(type);
        default: FAIL("uncovered independent binary model"); return 0;
    }
}
u64 Flags(O op, u64 a, u64 b, bool carry, T type, u64 prior) {
    const u32 bits = ir::GetValueSizeByte(type) * 8; const u64 sign = u64{1} << (bits - 1), mask = Mask(type);
    a &= mask; b &= mask; const u64 result = Result(op, a, b, carry, type);
    const bool subtract = op == O::Sub || op == O::Sbb;
    const u64 extra = op == O::Adc ? u64(carry) : op == O::Sbb ? u64(!carry) : 0;
    const auto signed_lane = [bits, sign](u64 value) -> __int128 {
        return value & sign ? __int128(value) - (__int128{1} << bits) : __int128(value);
    };
    bool c{}, v{}, af{};
    if (Arithmetic(op)) {
        if (subtract) c = static_cast<unsigned __int128>(a) >= static_cast<unsigned __int128>(b) + extra;
        else c = (static_cast<unsigned __int128>(a) + b + extra) > mask;
        const __int128 mathematical = subtract ? signed_lane(a) - signed_lane(b) - extra : signed_lane(a) + signed_lane(b) + extra;
        v = mathematical < -(__int128{1} << (bits - 1)) || mathematical >= (__int128{1} << (bits - 1));
        af = bool((a ^ b ^ result) & 16);
    }
    // Packed parity publishes the result byte, not a Boolean PF value. AF is
    // architecturally retained by logical producers in SwiftVM's flag model.
    const u64 affected = (u64{15} << 28) | 255 | (Arithmetic(op) ? u64{1} << 26 : 0);
    return (prior & ~affected) | (u64(bool(result & sign)) << 31) | (u64(result == 0) << 30) |
           (u64(c) << 29) | (u64(v) << 28) | (Arithmetic(op) ? u64(af) << 26 : 0) | (result & 255);
}
bool Encodable(O op, u64 rhs, T type) {
    const u64 value = (op == O::AndNot ? ~rhs : rhs) & Mask(type);
    if (ir::GetValueSizeByte(type) < 8) return true;
    const u64 low = value & 0xffffffff;
    const u64 extended = low & 0x80000000 ? low | 0xffffffff00000000ULL : low;
    return value == extended;
}
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() { for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 13); state->host_cpu_flags = initial_flags; }
    ~Machine() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
};
backend::FunctionPlan Plan() { backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000}); return plan; }
ir::Value Load(ir::Assembler& as, u32 offset, T type = T::U64) { return as.LoadUniform(ir::Uniform{offset, type}).SetType(type); }
ir::Value View(ir::Value value, T type) { return ir::Value{value.Def()}.SetCastType(type); }
void Store(ir::Assembler& as, u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
ir::Value Binary(ir::Assembler& as, O op, ir::Value lhs, ir::Operand rhs, T type) { return ir::Value{as.AppendInst(op, lhs, rhs)}.SetType(type); }
void Carry(ir::Assembler& as) { as.SetCarry(Load(as, 16, T::U8)); }
u64 Prior(bool carry) { return (initial_flags & ~(u64{1} << 29)) | (u64(carry) << 29); }
std::array<ir::Value, 4> Pressure(ir::Assembler& as) {
    std::array<ir::Value, 4> pressure; for (u32 i = 0; i < pressure.size(); ++i) pressure[i] = Load(as, 32 + i * 8); return pressure;
}
void Consume(ir::Assembler& as, const std::array<ir::Value, 4>& pressure) {
    for (u32 repeat = 0; repeat < 40; ++repeat) for (u32 i = 0; i < pressure.size(); ++i) Store(as, 768 + i * 8, pressure[i]);
}
void Initialize(Machine& machine, u64 seed, bool carry) {
    machine.Put(0, seed); machine.Put(16, carry); for (u32 i = 0; i < 4; ++i) machine.Put(32 + i * 8, seed + i * 29);
}
void PressureModel(Machine& machine, u64 seed) { for (u32 i = 0; i < 4; ++i) machine.Put(768 + i * 8, seed + i * 29); }
u64 Helper(u64 a, u64 b, u64 c) {
    asm volatile("xor %%r8d, %%r8d\n\txor %%r9d, %%r9d\n\txor %%r10d, %%r10d\n\txor %%r11d, %%r11d" ::: "r8", "r9", "r10", "r11", "cc");
    return a * 3 + b * 5 + c * 7;
}
struct PoisonCall : Xbyak::CodeGenerator {
    explicit PoisonCall(x64::Translator& code) : Xbyak::CodeGenerator(4096) {
        const u32 bytes = (code.FrameSize() + 4096 + 15) & ~u32{15};
        mov(rdx, rdi); mov(r11, rsp); sub(rsp, bytes); mov(rdi, rsp); mov(rcx, bytes / 8);
        mov(rax, 0xa5e39b7142c68df0ULL); cld(); rep(); stosq(); mov(rsp, r11); mov(rdi, rdx);
        mov(rax, reinterpret_cast<u64>(code.getCode())); jmp(rax);
    }
    void Run(Machine& machine) { const auto mxcsr = _mm_getcsr(); REQUIRE(getCode<HaltReason (*)(backend::State*)>()(machine.state) == HaltReason::CallHost); REQUIRE(_mm_getcsr() == mxcsr); }
};
void Complete(const Machine& off, const Machine& on, Machine& expected, u64 flags) {
    expected.state->current_loc = ir::Location{0x1000}; expected.state->prev_loc = ir::Location{0x1000};
    expected.state->halt_reason = HaltReason::CallHost; expected.state->host_cpu_flags = flags;
    REQUIRE(std::memcmp(off.bytes.data(), on.bytes.data(), on.bytes.size()) == 0);
    REQUIRE(std::memcmp(on.bytes.data(), expected.bytes.data(), expected.bytes.size()) == 0);
}
void Policies(const x64::Translator& off, const x64::Translator& on, bool cfg, u64 immediate, u64 rematerialized = 0) {
    REQUIRE_FALSE(off.ScalarBinaryImmediatesEnabled()); REQUIRE(on.ScalarBinaryImmediatesEnabled());
    REQUIRE(off.Statistics().scalar_binary_immediate_sites == 0); REQUIRE(off.Statistics().scalar_binary_rematerialized_immediate_sites == 0);
    REQUIRE(on.Statistics().scalar_binary_immediate_sites == immediate); REQUIRE(on.Statistics().scalar_binary_rematerialized_immediate_sites == rematerialized);
    REQUIRE(off.Statistics().scalar_rematerialized_read_sites == on.Statistics().scalar_rematerialized_read_sites + rematerialized);
    REQUIRE(off.FrameSize() == on.FrameSize()); REQUIRE(off.Statistics().scalar_rematerialized_definition_sites == on.Statistics().scalar_rematerialized_definition_sites);
    REQUIRE(off.Statistics().scalar_coalesced_binary_sites == on.Statistics().scalar_coalesced_binary_sites);
    if (cfg) {
        REQUIRE(off.CfgPlan().Validate()); REQUIRE(on.CfgPlan().Validate()); REQUIRE(off.CfgPlan().Instructions() == on.CfgPlan().Instructions());
        REQUIRE(off.CfgPlan().PhysicalRegisters() == on.CfgPlan().PhysicalRegisters());
        for (const auto& value : off.CfgPlan().Allocations()) {
            REQUIRE(off.CfgPlan().Register(value.producer) == on.CfgPlan().Register(value.producer));
            REQUIRE(off.FramePlan().Home(value.producer) == on.FramePlan().Home(value.producer));
            REQUIRE(off.FramePlan().RematerializedValue(value.producer) == on.FramePlan().RematerializedValue(value.producer));
        }
    }
    if (immediate) REQUIRE(on.getSize() < off.getSize());
}
void LoopLabel(ir::Assembler& as, backend::FunctionPlan& plan, ir::Value edge, ir::Value first) {
    as.BindLabel(edge); auto* label = &plan.body->GetInstList().back(); plan.body->RemoveInst(label); plan.body->InsertBefore(label, first.Def());
}
void Inputs(ir::Value destination, ir::Value initial, ir::Value backedge) { ir::Params inputs; inputs.Push(initial); inputs.Push(backedge); destination.Def()->SetArg(0, inputs); }
Config Configuration() { return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true, .backend_isa = kX86_64, .uniform_buffer_size = extent, .static_program = true, .stack_alignment = 16}; }
} // namespace

TEST_CASE("scalar binary direct immediates preserve independent full flags after delayed helper publication", "[x64][binary-immediate][width][flags][helper]") {
    for (auto op : operations) for (auto type : types) for (bool cfg : {false, true}) for (bool flags : {false, true}) {
        CAPTURE(op, type, cfg, flags); auto plan = Plan(); ir::Assembler as{plan.body.get()}; Carry(as); auto lhs = Load(as, 0, type);
        const auto view = ir::GetValueSizeByte(type) == 8 ? T::U16 : T::U64;
        std::array<ir::Value, boundaries.size()> results;
        for (u32 i = 0; i < boundaries.size(); ++i) results[i] = Binary(as, op, View(lhs, i & 1 ? view : type), ir::Operand{ir::Imm{boundaries[i]}}, type);
        auto returned = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, View(lhs, T::U64), View(lhs, T::U64), View(lhs, T::U64)).SetType(T::U64); Store(as, 512, returned);
        for (u32 i = 0; i < results.size(); ++i) {
            Store(as, 128 + i * 16, View(results[i], T::U64)); as.SaveFlags(results[i], ir::Flags::All);
            Store(as, 136 + i * 16, as.GetFlags(results[i], ir::Flags::All).SetType(T::U64));
        }
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator off{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, cfg, flags, true, false}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, cfg, flags, true, true}; on.Translate();
        u64 accepted{}; for (auto value : boundaries) accepted += Encodable(op, value, type); Policies(off, on, cfg, accepted);
        REQUIRE(on.Statistics().helper_call_sites > 0); if (Arithmetic(op)) REQUIRE(on.Statistics().flags_snapshot_sites == boundaries.size());
        PoisonCall old_call{off}, new_call{on};
        for (bool carry : {false, true}) for (u64 seed : {u64{0}, UINT64_MAX, u64{0x7fffffffffffffff}, u64{0x8123456789abcdef}}) {
            CAPTURE(carry, seed); Machine baseline, actual, expected; for (auto* machine : {&baseline, &actual, &expected}) Initialize(*machine, seed, carry);
            const u64 original = seed & Mask(type); u64 packed = Prior(carry);
            expected.Put(512, original * 15);
            for (u32 i = 0; i < results.size(); ++i) {
                const u64 a = original & Mask(i & 1 ? view : type);
                expected.Put(128 + i * 16, Result(op, a, boundaries[i], carry, type)); packed = Flags(op, a, boundaries[i], carry, type, packed); expected.Put(136 + i * 16, packed);
            }
            old_call.Run(baseline); new_call.Run(actual); Complete(baseline, actual, expected, packed);
        }
    }
}

TEST_CASE("binary immediates normalize unassigned constant producers and views before encoding", "[x64][binary-immediate][rematerialized][cast][pressure]") {
    constexpr std::array<u64, 5> values{0, UINT64_MAX, 0xffffffff, 0x80000000, 0xffffffff80000000ULL};
    for (auto op : operations) for (auto producer : types) for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool flags : {false, true}) {
        CAPTURE(op, producer, type, flags); auto plan = Plan(); ir::Assembler as{plan.body.get()}; Carry(as); auto pressure = Pressure(as); auto lhs = Load(as, 0);
        std::array<ir::Value, values.size()> constants;
        for (u32 i = 0; i < values.size(); ++i) constants[i] = as.LoadImm(ir::Imm{values[i], i & 1 ? T::S32 : T::U64}).SetType(producer);
        u64 accepted{};
        for (u32 i = 0; i < values.size(); ++i) for (u32 j = 0; j < types.size(); ++j) {
            const u64 rhs = values[i] & (i & 1 ? UINT32_MAX : UINT64_MAX) & Mask(producer) & Mask(types[j]);
            accepted += Encodable(op, rhs, type); auto result = Binary(as, op, lhs, ir::Operand{View(constants[i], types[j])}, type);
            Store(as, 128 + (i * types.size() + j) * 8, View(result, T::U64));
        }
        Consume(as, pressure); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator off{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags, true, false}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags, true, true}; on.Translate(); Policies(off, on, true, accepted, accepted);
        for (auto value : constants) { REQUIRE_FALSE(on.CfgPlan().Register(value.Def())); REQUIRE(on.FramePlan().RematerializedValue(value.Def())); }
        REQUIRE(off.Statistics().scalar_rematerialized_read_sites == values.size() * types.size());
        PoisonCall old_call{off}, new_call{on};
        for (bool carry : {false, true}) for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) {
            CAPTURE(carry, seed); Machine baseline, actual, expected; for (auto* machine : {&baseline, &actual, &expected}) Initialize(*machine, seed, carry); PressureModel(expected, seed);
            for (u32 i = 0; i < values.size(); ++i) for (u32 j = 0; j < types.size(); ++j) {
                const u64 rhs = values[i] & (i & 1 ? UINT32_MAX : UINT64_MAX) & Mask(producer) & Mask(types[j]);
                expected.Put(128 + (i * types.size() + j) * 8, Result(op, seed, rhs, carry, type));
            }
            old_call.Run(baseline); new_call.Run(actual); Complete(baseline, actual, expected, Prior(carry));
        }
    }
}

TEST_CASE("binary immediate lowering preserves destructive coalescing and complex operand fallbacks", "[x64][binary-immediate][coalesced][fallback]") {
    for (auto op : operations) for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; Carry(as); auto source = Load(as, 0);
        auto first = Binary(as, op, View(source, type), ir::Operand{ir::Imm{u64{7}}}, type);
        auto second = Binary(as, op, View(first, T::U64), ir::Operand{ir::Imm{u64{3}}, ir::Imm{u64{2}}, ir::OperandPlus}, type);
        auto third = Binary(as, op, second, ir::Operand{ir::Imm{u64{0x120}}, ir::Imm{u64{3}}, ir::OperandLsr}, type);
        auto fourth = Binary(as, op, third, ir::Operand{ir::Imm{u64{9}}, ir::DataClass{}, ir::OperandOp{ir::OperandOp::Plus, 1}}, type);
        auto fifth = Binary(as, op, fourth, ir::Operand{ir::Imm{u64{11}}, ir::DataClass{}, ir::OperandNone}, type);
        Store(as, 128, View(fifth, T::U64)); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator off{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags, true, false}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags, true, true}; on.Translate();
        Policies(off, on, true, 2); REQUIRE(on.CfgPlan().CanReuseBinaryInput(first.Def(), source.Def())); REQUIRE(on.Statistics().scalar_coalesced_binary_sites > 0);
        PoisonCall old_call{off}, new_call{on};
        for (bool carry : {false, true}) for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) {
            Machine baseline, actual, expected; for (auto* machine : {&baseline, &actual, &expected}) Initialize(*machine, seed, carry);
            const u64 a = Result(op, seed & Mask(type), 7, carry, type), b = Result(op, a, 5, carry, type);
            const u64 c = Result(op, b, 0x24, carry, type), d = Result(op, c, 9, carry, type);
            expected.Put(128, Result(op, d, 11, carry, type)); old_call.Run(baseline); new_call.Run(actual); Complete(baseline, actual, expected, Prior(carry));
        }
        auto colored_plan = Plan(); ir::Assembler colored_as{colored_plan.body.get()}; Carry(colored_as);
        auto colored = colored_as.LoadImm(ir::Imm{u64{7}}).SetType(T::U64), input = Load(colored_as, 0);
        auto output = Binary(colored_as, op, input, ir::Operand{colored}, type); Store(colored_as, 128, View(output, T::U64)); colored_plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator colored_off{colored_plan.body.get(), {}, nullptr, &colored_plan, true, x64::ExecutionMode::Ordinary, true, flags, true, false}; colored_off.Translate();
        x64::Translator colored_on{colored_plan.body.get(), {}, nullptr, &colored_plan, true, x64::ExecutionMode::Ordinary, true, flags, true, true}; colored_on.Translate();
        REQUIRE(colored_on.CfgPlan().Register(colored.Def())); Policies(colored_off, colored_on, true, 0); REQUIRE(colored_on.getSize() == colored_off.getSize());
        Machine old_state, actual, expected; for (auto* machine : {&old_state, &actual, &expected}) Initialize(*machine, UINT64_MAX, true);
        expected.Put(128, Result(op, UINT64_MAX, 7, true, type)); PoisonCall old_colored{colored_off}, new_colored{colored_on}; old_colored.Run(old_state); new_colored.Run(actual); Complete(old_state, actual, expected, Prior(true));
    }
}

TEST_CASE("binary immediate policy propagates preparation and preserves helper loop Phi snapshots", "[x64][binary-immediate][preparation][phi][helper]") {
    for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; Carry(as); auto trips = Load(as, 8); auto pressure = Pressure(as);
        auto seed = Load(as, 0), increment = as.LoadImm(ir::Imm{u64{17}}).SetType(T::U64);
        auto phi = as.AddPhi(ir::Params{}).SetType(T::U64), count = as.AddPhi(ir::Params{}).SetType(T::U64);
        auto returned = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, phi, phi, phi).SetType(T::U64); Store(as, 128, returned);
        auto next = Binary(as, O::Adc, phi, ir::Operand{increment}, T::U64);
        auto remaining = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64); auto edge = as.Goto(as.TestNotZero(remaining)); LoopLabel(as, plan, edge, phi);
        Inputs(phi, seed, next); Inputs(count, trips, remaining); Store(as, 136, phi); Store(as, 144, next); Store(as, 152, remaining);
        Consume(as, pressure); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        auto off = x64::Translator::PrepareFunction(plan, {}, nullptr, true, true, flags, {.scalar_binary_immediates = false}); off->Translate();
        auto on = x64::Translator::PrepareFunction(plan, {}, nullptr, true, true, flags, {.scalar_binary_immediates = true}); on->Translate();
        Policies(*off, *on, true, 2, 1); REQUIRE(on->FramePlan().RematerializedValue(increment.Def())); REQUIRE(on->Statistics().phi_value_copy_sites > 0);
        PoisonCall old_call{*off}, new_call{*on};
        for (bool carry : {false, true}) for (u64 iterations : {u64{1}, u64{2}, u64{7}}) {
            Machine baseline, actual, expected; const u64 input = UINT64_MAX - 23;
            for (auto* machine : {&baseline, &actual, &expected}) { Initialize(*machine, input, carry); machine->Put(8, iterations); }
            PressureModel(expected, input); const u64 value = input + (iterations - 1) * (17 + carry);
            expected.Put(128, value * 15); expected.Put(136, value); expected.Put(144, value + 17 + carry); expected.Put(152, 0);
            old_call.Run(baseline); new_call.Run(actual); Complete(baseline, actual, expected, Prior(carry));
        }
    }
}

TEST_CASE("binary immediate lowering restores complete Runtime state after actual protected-page faults", "[x64][binary-immediate][production][fault]") {
    struct Mapping { void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); ~Mapping() { if (page != MAP_FAILED) munmap(page, 4096); } } mapping;
    REQUIRE(mapping.page != MAP_FAILED);
    for (bool cfg : {false, true}) for (bool flags : {false, true}) {
        const auto config = Configuration(); backend::AddressSpace space{config}; backend::ModuleConfig policy; policy.read_only = true;
        policy.feature_overrides.Set(FeatureId::x64_cfg_allocation, cfg); policy.feature_overrides.Set(FeatureId::x64_flags_residency, flags);
        auto module = space.MapModule(0x1000, 0x2000, policy); Runtime runtime{&space}; auto plan = Plan(); ir::Assembler as{plan.body.get()}; Carry(as); auto pressure = Pressure(as);
        auto input = Load(as, 8), constant = as.LoadImm(ir::Imm{u64{17}}).SetType(T::U64);
        auto sum = Binary(as, O::Adc, input, ir::Operand{ir::Imm{u64{7}}}, T::U64); Store(as, 128, sum); as.SaveFlags(sum, ir::Flags::All); Consume(as, pressure);
        as.AdvancePC(ir::Imm{u32{4}}); auto loaded = as.LoadMemory(ir::Operand{Load(as, 0)}).SetType(T::U64); Store(as, 136, loaded);
        Store(as, 144, Binary(as, O::Xor, input, ir::Operand{constant}, T::U64)); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator inventory{plan.body.get(), {}, &config, &plan, true, x64::ExecutionMode::Ordinary, cfg, flags, true, true}; inventory.Translate();
        REQUIRE(inventory.Statistics().scalar_binary_immediate_sites >= 1); if (cfg) REQUIRE(inventory.Statistics().scalar_binary_rematerialized_immediate_sites == 1);
        REQUIRE(module->Push(plan.body.get())); REQUIRE(backend::TranslateIR(module, plan.body) != nullptr);
        auto* state = runtime.GetState(); state->guest_addr_limit = UINT64_MAX; state->guest_addr_mask = UINT64_MAX; state->interp_range_check = nullptr; state->interp_range_check_ctx = nullptr;
        for (bool accessible : {false, true, false}) {
            REQUIRE(mprotect(mapping.page, 4096, accessible ? PROT_READ | PROT_WRITE : PROT_NONE) == 0);
            constexpr u64 memory = 0x9988776655443322ULL, seed = UINT64_MAX - 5; if (accessible) std::memcpy(mapping.page, &memory, 8);
            Machine model; Initialize(model, seed, true); model.Put(0, reinterpret_cast<u64>(mapping.page)); model.Put(8, seed);
            std::memcpy(runtime.GetUniformBuffer().data(), model.state->uniform_buffer_begin, extent); state->host_cpu_flags = initial_flags; runtime.SetLocation(0x1000);
            Machine expected; std::memcpy(expected.bytes.data(), state, expected.bytes.size()); PressureModel(expected, seed); expected.Put(128, seed + 8);
            if (accessible) { expected.Put(136, memory); expected.Put(144, seed ^ 17); }
            expected.state->current_loc = ir::Location{0x1004}; expected.state->prev_loc = ir::Location{0x1000}; expected.state->halt_reason = HaltReason::None;
            expected.state->host_cpu_flags = Flags(O::Adc, seed, 7, true, T::U64, Prior(true));
            expected.state->spill_area[backend::kRiscvRecoveryPcSlot] = 0; expected.state->spill_area[backend::kRiscvRecoveryFrameSlot] = 0; expected.state->spill_area[backend::kRiscvMemoryOwnedSlot] = 0;
            const auto mxcsr = _mm_getcsr(); REQUIRE(runtime.Run() == (accessible ? HaltReason::CallHost : HaltReason::PageFatal)); REQUIRE(_mm_getcsr() == mxcsr);
            REQUIRE(std::memcmp(state, expected.bytes.data(), expected.bytes.size()) == 0);
        }
    }
}
