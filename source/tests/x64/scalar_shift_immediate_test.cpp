#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <vector>
#include <set>
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
void SameAllocationByOrdinal(const x64::CfgRegisterPlan& off, const x64::CfgRegisterPlan& on) {
    REQUIRE(off.Validate()); REQUIRE(on.Validate()); REQUIRE(off.PhysicalRegisters() == on.PhysicalRegisters());
    REQUIRE(off.Instructions().size() == on.Instructions().size()); REQUIRE(off.Blocks().size() == on.Blocks().size());
    REQUIRE(off.Edges().size() == on.Edges().size()); REQUIRE(off.ScalarUnaryPhasesEnabled()); REQUIRE(on.ScalarUnaryPhasesEnabled());
    REQUIRE(off.Allocations().size() == on.Allocations().size());
    for (u32 i = 0; i < off.Allocations().size(); ++i) {
        const auto& a = off.Allocations()[i]; const auto& b = on.Allocations()[i];
        REQUIRE(a.producer->GetOp() == b.producer->GetOp()); REQUIRE(a.slot == b.slot);
        REQUIRE(a.requires_canonical_home == b.requires_canonical_home); REQUIRE(a.requires_full_container == b.requires_full_container);
        REQUIRE(a.scalar_reads == b.scalar_reads); REQUIRE(a.weighted_reads == b.weighted_reads); REQUIRE(a.weighted_benefit == b.weighted_benefit);
    }
    for (u32 i = 0; i < off.Instructions().size(); ++i) {
        auto* a = off.Instructions()[i]; auto* b = on.Instructions()[i];
        REQUIRE(a->GetOp() == b->GetOp()); REQUIRE(a->ReturnType() == b->ReturnType());
        REQUIRE(off.Register(a) == on.Register(b)); REQUIRE(off.RequiresCanonicalHome(a) == on.RequiresCanonicalHome(b));
        for (u32 j = 0; j < off.Instructions().size(); ++j) {
            REQUIRE(off.Interferes(a, off.Instructions()[j]) == on.Interferes(b, on.Instructions()[j]));
            REQUIRE(off.CanReuseUnaryInput(a, off.Instructions()[j]) == on.CanReuseUnaryInput(b, on.Instructions()[j]));
            REQUIRE(off.CanReuseBinaryInput(a, off.Instructions()[j]) == on.CanReuseBinaryInput(b, on.Instructions()[j]));
        }
    }
}
} // namespace

TEST_CASE("direct immediate shifts preserve canonical widths views and masked count boundaries", "[x64][shift-immediate][native][width]") {
    const auto config = Configuration(); constexpr std::array ops{O::LslImm, O::LsrImm, O::AsrImm, O::RorImm};
    size_t old_bytes{}, new_bytes{};
    for (auto op : ops) for (u32 bits : {8u, 16u, 32u, 64u}) for (u32 view : {8u, 16u, 32u, 64u})
        for (u32 count : {0u, 1u, bits - 1, bits, bits + 1, 255u, 256u, UINT32_MAX}) {
            Program ordinary, direct;
            auto build = [&](Program& p) { auto source = p.Load(0); auto output = p.Unary(op, source.SetCastType(Type(view)), Type(bits), count); p.Store(128, output); p.Store(136, output); p.Finish(); };
            build(ordinary); build(direct);
            auto off = x64::Translator::PrepareFunction(ordinary.function, {}, &config, true, true, false,
                {.scalar_unary_phases = true, .scalar_shift_immediates = false});
            auto on = x64::Translator::PrepareFunction(direct.function, {}, &config, true, true, false,
                {.scalar_unary_phases = true, .scalar_shift_immediates = true});
            off->Translate(); on->Translate(); SameAllocationByOrdinal(off->CfgPlan(), on->CfgPlan());
            REQUIRE_FALSE(off->ScalarShiftImmediatesEnabled()); REQUIRE(on->ScalarShiftImmediatesEnabled());
            REQUIRE(off->Statistics().scalar_shift_immediate_sites == 0); REQUIRE(off->Statistics().scalar_shift_zero_sites == 0);
            REQUIRE(on->Statistics().scalar_shift_immediate_sites == 1);
            REQUIRE(on->Statistics().scalar_shift_zero_sites == (count % bits == 0 ? 1 : 0));
            REQUIRE(on->Statistics().scalar_cache_read_sites == off->Statistics().scalar_cache_read_sites);
            REQUIRE(on->Statistics().scalar_cache_write_sites == off->Statistics().scalar_cache_write_sites);
            old_bytes += off->getSize(); new_bytes += on->getSize(); PoisonCall old_call{*off}, new_call{*on};
            for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8000000000000080}, u64{0x7fffffffffffffff}, u64{0x0123456789abcdef}}) {
                CAPTURE(op, bits, view, count, seed); Machine baseline, actual, expected;
                for (auto* machine : {&baseline, &actual, &expected}) machine->Put(0, seed);
                const auto value = Model(op, seed, view, bits, count, 0); expected.Put(128, value); expected.Put(136, value); expected.Complete();
                old_call.Run(baseline); new_call.Run(actual); REQUIRE(actual.bytes == expected.bytes); REQUIRE(baseline.bytes == expected.bytes);
            }
        }
    REQUIRE(new_bytes < old_bytes);
}

