#include <array>
#include <cstring>
#include <new>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "translator/arm64/cpu.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
using F = ir::Flags;
constexpr u32 extent = 512;
constexpr u64 initial_flags = 0xfedcba9876543219ULL;
constexpr u64 nzcv_mask = 0xf0000000ULL;
T Type(u32 bits) { return bits == 8 ? T::U8 : bits == 16 ? T::U16 : bits == 32 ? T::U32 : T::U64; }
u64 Mask(u32 bits) { return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }
struct Program {
    backend::FunctionPlan function;
    ir::Assembler as;
    Program() : as((function.body = new ir::Block(ir::Location{0x1000})).get()) {}
    ir::Value Load(u32 offset) { return as.LoadUniform(ir::Uniform{offset, T::U64}).SetType(T::U64); }
    ir::Value Binary(O op, ir::Value lhs, ir::Operand rhs, T type) {
        return ir::Value{as.AppendInst(op, lhs, rhs)}.SetType(type);
    }
    void Store(u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, T::U64}, value.SetCastType(T::U64)); }
    ir::Inst* Save(ir::Value value, F requested = F::NZCV) { return as.AppendInst(O::SaveFlags, value, requested); }
    void Finish() { function.body->SetTerminal(ir::terminal::ReturnToHost{}); }
    x64::CarryChainPlan Carry(std::optional<u64> bytes = extent) {
        auto graph = x64::CfgRegisterPlan::ValidateGraph(function.body.get(), function);
        REQUIRE(graph.Enabled()); return x64::CarryChainPlan::Build(graph, &function, bytes);
    }
};
Config Configuration() {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true, .backend_isa = kX86_64,
            .uniform_buffer_size = extent, .static_program = true, .stack_alignment = 16};
}
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() { for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 13); state->host_cpu_flags = initial_flags; }
    ~Machine() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    void Complete() { state->current_loc = ir::Location{0x1000}; state->prev_loc = ir::Location{0x1000}; state->halt_reason = HaltReason::CallHost; }
};
void Run(x64::Translator& code, Machine& machine, HaltReason reason = HaltReason::CallHost) {
    const auto mxcsr = _mm_getcsr();
    REQUIRE(code.getCode<HaltReason (*)(backend::State*)>()(machine.state) == reason);
    REQUIRE(_mm_getcsr() == mxcsr);
}
struct Arithmetic { u64 value{}, nzcv{}; };
Arithmetic Model(O op, u64 left, u64 right, bool carry, u32 bits) {
    using Wide = unsigned __int128;
    using Signed = __int128;
    const auto mask = Mask(bits), sign = u64{1} << (bits - 1);
    const u64 a = left & mask, b = right & mask;
    const bool subtract = op == O::Sub || op == O::Sbb;
    const u64 extra = op == O::Adc ? carry : op == O::Sbb ? !carry : 0;
    Wide raw{}; bool guest_c{};
    if (subtract) {
        const Wide subtrahend = Wide{b} + extra;
        raw = Wide{a} - subtrahend; guest_c = Wide{a} >= subtrahend;
    } else {
        raw = Wide{a} + b + extra; guest_c = (raw >> bits) != 0;
    }
    const auto signed_value = [&](u64 value) { return value & sign ? Signed{value} - (Signed{1} << bits) : Signed{value}; };
    const Signed exact = subtract ? signed_value(a) - signed_value(b) - extra : signed_value(a) + signed_value(b) + extra;
    const Signed min = -(Signed{1} << (bits - 1)), max = (Signed{1} << (bits - 1)) - 1;
    Arithmetic result{u64(raw) & mask};
    result.nzcv = (result.value & sign ? u64{1} << 31 : 0) | (result.value == 0 ? u64{1} << 30 : 0) |
                  (guest_c ? u64{1} << 29 : 0) | (exact < min || exact > max ? u64{1} << 28 : 0);
    return result;
}
void SameAllocations(const x64::Translator& a, const x64::Translator& b) {
    REQUIRE(a.FrameSize() == b.FrameSize());
    REQUIRE(a.CfgPlan().PhysicalRegisters() == b.CfgPlan().PhysicalRegisters());
    REQUIRE(a.CfgPlan().Allocations().size() == b.CfgPlan().Allocations().size());
    for (u32 i = 0; i < a.CfgPlan().Allocations().size(); ++i) {
        const auto& x = a.CfgPlan().Allocations()[i]; const auto& y = b.CfgPlan().Allocations()[i];
        REQUIRE(x.producer == y.producer); REQUIRE(x.slot == y.slot);
        REQUIRE(x.requires_canonical_home == y.requires_canonical_home); REQUIRE(x.requires_full_container == y.requires_full_container);
    }
}
} // namespace

