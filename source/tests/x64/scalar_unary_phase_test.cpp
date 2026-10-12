#include <algorithm>
#include <array>
#include <bit>
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
using F = ir::Flags;
constexpr u32 extent = 1024;
constexpr u64 initial_flags = 0xfedcba9876543219ULL;
constexpr std::array unary_ops{O::Neg, O::ZeroExtend32, O::ZeroExtend32To64, O::ZeroExtend64, O::SignExtend,
    O::LslImm, O::LsrImm, O::AsrImm, O::RorImm, O::ByteSwap, O::BitExtract, O::BitClear,
    O::TestBit, O::TestZero, O::TestNotZero};
T Type(u32 bits) { return bits == 8 ? T::U8 : bits == 16 ? T::U16 : bits == 32 ? T::U32 : T::U64; }
u64 Mask(u32 bits) { return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }
u64 Signed(u64 value, u32 bits) {
    value &= Mask(bits); return bits == 64 || !(value & (u64{1} << (bits - 1))) ? value : value | ~Mask(bits);
}
struct Program {
    backend::FunctionPlan function;
    ir::Assembler as;
    Program() : as((function.body = new ir::Block(ir::Location{0x1000})).get()) {}
    ir::Inst* Label() {
        auto* label = new ir::Inst(O::BindLabel); label->SetArgs(ir::Value{label}); function.body->AppendInst(label); return label;
    }
    ir::Value Load(u32 offset) { return as.LoadUniform(ir::Uniform{offset, T::U64}).SetType(T::U64); }
    void Store(u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, T::U64}, value.SetCastType(T::U64)); }
    ir::Value Branch(ir::Value value) { return as.Goto(ir::BOOL{value}); }
    ir::Value Jump() { return Branch(as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)); }
    void Target(ir::Value branch, ir::Inst* label, bool stop = false) {
        function.targets[branch.Def()] = label; if (stop) function.stops.insert(branch.Def());
    }
    void Finish() { function.body->SetTerminal(ir::terminal::ReturnToHost{}); }
    x64::CfgRegisterPlan Plan(bool enabled, u64 budget = 8 * 1024 * 1024, std::vector<unsigned> regs = {15}) {
        return x64::CfgRegisterPlan::Build(function.body.get(), function,
            {.physical_registers = std::move(regs), .interference_storage_budget_bytes = budget,
             .loop_weighted_costs = true, .scalar_unary_phases = enabled});
    }
    ir::Value Unary(O op, ir::Value input, T result = T::U64, u32 first = 7, u32 second = 13) {
        ir::Inst* inst{};
        switch (op) {
            case O::LslImm: case O::LsrImm: case O::AsrImm: case O::RorImm:
            case O::ByteSwap: case O::TestBit: inst = as.AppendInst(op, input, ir::Imm{first}); break;
            case O::BitExtract: case O::BitClear: inst = as.AppendInst(op, input, ir::Imm{first}, ir::Imm{second}); break;
            default: inst = as.AppendInst(op, input); break;
        }
        return ir::Value{inst}.SetType(result);
    }
};
void Inputs(ir::Value phi, ir::Value initial, ir::Value next) {
    ir::Params params; params.Push(initial); params.Push(next); phi.Def()->SetArg(0, params);
}
u64 Model(O op, u64 source, u32 view, u32 bits, u32 first, u32 second) {
    u64 value = source & Mask(view);
    const u32 shift = first & (bits - 1);
    switch (op) {
        case O::Neg: value = 0 - value; break;
        case O::ZeroExtend32: case O::ZeroExtend32To64: value &= UINT32_MAX; break;
        case O::ZeroExtend64: break;
        case O::SignExtend: value = Signed(value, view); break;
        case O::LslImm: value = (value & Mask(bits)) << shift; break;
        case O::LsrImm: value = (value & Mask(bits)) >> shift; break;
        case O::AsrImm: {
            value = Signed(value, view); const bool negative = value >> 63;
            value >>= shift; if (shift && negative) value |= UINT64_MAX << (64 - shift); break;
        }
        case O::RorImm:
            value &= Mask(bits); if (shift) value = (value >> shift) | (value << (bits - shift)); break;
        case O::ByteSwap: {
            const u32 width = first == 16 ? 16 : first == 32 ? 32 : 64;
            u64 swapped{}; for (u32 byte = 0; byte < width / 8; ++byte) swapped |= ((value >> (byte * 8)) & 255) << (width - 8 - byte * 8);
            value = width == 16 ? (value & ~u64{0xffff}) | swapped : swapped; break;
        }
        case O::BitExtract: value >>= first & 63; if (second < 64 && second < bits) value &= Mask(second); break;
        case O::BitClear: value &= ~(Mask(second) << first); break;
        case O::TestBit: value = (value >> (first & 63)) & 1; break;
        case O::TestZero: value = value == 0; break;
        case O::TestNotZero: value = value != 0; break;
        default: FAIL("model received an unaudited opcode");
    }
    return value & Mask(bits);
}
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
struct PoisonCall : Xbyak::CodeGenerator {
    explicit PoisonCall(x64::Translator& code) : Xbyak::CodeGenerator(4096) {
        const u32 bytes = (code.FrameSize() + 4096 + 15) & ~u32{15};
        mov(rdx, rdi); mov(r11, rsp); sub(rsp, bytes); mov(rdi, rsp); mov(rcx, bytes / 8);
        mov(rax, 0xa5e39b7142c68df0ULL); cld(); rep(); stosq(); mov(rsp, r11); mov(rdi, rdx);
        mov(rax, reinterpret_cast<u64>(code.getCode())); jmp(rax);
    }
    void Run(Machine& machine) {
        const auto saved = _mm_getcsr(); REQUIRE(getCode<HaltReason (*)(backend::State*)>()(machine.state) == HaltReason::CallHost);
        REQUIRE(_mm_getcsr() == saved);
    }
};
void Same(const x64::CfgRegisterPlan& a, const x64::CfgRegisterPlan& b) {
    REQUIRE(a.Validate()); REQUIRE(b.Validate()); REQUIRE(a.Instructions() == b.Instructions());
    REQUIRE(a.PhysicalRegisters() == b.PhysicalRegisters()); REQUIRE(a.Allocations().size() == b.Allocations().size());
    REQUIRE(a.ScalarUnaryPhasesEnabled() == b.ScalarUnaryPhasesEnabled());
    for (const auto& allocation : a.Allocations()) {
        const auto found = std::find_if(b.Allocations().begin(), b.Allocations().end(), [&](const auto& other) { return other.producer == allocation.producer; });
        REQUIRE(found != b.Allocations().end()); REQUIRE(found->slot == allocation.slot);
        REQUIRE(found->requires_canonical_home == allocation.requires_canonical_home); REQUIRE(found->requires_full_container == allocation.requires_full_container);
        REQUIRE(found->scalar_reads == allocation.scalar_reads); REQUIRE(found->estimated_benefit == allocation.estimated_benefit);
        REQUIRE(found->weighted_reads == allocation.weighted_reads); REQUIRE(found->weighted_benefit == allocation.weighted_benefit);
        for (auto* other : a.Instructions()) {
            REQUIRE(a.Interferes(allocation.producer, other) == b.Interferes(allocation.producer, other));
            REQUIRE(a.CanReuseUnaryInput(allocation.producer, other) == b.CanReuseUnaryInput(allocation.producer, other));
            REQUIRE(a.CanReuseBinaryInput(allocation.producer, other) == b.CanReuseBinaryInput(allocation.producer, other));
        }
    }
}
u64 NegFlags(u64 source, u32 bits) {
    source &= Mask(bits); const u64 result = (0 - source) & Mask(bits);
    constexpr u64 mask = (u64{15} << 28) | (u64{1} << 26) | 255;
    return (initial_flags & ~mask) | (u64(bool(result & (u64{1} << (bits - 1)))) << 31) |
        (u64(result == 0) << 30) | (u64(source == 0) << 29) | (u64(source == (u64{1} << (bits - 1))) << 28) |
        (u64(bool((source ^ result) & 16)) << 26) | (result & 255);
}
u64 Helper(u64 a, u64 b, u64 c) {
    asm volatile("xor %%r8d, %%r8d\n\txor %%r9d, %%r9d\n\txor %%r10d, %%r10d\n\txor %%r11d, %%r11d" ::: "r8", "r9", "r10", "r11", "cc");
    return a + b + c;
}
void NativeLoop(Program& p) {
    auto seed = p.Load(8), trips = p.Load(16), invariant = p.Load(24); auto* header = p.Label();
    auto acc = p.as.AddPhi(ir::Params{}).SetType(T::U64), count = p.as.AddPhi(ir::Params{}).SetType(T::U64);
    auto first = p.Unary(O::ByteSwap, acc, T::U64, 64);
    auto second = p.Unary(O::RorImm, first, T::U64, 7);
    auto third = p.Unary(O::BitClear, second, T::U64, 13, 9);
    auto next = p.Unary(O::Neg, third);
    p.as.SaveFlags(next, F::All);
    auto returned = p.as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, next, invariant, next).SetType(T::U64);
    p.Store(128, returned); p.Store(136, next); p.Store(144, invariant);
    auto remaining = p.as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    auto back = p.Branch(p.as.TestNotZero(remaining).SetType(T::U8)); p.Target(back, header);
    Inputs(acc, seed, next); Inputs(count, trips, remaining); p.Store(152, remaining);
    p.Store(160, p.as.GetFlags(next, F::All).SetType(T::U64)); p.Finish();
}
u64 LoopModel(Machine& expected, u64 seed, u64 trips, u64 invariant) {
    u64 value = seed, flags{};
    for (u64 i = 0; i < trips; ++i) {
        value = Model(O::ByteSwap, value, 64, 64, 64, 0);
        value = Model(O::RorImm, value, 64, 64, 7, 0);
        value = Model(O::BitClear, value, 64, 64, 13, 9);
        flags = NegFlags(value, 64); value = 0 - value;
    }
    expected.Put(128, value * 2 + invariant); expected.Put(136, value); expected.Put(144, invariant);
    expected.Put(152, 0); expected.Put(160, flags); expected.state->host_cpu_flags = flags; expected.Complete(); return value;
}
} // namespace