TEST_CASE("zero shifts preserve legal NZ parity publication and earlier arithmetic metadata", "[x64][shift-immediate][native][flags]") {
    const auto config = Configuration();
    for (u32 bits : {8u, 16u, 32u, 64u}) for (bool enabled : {false, true}) for (bool resident : {false, true}) {
        Program p; auto source = p.Load(0); auto negated = p.Unary(O::Neg, source.SetCastType(Type(bits)), Type(bits));
        auto shifted = p.Unary(O::LsrImm, negated, Type(bits), bits);
        // Shift only synthesizes NZ and parity from its canonical result; CV
        // and AF come from the earlier real Neg snapshot, never shift metadata.
        p.as.SaveFlags(negated.SetCastType(T::U8), F::CV | F::AuxiliaryCarry);
        p.as.SaveFlags(shifted, F::NZ | F::Parity); p.Store(128, shifted); p.Finish();
        auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, resident,
            {.scalar_unary_phases = true, .scalar_shift_immediates = enabled}); code->Translate();
        REQUIRE(code->Statistics().scalar_shift_immediate_sites == (enabled ? 1 : 0));
        REQUIRE(code->Statistics().scalar_shift_zero_sites == (enabled ? 1 : 0)); PoisonCall call{*code};
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x80}, u64{0x8000}, u64{0x80000000}, u64{0x8000000000000000}}) {
            Machine actual, expected; actual.Put(0, seed); expected.Put(0, seed);
            expected.Put(128, Model(O::Neg, seed, bits, bits, 0, 0)); expected.state->host_cpu_flags = NegFlags(seed, bits); expected.Complete();
            call.Run(actual); REQUIRE(actual.bytes == expected.bytes);
        }
    }
}

TEST_CASE("immediate shifts execute on all assigned physical registers with identical allocation", "[x64][shift-immediate][native][physical]") {
    const auto config = Configuration();
    for (u32 view : {8u, 16u, 32u, 64u}) {
        Program ordinary, direct; std::array<ir::Value, 12> outputs;
        auto build = [&](Program& p, bool record) {
            std::array<ir::Value, 3> values{p.Load(0), p.Load(8), p.Load(16)};
            const std::array ops{O::LslImm, O::LsrImm, O::AsrImm, O::RorImm};
            for (u32 round = 0; round < ops.size(); ++round) for (u32 lane = 0; lane < values.size(); ++lane) {
                values[lane] = p.Unary(ops[round], values[lane].SetCastType(Type(view)), T::U64, round == 2 ? 64 : 7);
                if (record) outputs[round * 3 + lane] = values[lane];
            }
            for (u32 lane = 0; lane < values.size(); ++lane) { p.Store(128 + lane * 8, values[lane]); p.Store(160 + lane * 8, values[lane]); } p.Finish();
        };
        build(ordinary, false); build(direct, true);
        auto off = x64::Translator::PrepareFunction(ordinary.function, {}, &config, true, true, false,
            {.scalar_unary_phases = true, .scalar_shift_immediates = false});
        auto on = x64::Translator::PrepareFunction(direct.function, {}, &config, true, true, false,
            {.scalar_unary_phases = true, .scalar_shift_immediates = true});
        off->Translate(); on->Translate(); SameAllocationByOrdinal(off->CfgPlan(), on->CfgPlan()); std::set<unsigned> registers;
        for (auto output : outputs) {
            const auto input = output.Def()->GetArg<ir::Value>(0); const auto reg = on->CfgPlan().Register(output.Def()); REQUIRE(reg.has_value());
            REQUIRE(on->CfgPlan().CanReuseUnaryInput(output.Def(), input.Def())); REQUIRE(on->CfgPlan().Register(input.Def()) == reg); registers.insert(*reg);
        }
        const std::set<unsigned> wanted{3, 5, 15}; REQUIRE(registers == wanted);
        REQUIRE(on->Statistics().scalar_coalesced_unary_sites == outputs.size()); REQUIRE(on->Statistics().scalar_shift_immediate_sites == outputs.size());
        REQUIRE(on->Statistics().scalar_shift_zero_sites == 3); REQUIRE(on->getSize() < off->getSize()); PoisonCall old_call{*off}, new_call{*on};
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) {
            Machine baseline, actual, expected;
            for (u32 lane = 0; lane < 3; ++lane) {
                const u64 initial = seed ^ (u64{0x13579bdf2468ace0} * lane); for (auto* machine : {&baseline, &actual, &expected}) machine->Put(lane * 8, initial);
                u64 value = initial;
                for (auto op : {O::LslImm, O::LsrImm, O::AsrImm, O::RorImm}) value = Model(op, value, view, 64, op == O::AsrImm ? 64 : 7, 0);
                expected.Put(128 + lane * 8, value); expected.Put(160 + lane * 8, value);
            }
            expected.Complete(); old_call.Run(baseline); new_call.Run(actual); REQUIRE(actual.bytes == expected.bytes); REQUIRE(baseline.bytes == expected.bytes);
        }
    }
}