TEST_CASE("native CF pair preserves full State against arithmetic model in every scalar and flags mode", "[x64][carry-chain][native][model]") {
    const auto config = Configuration();
    constexpr std::array first_ops{O::Add, O::Sub, O::Adc, O::Sbb};
    constexpr std::array second_ops{O::Adc, O::Sbb};
    const std::array<u64, 10> inputs{0, 1, 0x7f, 0x80, 0xff, 0x7fffffff, 0x80000000,
        0x7fffffffffffffffULL, 0x8000000000000000ULL, UINT64_MAX};
    for (auto first_op : first_ops) for (auto second_op : second_ops) for (u32 bits : {8u, 16u, 32u, 64u})
        for (bool resident : {false, true}) for (bool cache : {false, true}) for (bool cfg : {false, true}) {
            Program p;
            auto left = p.Load(0), right = p.Load(8);
            auto first = p.Binary(first_op, left.SetCastType(Type(bits)), ir::Operand{right.SetCastType(Type(bits))}, Type(bits));
            p.Save(first); p.Store(128, first); // MOV-only publication.
            auto later_right = p.Load(16); // MOV-only load after first CF.
            auto second = p.Binary(second_op, first, ir::Operand{later_right.SetCastType(Type(bits))}, Type(bits));
            p.Save(second); p.Store(136, second); p.Finish();
            auto off = x64::Translator::PrepareFunction(p.function, {}, &config, cache, cfg, resident,
                {.native_cf_chains = false});
            auto on = x64::Translator::PrepareFunction(p.function, {}, &config, cache, cfg, resident,
                {.native_cf_chains = true});
            off->Translate(); on->Translate(); SameAllocations(*off, *on);
            const auto& old = off->Statistics(); const auto& now = on->Statistics();
            REQUIRE(off->CarryPlan().ChainCount() == 0); REQUIRE(on->CarryPlan().ChainCount() == 1);
            REQUIRE(now.native_cf_forward_sites == 1); REQUIRE(now.native_cf_merge_omissions == 1);
            REQUIRE(now.native_cf_resident_bt_omissions == (resident ? 1 : 0));
            REQUIRE(now.native_cf_state_bt_omissions == (resident ? 0 : 1));
            REQUIRE(now.native_cf_polarity_cmc_sites == ((first_op == O::Sub || first_op == O::Sbb) != (second_op == O::Sbb) ? 1 : 0));
            REQUIRE(now.flags_snapshot_sites == old.flags_snapshot_sites); REQUIRE(now.flags_snapshot_sites == 2);
            REQUIRE(now.flags_state_carry_read_sites + now.flags_resident_carry_read_sites + 1 ==
                    old.flags_state_carry_read_sites + old.flags_resident_carry_read_sites);
            REQUIRE(now.emitted_code_bytes < old.emitted_code_bytes);
            for (bool carry : {false, true}) for (u32 i = 0; i < inputs.size(); ++i) {
                const u64 a = inputs[i], b = inputs[(i + 3) % inputs.size()], c = inputs[(i + 7) % inputs.size()];
                Machine baseline, actual, expected;
                const u64 flags = (initial_flags & ~(u64{1} << 29)) | (u64{carry} << 29);
                for (auto* machine : {&baseline, &actual, &expected}) {
                    machine->Put(0, a); machine->Put(8, b); machine->Put(16, c); machine->state->host_cpu_flags = flags;
                }
                const auto x = Model(first_op, a, b, carry, bits);
                const auto y = Model(second_op, x.value, c, (x.nzcv >> 29) & 1, bits);
                expected.Put(128, x.value); expected.Put(136, y.value);
                expected.state->host_cpu_flags = (flags & ~nzcv_mask) | y.nzcv; expected.Complete();
                Run(*off, baseline); Run(*on, actual);
                REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
            }
        }
}