TEST_CASE("CFG unary phases give every audited dead input the same single physical register", "[x64][unary-phase][allocation]") {
    for (auto op : unary_ops) {
        CAPTURE(op); Program p; auto input = p.Load(0), output = p.Unary(op, input);
        p.Store(128, output); p.Store(136, output); p.Finish();
        auto off = p.Plan(false), on = p.Plan(true); REQUIRE(off.Validate()); REQUIRE(on.Validate());
        REQUIRE_FALSE(off.ScalarUnaryPhasesEnabled()); REQUIRE(on.ScalarUnaryPhasesEnabled());
        REQUIRE(off.Interferes(input.Def(), output.Def())); REQUIRE_FALSE(off.CanReuseUnaryInput(output.Def(), input.Def()));
        REQUIRE_FALSE(on.Interferes(input.Def(), output.Def())); REQUIRE(on.CanReuseUnaryInput(output.Def(), input.Def()));
        REQUIRE_FALSE(on.CanReuseBinaryInput(output.Def(), input.Def()));
        REQUIRE(on.Register(input.Def()) == 15); REQUIRE(on.Register(output.Def()) == 15);
        REQUIRE_FALSE((off.Register(input.Def()) && off.Register(output.Def())));
        Same(on, p.Plan(true, 0)); Same(on, p.Plan(true)); Same(off, p.Plan(false, 0));
    }
    Program empty; empty.Finish(); auto graph = x64::CfgRegisterPlan::ValidateGraph(empty.function.body.get(), empty.function);
    REQUIRE_FALSE(graph.ScalarUnaryPhasesEnabled());
}