TEST_CASE("direct immediate shifts preserve legacy and home-only execution while variable shifts stay unchanged", "[x64][shift-immediate][native][fallback]") {
    const auto config = Configuration();
    for (bool cache : {false, true}) for (bool enabled : {false, true}) {
        Program p; auto source = p.Load(0), count = p.Load(8);
        p.Store(128, p.Unary(O::AsrImm, ir::Value{source.Def()}.SetCastType(T::U16), T::U64, 256));
        p.Store(136, p.as.LslValue(ir::Value{source.Def()}.SetCastType(T::U16), count).SetType(T::U64));
        p.Store(144, p.as.LsrValue(ir::Value{source.Def()}.SetCastType(T::U16), count).SetType(T::U64));
        p.Store(152, p.as.AsrValue(ir::Value{source.Def()}.SetCastType(T::U16), count).SetType(T::U64));
        p.Store(160, p.as.RorValue(ir::Value{source.Def()}.SetCastType(T::U16), count).SetType(T::U64)); p.Finish();
        auto code = x64::Translator::PrepareFunction(p.function, {}, &config, cache, false, false, {.scalar_shift_immediates = enabled}); code->Translate();
        REQUIRE_FALSE(code->CfgPlan().Enabled()); REQUIRE(code->Statistics().scalar_shift_immediate_sites == (enabled ? 1 : 0));
        REQUIRE(code->Statistics().scalar_shift_zero_sites == (enabled ? 1 : 0)); PoisonCall call{*code};
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8000000000000080}, u64{0x0123456789abcdef}}) for (u64 shift : {u64{0}, u64{1}, u64{63}, u64{64}, u64{65}, UINT64_MAX}) {
            Machine actual, expected; for (auto* machine : {&actual, &expected}) { machine->Put(0, seed); machine->Put(8, shift); }
            expected.Put(128, Model(O::AsrImm, seed, 16, 64, 256, 0));
            for (auto [offset, op] : std::array<std::pair<u32, O>, 4>{{{136, O::LslImm}, {144, O::LsrImm}, {152, O::AsrImm}, {160, O::RorImm}}}) expected.Put(offset, Model(op, seed, 16, 64, u32(shift), 0));
            expected.Complete(); call.Run(actual); REQUIRE(actual.bytes == expected.bytes);
        }
    }
    Program p; auto source = p.Load(0), count = p.Load(8); p.Store(128, p.as.LsrValue(source, count).SetType(T::U64)); p.Finish();
    auto code = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, false, {.scalar_shift_immediates = true}); code->Translate();
    REQUIRE(code->Statistics().scalar_shift_immediate_sites == 0); REQUIRE(code->Statistics().scalar_shift_zero_sites == 0);
}