TEST_CASE("native CF pair preserves delayed first producer metadata and partial later publication", "[x64][carry-chain][native][metadata]") {
    const auto config = Configuration();
    for (bool resident : {false, true}) for (bool immediates : {false, true}) for (u32 bits : {8u, 16u, 32u, 64u}) {
        Program p; auto a = p.Load(0), b = p.Load(8);
        auto first = p.Binary(O::Sub, a.SetCastType(Type(bits)), ir::Operand{b.SetCastType(Type(bits))}, Type(bits));
        p.Save(first); auto second = p.Binary(O::Adc, first, ir::Operand{ir::Imm{u8{1}}}, Type(bits));
        p.Save(second); p.Save(first.SetCastType(T::U8), F::CV); p.Store(128, first); p.Store(136, second); p.Finish();
        auto off = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, resident,
            {.scalar_binary_immediates = immediates, .native_cf_chains = false});
        auto on = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, resident,
            {.scalar_binary_immediates = immediates, .native_cf_chains = true});
        off->Translate(); on->Translate(); SameAllocations(*off, *on);
        REQUIRE(on->Statistics().native_cf_forward_sites == 1);
        REQUIRE(off->Statistics().flags_snapshot_sites == on->Statistics().flags_snapshot_sites);
        for (u64 a_value : {u64{0}, u64{1}, UINT64_MAX, u64{0x8000000000000000}}) {
            Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) { machine->Put(0, a_value); machine->Put(8, 1); }
            const auto x = Model(O::Sub, a_value, 1, false, bits), y = Model(O::Adc, x.value, 1, (x.nzcv >> 29) & 1, bits);
            expected.Put(128, x.value); expected.Put(136, y.value);
            expected.state->host_cpu_flags = (initial_flags & ~nzcv_mask) | (y.nzcv & 0xc0000000) | (x.nzcv & 0x30000000);
            expected.Complete(); Run(*off, baseline); Run(*on, actual);
            REQUIRE(actual.bytes == expected.bytes); REQUIRE(baseline.bytes == expected.bytes);
        }
    }
}

TEST_CASE("native CF preanalysis rejects masks modifiers width changes and observations", "[x64][carry-chain][plan][reject]") {
    for (u32 rejection = 0; rejection < 14; ++rejection) {
        Program p; auto source = p.Load(0), rhs = p.Load(8);
        auto first = p.Binary(O::Add, source, ir::Operand{rhs}, T::U64);
        auto* publication = p.Save(first, rejection == 0 ? F::NZ : rejection == 1 ? F::All : F::NZCV);
        switch (rejection) {
            case 2: p.as.GetFlags(first, F::All).SetType(T::U64); break;
            case 3: p.as.GetLocation().SetType(T::U64); break;
            case 4: p.as.SetCarry(source); break;
            case 5: p.as.AppendInst(O::UniformBarrier); break;
            case 6: { auto* marker = p.as.AppendInst(O::Nop); p.function.halt_checks.insert(marker); break; }
            case 7: p.as.Not(source).SetType(T::U64); break;
            default: break;
        }
        const auto type = rejection == 8 ? T::U32 : T::U64;
        const auto operand = rejection == 9 ? ir::Operand{rhs, source, ir::OperandOp{ir::OperandOp::Plus}} :
                             rejection == 10 ? ir::Operand{ir::DataClass{rhs}, {}, ir::OperandOp{ir::OperandOp::None, 1}} : ir::Operand{rhs};
        auto second = p.Binary(rejection == 11 ? O::Add : O::Adc, first, operand, type);
        p.Save(second, rejection == 12 ? F::CV : F::NZCV);
        if (rejection == 13) p.function.halt_checks.insert(publication); // Invalid metadata rejected by graph itself.
        p.Finish();
        auto graph = x64::CfgRegisterPlan::ValidateGraph(p.function.body.get(), p.function);
        if (rejection == 13) REQUIRE_FALSE(graph.Enabled()); else REQUIRE(graph.Enabled());
        const auto plan = x64::CarryChainPlan::Build(graph, &p.function, extent);
        REQUIRE(plan.ChainCount() == 0); REQUIRE_FALSE(plan.DefersPublication(publication)); REQUIRE_FALSE(plan.IncomingFor(second.Def()));
    }
}