TEST_CASE("unary phase policy preserves the exact existing binary interference and reuse contracts", "[x64][unary-phase][binary-compatibility]") {
    for (auto op : {O::Add, O::Sub, O::Adc, O::Sbb, O::And, O::Or, O::Xor, O::AndNot}) {
        Program p; auto source = p.Load(0);
        auto output = ir::Value{p.as.AppendInst(op, source, ir::Operand{ir::Imm{u64{7}}})}.SetType(T::U64);
        p.Store(128, output); p.Store(136, output); p.Finish(); auto off = p.Plan(false), on = p.Plan(true);
        REQUIRE(off.Validate()); REQUIRE(on.Validate()); REQUIRE(off.CanReuseBinaryInput(output.Def(), source.Def()));
        REQUIRE(on.CanReuseBinaryInput(output.Def(), source.Def()));
        for (const auto& allocation : off.Allocations()) {
            REQUIRE(on.Slot(allocation.producer) == allocation.slot);
            REQUIRE(on.RequiresCanonicalHome(allocation.producer) == off.RequiresCanonicalHome(allocation.producer));
            REQUIRE(on.RequiresFullContainer(allocation.producer) == off.RequiresFullContainer(allocation.producer));
            for (auto* other : off.Instructions()) {
                REQUIRE(on.Interferes(allocation.producer, other) == off.Interferes(allocation.producer, other));
                REQUIRE(on.CanReuseBinaryInput(allocation.producer, other) == off.CanReuseBinaryInput(allocation.producer, other));
                REQUIRE_FALSE(on.CanReuseUnaryInput(allocation.producer, other));
                REQUIRE_FALSE(off.CanReuseUnaryInput(allocation.producer, other));
            }
        }
    }
}