TEST_CASE("direct immediate shifts preserve complete loop Phi helper flags and poisoned frame", "[x64][shift-immediate][native][loop][helper]") {
    const auto config = Configuration();
    for (bool flags : {false, true}) for (bool forwarding : {false, true}) {
        Program ordinary, direct; NativeLoop(ordinary); NativeLoop(direct);
        auto off = x64::Translator::PrepareFunction(ordinary.function, {}, &config, true, true, flags,
            {.uniform_forwarding = forwarding, .scalar_shift_immediates = false});
        auto on = x64::Translator::PrepareFunction(direct.function, {}, &config, true, true, flags,
            {.uniform_forwarding = forwarding, .scalar_shift_immediates = true});
        off->Translate(); on->Translate(); SameAllocationByOrdinal(off->CfgPlan(), on->CfgPlan());
        REQUIRE(on->Statistics().scalar_shift_immediate_sites == 1); REQUIRE(on->Statistics().scalar_shift_zero_sites == 0); REQUIRE(on->getSize() < off->getSize());
        REQUIRE(on->Statistics().helper_call_sites > 0); REQUIRE(on->Statistics().phi_value_copy_sites > 0); PoisonCall old_call{*off}, new_call{*on};
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) for (u64 trips : {u64{1}, u64{2}, u64{7}}) {
            constexpr u64 invariant = 17; Machine baseline, actual, expected;
            for (auto* machine : {&baseline, &actual, &expected}) { machine->Put(8, seed); machine->Put(16, trips); machine->Put(24, invariant); }
            LoopModel(expected, seed, trips, invariant); old_call.Run(baseline); new_call.Run(actual); REQUIRE(actual.bytes == expected.bytes); REQUIRE(baseline.bytes == expected.bytes);
        }
    }
}

TEST_CASE("production direct zero shifts preserve complete State across real protected page faults", "[x64][shift-immediate][production][fault]") {
    struct Mapping { void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); ~Mapping() { if (page != MAP_FAILED) munmap(page, 4096); } } mapping;
    REQUIRE(mapping.page != MAP_FAILED);
    for (bool flags : {false, true}) {
        const auto config = Configuration(); backend::AddressSpace space{config}; backend::ModuleConfig policy; policy.read_only = true;
        policy.feature_overrides.Set(FeatureId::x64_cfg_allocation, true); policy.feature_overrides.Set(FeatureId::x64_flags_residency, flags);
        auto module = space.MapModule(0x1000, 0x2000, policy); Runtime runtime{&space}; Program p;
        auto source = p.Load(8), next = p.Unary(O::AsrImm, source.SetCastType(T::U16), T::U64, 256);
        p.as.SaveFlags(next, F::NZ | F::Parity); p.Store(128, next); p.as.AdvancePC(ir::Imm{u32{4}});
        p.Store(136, p.as.LoadMemory(ir::Operand{p.Load(0)}).SetType(T::U64)); p.Store(144, p.Unary(O::LsrImm, next, T::U64, 7)); p.Finish();
        auto inventory = x64::Translator::PrepareFunction(p.function, {}, &config, true, true, flags); inventory->Translate();
        REQUIRE(inventory->ScalarShiftImmediatesEnabled()); REQUIRE(inventory->Statistics().scalar_shift_immediate_sites == 2); REQUIRE(inventory->Statistics().scalar_shift_zero_sites == 1);
        REQUIRE(module->Push(p.function.body.get())); REQUIRE(backend::TranslateIR(module, p.function.body) != nullptr);
        auto* state = runtime.GetState(); state->pt = nullptr; state->guest_addr_limit = UINT64_MAX; state->guest_addr_mask = UINT64_MAX; state->interp_range_check = nullptr; state->interp_range_check_ctx = nullptr;
        for (bool accessible : {false, true, false}) {
            REQUIRE(mprotect(mapping.page, 4096, accessible ? PROT_READ | PROT_WRITE : PROT_NONE) == 0);
            constexpr u64 memory = 0x9988776655443322ULL, seed = 0x8123456789abcdefULL; if (accessible) std::memcpy(mapping.page, &memory, 8);
            Machine initial; initial.Put(0, reinterpret_cast<u64>(mapping.page)); initial.Put(8, seed);
            std::memcpy(runtime.GetUniformBuffer().data(), initial.state->uniform_buffer_begin, extent); state->host_cpu_flags = initial_flags; runtime.SetLocation(0x1000);
            Machine expected; std::memcpy(expected.bytes.data(), state, expected.bytes.size()); const u64 value = Signed(seed, 16); expected.Put(128, value);
            constexpr u64 mask = (u64{3} << 30) | 255;
            expected.state->host_cpu_flags = (initial_flags & ~mask) | (u64(bool(value >> 63)) << 31) | (u64(value == 0) << 30) | (value & 255);
            if (accessible) { expected.Put(136, memory); expected.Put(144, value >> 7); }
            expected.state->current_loc = ir::Location{0x1004}; expected.state->prev_loc = ir::Location{0x1000}; expected.state->halt_reason = HaltReason::None;
            expected.state->spill_area[backend::kRiscvRecoveryPcSlot] = 0; expected.state->spill_area[backend::kRiscvRecoveryFrameSlot] = 0; expected.state->spill_area[backend::kRiscvMemoryOwnedSlot] = 0;
            const auto saved = _mm_getcsr(); REQUIRE(runtime.Run() == (accessible ? HaltReason::CallHost : HaltReason::PageFatal)); REQUIRE(_mm_getcsr() == saved);
            REQUIRE(std::memcmp(state, expected.bytes.data(), expected.bytes.size()) == 0);
        }
    }
}