TEST_CASE("native CF plan requires bounded uniform moves and disjoint complete pairs", "[x64][carry-chain][plan][pairs]") {
    Program p; auto source = p.Load(0);
    auto first = p.Binary(O::Add, source, ir::Operand{ir::Imm{u64{1}}}, T::U64); auto* save1 = p.Save(first);
    p.Store(128, first); auto rhs = p.Load(8);
    auto second = p.Binary(O::Adc, first, ir::Operand{rhs}, T::U64); auto* save2 = p.Save(second);
    auto third = p.Binary(O::Sbb, second, ir::Operand{ir::Imm{u64{0xffffffffffffffff}}}, T::U64); auto* save3 = p.Save(third);
    auto fourth = p.Binary(O::Adc, third, ir::Operand{rhs}, T::U64); auto* save4 = p.Save(fourth); p.Finish();
    const auto full = p.Carry(); REQUIRE(full.ChainCount() == 2);
    REQUIRE(full.DefersPublication(save1)); REQUIRE_FALSE(full.DefersPublication(save2));
    REQUIRE(full.DefersPublication(save3)); REQUIRE_FALSE(full.DefersPublication(save4));
    REQUIRE(full.IncomingFor(second.Def())); REQUIRE(full.IncomingFor(fourth.Def()));
    REQUIRE(p.Carry(std::nullopt).ChainCount() == 1); REQUIRE(p.Carry(128).ChainCount() == 1);
    x64::CfgRegisterPlan missing; REQUIRE(x64::CarryChainPlan::Build(missing, nullptr, extent).ChainCount() == 0);
}

TEST_CASE("native CF chains remain default off and disabled in SingleInstruction", "[x64][carry-chain][policy]") {
    Program p; auto source = p.Load(0);
    auto first = p.Binary(O::Add, source, ir::Operand{ir::Imm{u8{1}}}, T::U64); p.Save(first);
    auto second = p.Binary(O::Adc, first, ir::Operand{ir::Imm{u8{1}}}, T::U64); p.Save(second); p.Finish();
    const auto config = Configuration();
    x64::Translator ordinary(p.function.body.get(), {}, &config, &p.function);
    REQUIRE_FALSE(ordinary.NativeCfChainsEnabled()); REQUIRE(ordinary.CarryPlan().ChainCount() == 0);
    x64::Translator single(p.function.body.get(), {}, &config, nullptr, false,
        x64::ExecutionMode::SingleInstruction, false, false, true, true, true, true, true, true, true, true);
    REQUIRE_FALSE(single.NativeCfChainsEnabled()); REQUIRE(single.CarryPlan().ChainCount() == 0);
}

TEST_CASE("native CF plan rejects CFG labels helper boundaries and missing final publication", "[x64][carry-chain][plan][boundaries]") {
    for (u32 boundary = 0; boundary < 6; ++boundary) {
        Program p; auto source = p.Load(0);
        auto first = p.Binary(O::Add, source, ir::Operand{ir::Imm{u8{1}}}, T::U64); auto* publication = p.Save(first);
        if (boundary == 0) {
            auto condition = p.as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8);
            auto branch = p.as.Goto(ir::BOOL{condition});
            auto* label = new ir::Inst(O::BindLabel); label->SetArgs(ir::Value{label}); p.function.body->AppendInst(label);
            p.function.targets[branch.Def()] = label;
        } else if (boundary == 1) {
            // Valid void helper IR with no invocation here: it is rejected by
            // the native-CF proof regardless of its declared uniform effects.
            p.as.CallLambda(ir::Lambda{ir::Imm{u64{0x1000}}}).SetType(T::U64);
        } else if (boundary == 2) {
            auto* stop = p.as.AppendInst(O::Nop); p.function.stops.insert(stop);
            p.function.exits.emplace(stop, ir::terminal::ReturnToHost{});
        }
        auto second = p.Binary(O::Adc, first, ir::Operand{ir::Imm{u8{1}}}, T::U64);
        if (boundary != 3) {
            if (boundary == 4) p.as.GetFlags(first, F::All).SetType(T::U64);
            p.Save(second);
        }
        if (boundary == 5) {
            auto* label = new ir::Inst(O::BindLabel); label->SetArgs(ir::Value{label}); p.function.body->AppendInst(label);
            ir::Params inputs; inputs.Push(second); auto phi = p.as.AddPhi(inputs).SetType(T::U64);
            p.Store(128, phi);
        }
        p.Finish(); const auto plan = p.Carry();
        REQUIRE(plan.ChainCount() == 0); REQUIRE_FALSE(plan.DefersPublication(publication));
        REQUIRE_FALSE(plan.IncomingFor(second.Def()));
    }
}