TEST_CASE("CFG unary phases preserve later ordinary raw metadata and Phi edge reads", "[x64][unary-phase][liveness]") {
    SECTION("ordinary later use keeps the source live") {
        Program p; auto source = p.Load(0), output = p.Unary(O::Neg, source); p.Store(128, output); p.Store(136, source); p.Finish();
        auto plan = p.Plan(true); REQUIRE(plan.Validate()); REQUIRE(plan.Interferes(source.Def(), output.Def()));
        REQUIRE_FALSE(plan.CanReuseUnaryInput(output.Def(), source.Def()));
    }
    SECTION("an extra same-instruction raw view is live through publication") {
        Program p; auto source = p.Load(0), output = p.Unary(O::LsrImm, source); output.Def()->SetArg(4, source.SetCastType(T::V128));
        p.Store(128, output); p.Finish(); auto plan = p.Plan(true); REQUIRE(plan.Validate());
        REQUIRE(plan.RequiresCanonicalHome(source.Def())); REQUIRE(plan.RequiresFullContainer(source.Def()));
        REQUIRE(plan.Interferes(source.Def(), output.Def())); REQUIRE_FALSE(plan.CanReuseUnaryInput(output.Def(), source.Def()));
    }
    SECTION("a raw input view cannot enter the scalar phase inventory") {
        Program p; auto source = p.Load(0), output = p.Unary(O::ZeroExtend64, source.SetCastType(T::V128));
        p.Store(128, output); p.Finish(); auto plan = p.Plan(true); REQUIRE(plan.Validate());
        REQUIRE(plan.Interferes(source.Def(), output.Def())); REQUIRE_FALSE(plan.CanReuseUnaryInput(output.Def(), source.Def()));
    }
    SECTION("earlier raw observation requires storage but does not keep a later dead input live") {
        Program p; auto source = p.Load(0); auto raw = p.as.BitCast(ir::Value{source.Def()}.SetCastType(T::V128)).SetType(T::V128);
        p.as.StoreUniform(ir::Uniform{256, T::V128}, raw); p.Store(264, source);
        auto output = p.Unary(O::Neg, source); p.Store(128, output); p.Finish(); auto plan = p.Plan(true);
        REQUIRE(plan.Validate()); REQUIRE(plan.RequiresCanonicalHome(source.Def())); REQUIRE(plan.RequiresFullContainer(source.Def()));
        REQUIRE(plan.CanReuseUnaryInput(output.Def(), source.Def())); REQUIRE_FALSE(plan.Interferes(source.Def(), output.Def()));
    }
    SECTION("delayed parity publication still consumes the original arithmetic result") {
        Program p; auto source = p.Load(0), negated = p.Unary(O::Neg, source), output = p.Unary(O::ByteSwap, negated, T::U64, 64);
        p.as.SaveFlags(negated, F::All); p.Store(128, output); p.Finish(); auto plan = p.Plan(true);
        REQUIRE(plan.Validate()); REQUIRE(plan.Interferes(negated.Def(), output.Def()));
        REQUIRE_FALSE(plan.CanReuseUnaryInput(output.Def(), negated.Def()));
    }
    SECTION("a backedge Phi reads the original rather than the unary output") {
        Program p; auto seed = p.Load(0), entry = p.Jump(); auto* header = p.Label();
        auto carried = p.as.AddPhi(ir::Params{}).SetType(T::U64), output = p.Unary(O::Neg, carried);
        p.Store(128, output); auto back = p.Branch(p.Load(8)); Inputs(carried, seed, carried);
        p.Target(entry, header, true); p.Target(back, header); p.Finish(); auto plan = p.Plan(true);
        REQUIRE(plan.Validate()); REQUIRE(plan.Interferes(carried.Def(), output.Def()));
        REQUIRE_FALSE(plan.CanReuseUnaryInput(output.Def(), carried.Def()));
    }
    SECTION("both Not spellings retain their existing conservative interference") {
        for (bool operand : {false, true}) {
            Program p; auto source = p.Load(0);
            auto output = operand ? p.as.Not(source, ir::Operand{source}).SetType(T::U64) : p.as.Not(source).SetType(T::U8);
            p.Store(128, output); p.Finish(); auto plan = p.Plan(true); REQUIRE(plan.Validate());
            REQUIRE(plan.Interferes(source.Def(), output.Def())); REQUIRE_FALSE(plan.CanReuseUnaryInput(output.Def(), source.Def()));
        }
    }
}