TEST_CASE("direct shifts mask complete U64 immediate values before selecting the instruction count", "[x64][shift-immediate][native][u64]") {
    const auto config = Configuration();
    for (auto op : {O::LslImm, O::LsrImm, O::AsrImm, O::RorImm}) for (u32 bits : {8u, 16u, 32u, 64u})
        for (u64 count : {u64{0}, u64{0x100000000}, u64{0x100000001}, u64{0x8000000000000000}, u64{0x800000000000003f}, UINT64_MAX}) {
            Program ordinary, direct;
            auto build = [&](Program& p) { auto source = p.Load(0); auto output = ir::Value{p.as.AppendInst(op, source.SetCastType(T::U16), ir::Imm{count})}.SetType(Type(bits)); p.Store(128, output); p.Finish(); };
            build(ordinary); build(direct);
            auto off = x64::Translator::PrepareFunction(ordinary.function, {}, &config, true, true, false, {.scalar_shift_immediates = false});
            auto on = x64::Translator::PrepareFunction(direct.function, {}, &config, true, true, false, {.scalar_shift_immediates = true});
            off->Translate(); on->Translate(); SameAllocationByOrdinal(off->CfgPlan(), on->CfgPlan());
            REQUIRE(on->Statistics().scalar_shift_immediate_sites == 1); REQUIRE(on->Statistics().scalar_shift_zero_sites == (count % bits == 0 ? 1 : 0));
            PoisonCall old_call{*off}, new_call{*on};
            for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8123456789abcdef}}) {
                CAPTURE(op, bits, count, seed); Machine baseline, actual, expected; for (auto* machine : {&baseline, &actual, &expected}) machine->Put(0, seed);
                // Declared extents are at most 64, so only the low six bits of
                // this independently reduced full U64 count can affect math.
                const u32 reduced = u32(count % bits); expected.Put(128, Model(op, seed, 16, bits, reduced, 0)); expected.Complete();
                old_call.Run(baseline); new_call.Run(actual); REQUIRE(actual.bytes == expected.bytes); REQUIRE(baseline.bytes == expected.bytes);
            }
        }
}

TEST_CASE("non-scalar result widths retain their existing immediate-shift fallback encoding", "[x64][shift-immediate][validation]") {
    const auto config = Configuration();
    for (auto op : {O::LslImm, O::LsrImm, O::AsrImm, O::RorImm}) for (T result : {T::VOID, T::V128}) {
        Program ordinary, direct;
        auto build = [&](Program& p) { auto source = p.Load(0); p.Unary(op, source, result, 0); p.Finish(); };
        build(ordinary); build(direct);
        auto off = x64::Translator::PrepareFunction(ordinary.function, {}, &config, false, false, false, {.scalar_shift_immediates = false});
        auto on = x64::Translator::PrepareFunction(direct.function, {}, &config, false, false, false, {.scalar_shift_immediates = true});
        off->Translate(); on->Translate();
        REQUIRE(on->Statistics().scalar_shift_immediate_sites == 0); REQUIRE(on->Statistics().scalar_shift_zero_sites == 0);
        REQUIRE(on->getSize() == off->getSize());
        REQUIRE(std::memcmp(on->getCode(), off->getCode(), on->getSize()) == 0);
        // Preserve existing emission; these unsupported-width units are never
        // executed and establish no additional scalar IR legality contract.
    }
    for (bool enabled : {false, true}) {
        Program p; auto source = p.Load(0); p.Unary(O::LslImm, source, T::V256, 0); p.Finish();
        REQUIRE_THROWS([&] {
            auto code = x64::Translator::PrepareFunction(p.function, {}, &config, false, false, false, {.scalar_shift_immediates = enabled});
        }());
    }
}

TEST_CASE("SingleInstruction construction disables immediate shift policy", "[x64][shift-immediate][mode-policy]") {
    // Constructor policy validation only; Step translation/execution and its
    // specialized acceptance remain outside this optimization request.
    const auto config = Configuration();
    for (bool requested : {false, true}) {
        Program p; p.Finish();
        x64::Translator code{p.function.body.get(), {}, &config, nullptr, false, x64::ExecutionMode::SingleInstruction,
            false, false, true, true, true, true, true, requested};
        REQUIRE_FALSE(code.ScalarShiftImmediatesEnabled()); REQUIRE(code.Statistics().scalar_shift_immediate_sites == 0);
        REQUIRE(code.Statistics().scalar_shift_zero_sites == 0);
    }
}