TEST_CASE("native CF pure AdvancePC preserves accumulated publication PC and complete State", "[x64][carry-chain][native][pc]") {
    const auto config = Configuration();
    for (bool resident : {false, true}) {
        Program p; auto source = p.Load(0);
        auto first = p.Binary(O::Add, source, ir::Operand{ir::Imm{u8{1}}}, T::U64); p.Save(first);
        p.as.AdvancePC(ir::Imm{u32{4}}); p.as.AdvancePC(ir::Imm{u64{8}});
        auto second = p.Binary(O::Adc, first, ir::Operand{ir::Imm{u8{1}}}, T::U64);
        p.as.AdvancePC(ir::Imm{u32{4}}); p.Save(second); p.Store(128, second);
        auto* exit = p.as.AppendInst(O::Nop); p.function.stops.insert(exit);
        p.function.exits.emplace(exit, ir::terminal::ReturnToHost{}); p.Finish();
        auto off = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, resident, {.native_cf_chains = false});
        auto on = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, resident, {.native_cf_chains = true});
        off->Translate(); on->Translate(); REQUIRE(on->Statistics().native_cf_forward_sites == 1);
        for (u64 value : {u64{0}, UINT64_MAX, u64{0x7fffffffffffffff}}) {
            Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) machine->Put(0, value);
            const auto x = Model(O::Add, value, 1, false, 64), y = Model(O::Adc, x.value, 1, (x.nzcv >> 29) & 1, 64);
            expected.Put(128, y.value); expected.state->host_cpu_flags = (initial_flags & ~nzcv_mask) | y.nzcv;
            expected.Complete(); expected.state->current_loc = ir::Location{0x1010};
            Run(*off, baseline); Run(*on, actual); REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
        }
    }
}

TEST_CASE("decoded A64 ADDS ADCS forwards native CF across AdvancePC with complete State", "[x64][carry-chain][native][decoded]") {
    class CodeMemory final : public MemoryInterface {
    public:
        std::array<u32, 3> words;
        explicit CodeMemory(bool wide) : words{wide ? 0xb1000420u : 0x31000420u,
                                               wide ? 0xba050083u : 0x3a050083u, 0x14000001u} {}
        // ADDS X0,X1,#1 / ADCS X3,X4,X5 / B 0x100c, or W variants.
        bool Read(void* destination, size_t address, size_t size) override {
            if (address < 0x1000 || address - 0x1000 > sizeof(words) || size > sizeof(words) - (address - 0x1000)) return false;
            std::memcpy(destination, reinterpret_cast<u8*>(words.data()) + address - 0x1000, size); return true;
        }
        bool Write(void*, size_t, size_t) override { return false; }
        void* GetPointer(void* pointer) override {
            const auto address = reinterpret_cast<size_t>(pointer);
            return address >= 0x1000 && address - 0x1000 < sizeof(words) ? reinterpret_cast<u8*>(words.data()) + address - 0x1000 : nullptr;
        }
    };
    const auto config = Configuration();
    for (bool wide : {false, true}) for (bool resident : {false, true}) for (bool cfg : {false, true}) {
        Program p; CodeMemory memory{wide}; swift::arm64::A64Decoder{0x1000, &memory, &p.as}.Decode();
        auto off = x64::Translator::PrepareFunction(p.function, {}, &config, true, cfg, resident, {.native_cf_chains = false});
        auto on = x64::Translator::PrepareFunction(p.function, {}, &config, true, cfg, resident, {.native_cf_chains = true});
        off->Translate(); on->Translate(); REQUIRE(on->Statistics().native_cf_forward_sites == 1);
        REQUIRE(on->Statistics().native_cf_merge_omissions == 1); REQUIRE(on->Statistics().flags_snapshot_sites == 2);
        REQUIRE(off->Statistics().flags_snapshot_sites == on->Statistics().flags_snapshot_sites);
        for (u64 a : {u64{0}, UINT64_MAX, u64{0x7fffffffffffffff}, u64{0x7fffffff}}) {
            Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) { machine->Put(8, a); machine->Put(32, UINT64_MAX); machine->Put(40, 1); }
            const u32 bits = wide ? 64 : 32;
            const auto first = Model(O::Add, a, 1, false, bits), second = Model(O::Adc, UINT64_MAX, 1, (first.nzcv >> 29) & 1, bits);
            expected.Put(0, first.value); expected.Put(24, second.value);
            expected.state->host_cpu_flags = (initial_flags & ~nzcv_mask) | second.nzcv;
            expected.state->current_loc = ir::Location{0x100c}; expected.state->prev_loc = ir::Location{0x1000};
            Run(*off, baseline, HaltReason::None); Run(*on, actual, HaltReason::None);
            REQUIRE(baseline.bytes == expected.bytes); REQUIRE(actual.bytes == expected.bytes);
        }
    }
}