TEST_CASE("prepared unary phases preserve independent width and view math in full native State", "[x64][unary-phase][native][width]") {
    const auto config = Configuration();
    for (auto op : unary_ops) for (u32 bits : {8u, 16u, 32u, 64u}) for (u32 view : {8u, 16u, 32u, 64u}) {
        // Canonical fixed-width IR extends have fixed declared result widths.
        if ((op == O::ZeroExtend32 && bits != 32) || ((op == O::ZeroExtend32To64 || op == O::ZeroExtend64) && bits != 64)) continue;
        Program p; auto source = p.Load(0); std::vector<std::array<u32, 2>> arguments{{7, 13}};
        if (op == O::LslImm || op == O::LsrImm || op == O::AsrImm || op == O::RorImm) arguments = {{0, 0}, {bits, 0}, {bits + 1, 0}, {63, 0}};
        if (op == O::ByteSwap) arguments = {{16, 0}, {32, 0}, {64, 0}};
        if (op == O::BitExtract) arguments = {{0, 0}, {0, 64}, {7, 13}, {31, 8}, {63, 1}};
        if (op == O::BitClear) arguments = {{0, 0}, {0, 64}, {7, 13}, {31, 8}, {63, 1}};
        if (op == O::TestBit) arguments = {{0, 0}, {7, 0}, {31, 0}, {63, 0}};
        for (u32 i = 0; i < arguments.size(); ++i) p.Store(128 + i * 8, p.Unary(op, source.SetCastType(Type(view)), Type(bits), arguments[i][0], arguments[i][1]));
        p.Finish(); auto off = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, false, {.scalar_unary_phases = false});
        auto on = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, false, {.scalar_unary_phases = true});
        off->Translate(); on->Translate(); REQUIRE_FALSE(off->CfgPlan().ScalarUnaryPhasesEnabled()); REQUIRE(on->CfgPlan().ScalarUnaryPhasesEnabled());
        REQUIRE(off->CfgPlan().Instructions() == on->CfgPlan().Instructions()); PoisonCall old_call{*off}, new_call{*on};
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x7fffffffffffffff}, u64{0x8000000000000080}, u64{0x0123456789abcdef}}) {
            CAPTURE(op, bits, view, seed); Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) machine->Put(0, seed);
            for (u32 i = 0; i < arguments.size(); ++i) expected.Put(128 + i * 8, Model(op, seed, view, bits, arguments[i][0], arguments[i][1]));
            expected.Complete(); old_call.Run(baseline); new_call.Run(actual);
            REQUIRE(actual.bytes == expected.bytes); REQUIRE(baseline.bytes == expected.bytes);
        }
    }
}

TEST_CASE("unary phase reuse preserves delayed Neg flags snapshots and canonical result width", "[x64][unary-phase][native][flags]") {
    const auto config = Configuration();
    for (u32 bits : {8u, 16u, 32u, 64u}) for (bool resident : {false, true}) {
        Program p; auto source = p.Load(0), negated = p.Unary(O::Neg, source.SetCastType(Type(bits)), Type(bits));
        // CV+AF publication consumes metadata only, letting the negated result die at the next unary.
        auto next = p.Unary(O::ByteSwap, negated, T::U64, 64); p.as.SaveFlags(negated.SetCastType(T::U8), F::CV | F::AuxiliaryCarry);
        p.Store(128, next); p.Finish(); auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, resident, {.scalar_unary_phases = true});
        code->Translate(); REQUIRE(code->CfgPlan().CanReuseUnaryInput(next.Def(), negated.Def())); PoisonCall call{*code};
        constexpr u64 publication = (u64{3} << 28) | (u64{1} << 26);
        for (u64 seed : {u64{0}, u64{1}, UINT64_MAX, u64{0x80}, u64{0x8000}, u64{0x80000000}, u64{0x8000000000000000}}) {
            Machine actual, expected; actual.Put(0, seed); expected.Put(0, seed);
            expected.Put(128, Model(O::ByteSwap, Model(O::Neg, seed, bits, bits, 0, 0), bits, 64, 64, 0));
            expected.state->host_cpu_flags = (initial_flags & ~publication) | (NegFlags(seed, bits) & publication); expected.Complete();
            call.Run(actual); REQUIRE(actual.bytes == expected.bytes);
        }
    }
}

TEST_CASE("unary reuse retains the initialized high qword of an earlier raw container", "[x64][unary-phase][native][raw]") {
    const auto config = Configuration(); Program p; auto source = p.Load(0);
    auto raw = p.as.BitCast(ir::Value{source.Def()}.SetCastType(T::V128)).SetType(T::V128);
    p.as.StoreUniform(ir::Uniform{256, T::V128}, raw); p.Store(272, source);
    auto output = p.Unary(O::Neg, source); p.Store(128, output); p.Finish();
    auto off = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, false, {.scalar_unary_phases = false});
    auto on = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, false, {.scalar_unary_phases = true});
    off->Translate(); on->Translate(); REQUIRE(on->CfgPlan().RequiresFullContainer(source.Def()));
    REQUIRE(on->CfgPlan().RequiresCanonicalHome(source.Def())); REQUIRE(on->CfgPlan().CanReuseUnaryInput(output.Def(), source.Def()));
    PoisonCall old_call{*off}, new_call{*on};
    for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) {
        Machine baseline, actual, expected; for (auto* machine : {&baseline, &actual, &expected}) machine->Put(0, seed);
        expected.Put(256, seed); expected.Put(264, 0); expected.Put(272, seed); expected.Put(128, 0 - seed); expected.Complete();
        old_call.Run(baseline); new_call.Run(actual); REQUIRE(actual.bytes == expected.bytes); REQUIRE(baseline.bytes == expected.bytes);
    }
}

TEST_CASE("prepared unary phases preserve loop Phi helpers flags and poisoned frame state", "[x64][unary-phase][native][loop][helper]") {
    const auto config = Configuration();
    for (bool flags : {false, true}) for (bool forwarding : {false, true}) {
        // Each preparation owns its body: forwarding can change operands and
        // must never invalidate the allocation retained by another translator.
        Program ordinary, phased; NativeLoop(ordinary); NativeLoop(phased);
        auto off = x64::Translator::PrepareFunction(ordinary.function, {}, &config, true, true, flags, {.uniform_forwarding = forwarding, .scalar_unary_phases = false});
        auto on = x64::Translator::PrepareFunction(phased.function, {}, &config, true, true, flags, {.uniform_forwarding = forwarding, .scalar_unary_phases = true});
        off->Translate(); on->Translate(); REQUIRE(on->CfgPlan().Validate()); REQUIRE(on->CfgPlan().ScalarUnaryPhasesEnabled());
        REQUIRE(on->Statistics().helper_call_sites > 0); REQUIRE(on->Statistics().phi_value_copy_sites > 0);
        bool reuse{}; for (auto* output : on->CfgPlan().Instructions()) for (auto* input : on->CfgPlan().Instructions()) reuse |= on->CfgPlan().CanReuseUnaryInput(output, input);
        REQUIRE(reuse); PoisonCall old_call{*off}, new_call{*on};
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) for (u64 trips : {u64{1}, u64{2}, u64{7}}) {
            constexpr u64 invariant = 17; Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) { machine->Put(8, seed); machine->Put(16, trips); machine->Put(24, invariant); }
            LoopModel(expected, seed, trips, invariant); old_call.Run(baseline); new_call.Run(actual);
            REQUIRE(actual.bytes == expected.bytes); REQUIRE(baseline.bytes == expected.bytes);
        }
    }
}

TEST_CASE("prepared unary policy survives final forwarding allocation adoption", "[x64][unary-phase][preparation][reuse]") {
    const auto config = Configuration();
    for (bool enabled : {false, true}) {
        Program p; auto source = p.Load(0), duplicate = p.Load(0), output = p.Unary(O::Neg, duplicate);
        p.Store(128, output); p.Store(136, source); p.Store(144, source); p.Finish();
        auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, false,
            {.uniform_forwarding = true, .scalar_unary_phases = enabled}); code->Translate();
        REQUIRE(code->CfgPlan().ScalarUnaryPhasesEnabled() == enabled); REQUIRE(code->CfgPlan().Validate());
        REQUIRE(duplicate.Def()->GetOp() == O::BitExtract); REQUIRE(code->UniformForwardStats().loads_forwarded == 1);
        REQUIRE(code->PreparationStats().reused_final_allocation); REQUIRE(code->PreparationStats().full_allocation_builds == 1);
    }
}

TEST_CASE("production unary allocation recovers complete state across protected page transitions", "[x64][unary-phase][production][fault]") {
    struct Mapping { void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); ~Mapping() { if (page != MAP_FAILED) munmap(page, 4096); } } mapping;
    REQUIRE(mapping.page != MAP_FAILED);
    for (bool flags : {false, true}) {
        const auto config = Configuration(); backend::AddressSpace space{config}; backend::ModuleConfig policy; policy.read_only = true;
        policy.feature_overrides.Set(FeatureId::x64_cfg_allocation, true); policy.feature_overrides.Set(FeatureId::x64_flags_residency, flags);
        auto module = space.MapModule(0x1000, 0x2000, policy); Runtime runtime{&space}; Program p;
        auto source = p.Load(8), first = p.Unary(O::ByteSwap, source, T::U64, 64), next = p.Unary(O::Neg, first);
        p.as.SaveFlags(next, F::All); p.Store(128, next); p.as.AdvancePC(ir::Imm{u32{4}});
        p.Store(136, p.as.LoadMemory(ir::Operand{p.Load(0)}).SetType(T::U64)); p.Store(144, p.Unary(O::LsrImm, next, T::U64, 7)); p.Finish();
        x64::Translator inventory{p.function.body.get(), {}, &config, &p.function, true, x64::ExecutionMode::Ordinary, true, flags}; inventory.Translate();
        REQUIRE(inventory.CfgPlan().ScalarUnaryPhasesEnabled()); REQUIRE(module->Push(p.function.body.get())); REQUIRE(backend::TranslateIR(module, p.function.body) != nullptr);
        auto* state = runtime.GetState(); state->pt = nullptr; state->guest_addr_limit = UINT64_MAX; state->guest_addr_mask = UINT64_MAX;
        state->interp_range_check = nullptr; state->interp_range_check_ctx = nullptr;
        for (bool accessible : {false, true, false}) {
            REQUIRE(mprotect(mapping.page, 4096, accessible ? PROT_READ | PROT_WRITE : PROT_NONE) == 0);
            constexpr u64 memory = 0x9988776655443322ULL, seed = 0x8123456789abcdefULL;
            if (accessible) std::memcpy(mapping.page, &memory, 8);
            Machine initial; initial.Put(0, reinterpret_cast<u64>(mapping.page)); initial.Put(8, seed);
            std::memcpy(runtime.GetUniformBuffer().data(), initial.state->uniform_buffer_begin, extent); state->host_cpu_flags = initial_flags; runtime.SetLocation(0x1000);
            Machine expected; std::memcpy(expected.bytes.data(), state, expected.bytes.size());
            const u64 swapped = Model(O::ByteSwap, seed, 64, 64, 64, 0), value = 0 - swapped;
            expected.Put(128, value); expected.state->host_cpu_flags = NegFlags(swapped, 64);
            if (accessible) { expected.Put(136, memory); expected.Put(144, value >> 7); }
            expected.state->current_loc = ir::Location{0x1004}; expected.state->prev_loc = ir::Location{0x1000}; expected.state->halt_reason = HaltReason::None;
            expected.state->spill_area[backend::kRiscvRecoveryPcSlot] = 0; expected.state->spill_area[backend::kRiscvRecoveryFrameSlot] = 0;
            expected.state->spill_area[backend::kRiscvMemoryOwnedSlot] = 0;
            const auto saved = _mm_getcsr(); REQUIRE(runtime.Run() == (accessible ? HaltReason::CallHost : HaltReason::PageFatal));
            REQUIRE(_mm_getcsr() == saved); REQUIRE(std::memcmp(state, expected.bytes.data(), expected.bytes.size()) == 0);
        }
    }
}
