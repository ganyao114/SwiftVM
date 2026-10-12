#include <array>
#include <cstring>
#include <sys/mman.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/function_plan.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using T = ir::ValueType;
namespace {
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 256> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() {
        for (u32 i = 0; i < 256; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 13);
        state->host_cpu_flags = 0xabcde98765432109ULL;
    }
    ~Machine() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    u64 Get(u32 offset) const { u64 value; std::memcpy(&value, state->uniform_buffer_begin + offset, 8); return value; }
};
void Equal(const Machine& a, const Machine& b) {
    REQUIRE(std::memcmp(a.state->uniform_buffer_begin, b.state->uniform_buffer_begin, 256) == 0);
    REQUIRE(a.state->host_cpu_flags == b.state->host_cpu_flags);
    REQUIRE(a.state->current_loc == b.state->current_loc);
    REQUIRE(a.state->prev_loc == b.state->prev_loc);
    REQUIRE(a.state->halt_reason == b.state->halt_reason);
    REQUIRE(a.state->spill_area == b.state->spill_area);
}
HaltReason Run(x64::Translator& translator, Machine& machine) {
    return translator.getCode<HaltReason (*)(backend::State*)>()(machine.state);
}
ir::Value Binary(ir::Assembler& as, ir::OpCode op, ir::Value value, u64 immediate) {
    return ir::Value{as.AppendInst(op, value, ir::Operand{ir::Imm{immediate}})}.SetType(T::U64);
}
void Store(ir::Assembler& as, u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
u64 Helper(u64 a, u64 b, u64 c) {
    asm volatile("xor %%r8, %%r8\n\txor %%r9, %%r9\n\txor %%r10, %%r10\n\txor %%r11, %%r11"
                 ::: "r8", "r9", "r10", "r11", "cc");
    return a * 3 + b * 5 + c * 7;
}
struct ArithmeticModel { u64 result, flags; };
ArithmeticModel ModelArithmetic(ir::OpCode op, u32 bits, u64 left, u64 right, bool carry,
                                ir::Flags requested, u64 previous_flags) {
    const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
    const u64 sign = u64{1} << (bits - 1);
    left &= mask; right &= mask;
    const bool subtract = op == ir::OpCode::Sub || op == ir::OpCode::Sbb;
    const u64 extra = op == ir::OpCode::Adc ? carry : op == ir::OpCode::Sbb ? !carry : 0;
    const unsigned __int128 operand = static_cast<unsigned __int128>(right) + extra;
    const unsigned __int128 wide = subtract ? static_cast<unsigned __int128>(left) - operand
                                            : static_cast<unsigned __int128>(left) + operand;
    const u64 result = static_cast<u64>(wide) & mask;
    const bool next_carry = subtract ? static_cast<unsigned __int128>(left) >= operand : wide > mask;
    const auto signed_value = [&](u64 value) {
        return static_cast<__int128>(value) - ((value & sign) ? (__int128{1} << bits) : 0);
    };
    const __int128 signed_result = subtract ? signed_value(left) - signed_value(right) - extra
                                            : signed_value(left) + signed_value(right) + extra;
    const bool overflow = signed_result < -(__int128{1} << (bits - 1)) ||
                          signed_result >= (__int128{1} << (bits - 1));
    const bool auxiliary = subtract ? (left & 15) < (right & 15) + extra
                                    : (left & 15) + (right & 15) + extra > 15;
    u64 published = previous_flags;
    const auto bit = [&](ir::Flags flag, u32 position, bool value) {
        if (True(requested & flag)) published = (published & ~(u64{1} << position)) | (u64(value) << position);
    };
    bit(ir::Flags::Negate, 31, result & sign); bit(ir::Flags::Zero, 30, result == 0);
    bit(ir::Flags::Carry, 29, next_carry); bit(ir::Flags::Overflow, 28, overflow);
    bit(ir::Flags::AuxiliaryCarry, 26, auxiliary);
    // SwiftVM stores the low result byte for the later parity predicate.
    if (True(requested & ir::Flags::Parity)) published = (published & ~u64{255}) | (result & 255);
    return {result, published};
}
}

TEST_CASE("whole CFG registers survive loop Phi swaps pressure and returning helpers", "[x64][cfg-allocation][execution]") {
    for (bool callback : {false, true}) for (bool flags : {false, true}) {
        backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000});
        ir::Assembler as{plan.body.get()};
        auto stable = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto count = as.AddPhi(ir::Params{}).SetType(T::U64);
        auto a = as.AddPhi(ir::Params{}).SetType(T::U64);
        auto b = as.AddPhi(ir::Params{}).SetType(T::U64);
        if (callback) {
            const auto result = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, a, b, stable).SetType(T::U64);
            Store(as, 32, result);
        }
        auto next = Binary(as, ir::OpCode::Sub, count, 1);
        auto branch = as.Goto(as.TestNotZero(next));
        as.BindLabel(branch);
        auto* label = &plan.body->GetInstList().back();
        plan.body->RemoveInst(label); plan.body->InsertBefore(label, count.Def());
        ir::Params count_inputs; count_inputs.Push(ir::Imm{u64{9}}); count_inputs.Push(next); count.Def()->SetArg(0, count_inputs);
        ir::Params a_inputs; a_inputs.Push(ir::Imm{u64{11}}); a_inputs.Push(b); a.Def()->SetArg(0, a_inputs);
        ir::Params b_inputs; b_inputs.Push(ir::Imm{u64{22}}); b_inputs.Push(a); b.Def()->SetArg(0, b_inputs);
        Store(as, 8, a); Store(as, 16, b); Store(as, 24, Binary(as, ir::OpCode::Add, stable, 77));
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator off{plan.body.get(), {}, nullptr, &plan, true}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; on.Translate();
        for (u64 seed : {u64{0}, u64{0x8123456789abcdef}, UINT64_MAX}) {
            Machine baseline, actual; baseline.Put(0, seed); actual.Put(0, seed);
            REQUIRE(Run(off, baseline) == HaltReason::CallHost);
            REQUIRE(Run(on, actual) == HaltReason::CallHost); Equal(baseline, actual);
            REQUIRE(actual.Get(8) == 11); REQUIRE(actual.Get(16) == 22); REQUIRE(actual.Get(24) == seed + 77);
            if (callback) REQUIRE(actual.Get(32) == 11 * 3 + 22 * 5 + seed * 7);
        }
        REQUIRE(on.Statistics().scalar_cache_read_sites > 0);
        REQUIRE(on.FramePlan().Enabled());
        REQUIRE(on.FrameSize() < off.FrameSize());
    }
}

TEST_CASE("whole CFG allocation resolves critical diamond Phi edges and delayed uses", "[x64][cfg-allocation][execution]") {
    for (bool invert : {false, true}) for (bool flags : {false, true}) {
        backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000});
        ir::Assembler as{plan.body.get()};
        auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
        auto input = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        auto initial = Binary(as, ir::OpCode::Add, input, 17);
        auto conditional = invert ? as.NotGoto(ir::BOOL{condition}) : as.Goto(ir::BOOL{condition});
        auto updated = Binary(as, ir::OpCode::Xor, input, 0x85);
        auto jump = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
        as.BindLabel(jump); auto* label = &plan.body->GetInstList().back();
        ir::Params inputs; inputs.Push(initial); inputs.Push(updated);
        auto phi = as.AddPhi(inputs).SetType(T::U64);
        auto result = Binary(as, ir::OpCode::Add, phi, 11);
        Store(as, 16, result); Store(as, 24, input);
        plan.targets[conditional.Def()] = label; plan.targets[jump.Def()] = label; plan.stops.insert(jump.Def());
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator off{plan.body.get(), {}, nullptr, &plan, true}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; on.Translate();
        for (u64 seed : {u64{0}, u64{0x8123456789abcdef}, UINT64_MAX}) for (u64 test : {u64{0}, u64{1}, u64{255}}) {
            Machine baseline, actual; baseline.Put(0, test); actual.Put(0, test); baseline.Put(8, seed); actual.Put(8, seed);
            REQUIRE(Run(off, baseline) == HaltReason::CallHost);
            REQUIRE(Run(on, actual) == HaltReason::CallHost); Equal(baseline, actual);
            REQUIRE(actual.Get(16) == ((invert ? test == 0 : test != 0) ? seed + 17 : seed ^ 0x85) + 11);
            REQUIRE(actual.Get(24) == seed);
        }
        REQUIRE(on.Statistics().scalar_cache_read_sites > 0);
    }
}

TEST_CASE("whole CFG allocation retains scalar raw containers consumed by vector Phi", "[x64][cfg-allocation][raw]") {
    for (auto type : {T::U8, T::U32, T::U64}) for (bool flags : {false, true}) {
        backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000}); ir::Assembler as{plan.body.get()};
        auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
        auto first = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
        auto skip = as.Goto(ir::BOOL{condition});
        auto second = as.LoadUniform(ir::Uniform{16, type}).SetType(type);
        as.BindLabel(skip);
        ir::Params inputs; inputs.Push(first.SetCastType(T::V128)); inputs.Push(second.SetCastType(T::V128));
        auto phi = as.AddPhi(inputs).SetType(T::V128);
        Store(as, 32, phi); Store(as, 48, first.SetCastType(T::V128));
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator off{plan.body.get(), {}, nullptr, &plan}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; on.Translate();
        REQUIRE(on.CfgPlan().RequiresCanonicalHome(first.Def())); REQUIRE(on.CfgPlan().RequiresCanonicalHome(second.Def()));
        const auto bits = ir::GetValueSizeByte(type) * 8; const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        for (u64 selected : {u64{0}, u64{1}}) {
            Machine expected, actual; expected.Put(0, selected); actual.Put(0, selected);
            expected.Put(8, UINT64_MAX); actual.Put(8, UINT64_MAX);
            expected.Put(16, 0x123456789abcdef0ULL); actual.Put(16, 0x123456789abcdef0ULL);
            REQUIRE(Run(off, expected) == HaltReason::CallHost); REQUIRE(Run(on, actual) == HaltReason::CallHost); Equal(expected, actual);
            REQUIRE(actual.Get(32) == ((selected ? UINT64_MAX : 0x123456789abcdef0ULL) & mask));
            REQUIRE(actual.Get(40) == 0); REQUIRE(actual.Get(48) == mask); REQUIRE(actual.Get(56) == 0);
        }
    }
}

TEST_CASE("production whole CFG loop fault publishes committed flags and clears recovery slots", "[x64][cfg-allocation][production][fault]") {
    std::array<UniformRangeDesc, 5> lanes{{{0, 8}, {8, 8}, {16, 8}, {24, 8}, {32, 8}}};
    struct Mapping { void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ~Mapping() { if (page != MAP_FAILED) munmap(page, 4096); } } mapping;
    REQUIRE(mapping.page != MAP_FAILED);
    for (bool cfg : {false, true}) for (bool flags : {false, true}) {
        REQUIRE(mprotect(mapping.page, 4096, PROT_NONE) == 0);
        Config config{.loc_start = 0, .loc_end = 0x10000, .enable_jit = true, .backend_isa = kX86_64,
                      .uniform_buffer_size = 256, .loop_gpr_uniform_ranges = lanes,
                      .static_program = true, .global_opts = Optimizations::UniformElimination, .stack_alignment = 16};
        backend::AddressSpace space{config}; backend::ModuleConfig policy;
        policy.feature_overrides.Set(FeatureId::x64_cfg_allocation, cfg);
        policy.feature_overrides.Set(FeatureId::x64_flags_residency, flags);
        auto module = space.MapModule(0x1000, 0x2000, policy); Runtime runtime{&space};
        {
            ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0x1000});
            auto seed = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
            auto root = function->Add<ir::U64>(seed, ir::Operand{ir::Imm{u64{17}}});
            function->StoreUniform(ir::Uniform{8, T::U64}, root);
            builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0x1100}}); builder.SetCurBlock(ir::Location{0x1100});
            ir::Params inputs; inputs.Push(root); auto phi = function->AddPhi<ir::U64>(inputs);
            auto committed = function->Add<ir::U64>(phi, ir::Operand{ir::Imm{u64{23}}});
            function->SaveFlags(committed, ir::Flags::NZCV); function->StoreUniform(ir::Uniform{16, T::U64}, committed);
            auto address = function->LoadUniform<ir::U64>(ir::Uniform{24, T::U64});
            auto loaded = function->LoadMemory<ir::U64>(ir::Operand{address}); function->StoreUniform(ir::Uniform{32, T::U64}, loaded);
            function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
            REQUIRE(x64::CompileFunctions(module, std::span{&function, 1}) != nullptr);
            REQUIRE(space.GetCodeCache(0x1100) == nullptr);
        }
        for (bool accessible : {false, true}) {
            if (accessible) { REQUIRE(mprotect(mapping.page, 4096, PROT_READ | PROT_WRITE) == 0); const u64 bytes = 0x9988776655443322ULL; std::memcpy(mapping.page, &bytes, 8); }
            const u64 seed = UINT64_MAX - 35, pointer = reinterpret_cast<u64>(mapping.page), sentinel = 0x1122334455667788ULL;
            auto buffer = runtime.GetUniformBuffer(); std::memcpy(buffer.data(), &seed, 8); std::memcpy(buffer.data() + 24, &pointer, 8); std::memcpy(buffer.data() + 32, &sentinel, 8);
            runtime.GetState()->host_cpu_flags = 0xabcde98765432109ULL; runtime.GetState()->guest_addr_limit = UINT64_MAX; runtime.SetLocation(0x1000);
            REQUIRE(runtime.Run() == (accessible ? HaltReason::CallHost : HaltReason::PageFatal));
            u64 actual{}; std::memcpy(&actual, buffer.data() + 8, 8); REQUIRE(actual == seed + 17);
            std::memcpy(&actual, buffer.data() + 16, 8); REQUIRE(actual == seed + 40);
            std::memcpy(&actual, buffer.data() + 32, 8); REQUIRE(actual == (accessible ? 0x9988776655443322ULL : sentinel));
            REQUIRE(runtime.GetState()->host_cpu_flags == ((0xabcde98765432109ULL & ~(u64{15} << 28)) | (u64{1} << 29)));
            REQUIRE(runtime.GetLocation() == 0x1100);
            REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
            REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
            REQUIRE(runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] == 0);
        }
    }
}

TEST_CASE("whole CFG binary reuse captures wider repeated RHS before destructive narrow writes", "[x64][cfg-allocation][coalescing]") {
    for (auto op : {ir::OpCode::Add, ir::OpCode::Sub, ir::OpCode::Adc, ir::OpCode::Sbb,
                    ir::OpCode::And, ir::OpCode::Or, ir::OpCode::Xor, ir::OpCode::AndNot})
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool flags : {false, true}) {
        CAPTURE(op, type, flags);
        backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000});
        ir::Assembler as{plan.body.get()};
        auto stable = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        ir::Operand rhs{input.SetCastType(T::U64), ir::Imm{u64{8}}, ir::OperandLsr};
        auto output = ir::Value{as.AppendInst(op, input.SetCastType(type), rhs)}.SetType(type);
        as.StoreUniform(ir::Uniform{16, T::U64}, output.SetCastType(T::U64));
        Store(as, 24, stable);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator off{plan.body.get(), {}, nullptr, &plan, true}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; on.Translate();
        REQUIRE(on.CfgPlan().CanReuseBinaryInput(output.Def(), input.Def()));
        REQUIRE(on.CfgPlan().Register(output.Def()) == on.CfgPlan().Register(input.Def()));
        REQUIRE(on.Statistics().scalar_coalesced_binary_sites == 1);
        const auto bits = ir::GetValueSizeByte(type) * 8;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}, u64{0x80000000000000ff}})
        for (bool carry : {false, true}) {
            const u64 left = seed & mask, right = seed >> 8;
            u64 expected{};
            switch (op) {
                case ir::OpCode::Add: expected = left + right; break;
                case ir::OpCode::Sub: expected = left - right; break;
                case ir::OpCode::Adc: expected = left + right + carry; break;
                case ir::OpCode::Sbb: expected = left - right - !carry; break;
                case ir::OpCode::And: expected = left & right; break;
                case ir::OpCode::Or: expected = left | right; break;
                case ir::OpCode::Xor: expected = left ^ right; break;
                case ir::OpCode::AndNot: expected = left & ~right; break;
                default: FAIL("uncovered binary operation");
            }
            Machine baseline, actual;
            for (auto* machine : {&baseline, &actual}) {
                machine->Put(0, seed); machine->Put(8, 0x9988776655443322ULL);
                machine->state->host_cpu_flags = (machine->state->host_cpu_flags & ~(u64{1} << 29)) | (u64(carry) << 29);
            }
            REQUIRE(Run(off, baseline) == HaltReason::CallHost);
            REQUIRE(Run(on, actual) == HaltReason::CallHost); Equal(baseline, actual);
            REQUIRE(actual.Get(16) == (expected & mask));
            REQUIRE(actual.Get(24) == 0x9988776655443322ULL);
        }
    }
}

TEST_CASE("whole CFG delayed flags preserve native snapshots across destructive register reuse", "[x64][cfg-allocation][coalescing][flags]") {
    for (auto op : {ir::OpCode::Add, ir::OpCode::Sub, ir::OpCode::Adc, ir::OpCode::Sbb})
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (auto requested : {ir::Flags::CV, ir::Flags::NZCV, ir::Flags::NZ, ir::Flags::Parity, ir::Flags::All})
    for (bool flags : {false, true}) for (bool raw : {false, true}) {
        CAPTURE(op, type, requested, flags, raw);
        backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000});
        ir::Assembler as{plan.body.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        if (raw) {
            // Real raw reads require the original canonical home, even when
            // its last scalar use destructively shares the producer register.
            auto container = as.BitCast(ir::Value{input.Def()}.SetCastType(T::V128)).SetType(T::V128);
            Store(as, 64, container);
            // SetCastType mutates the Value view. Explicit fresh scalar views
            // make these profitable audited reads, rather than raw V128 reads.
            Store(as, 80, ir::Value{input.Def()}.SetCastType(T::U64));
            Store(as, 88, ir::Value{input.Def()}.SetCastType(T::U64));
        }
        auto rhs = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        // Keep RHS home-backed so the allocator's lhs/output affinity must
        // exercise destructive reuse even with the two-register flags budget.
        auto rhs_container = as.BitCast(rhs.SetCastType(T::V128)).SetType(T::V128);
        Store(as, 96, rhs_container);
        auto producer = ir::Value{as.AppendInst(op, input.SetCastType(type), ir::Operand{rhs.SetCastType(type)})}.SetType(type);
        as.StoreUniform(ir::Uniform{32, T::U64}, producer.SetCastType(T::U64));
        auto pressure = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        for (u64 i = 1; i <= 6; ++i) pressure = Binary(as, i % 2 ? ir::OpCode::Add : ir::OpCode::Sub, pressure, i);
        Store(as, 40, pressure);
        as.SaveFlags(producer, requested);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator off{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; on.Translate();
        REQUIRE(on.CfgPlan().CanReuseBinaryInput(producer.Def(), input.Def()));
        REQUIRE(on.CfgPlan().Register(input.Def()));
        REQUIRE(on.CfgPlan().Register(producer.Def()) == on.CfgPlan().Register(input.Def()));
        REQUIRE(on.CfgPlan().RequiresCanonicalHome(input.Def()) == raw);
        REQUIRE(on.CfgPlan().Interferes(producer.Def(), pressure.Def()) ==
                (requested == ir::Flags::NZ || requested == ir::Flags::Parity || requested == ir::Flags::All));
        REQUIRE(on.Statistics().scalar_coalesced_binary_sites >= 7);
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1, sign = u64{1} << (bits - 1);
        struct Inputs { u64 left, right; };
        for (auto operands : std::array<Inputs, 8>{{{0, 0}, {mask, 1}, {sign - 1, 1}, {sign, 1},
                                                  {sign, mask}, {0, mask}, {mask, mask},
                                                  {0x123456789abcdef0ULL, 0xfedcba9876543210ULL}}})
        for (bool carry : {false, true}) {
            Machine baseline, actual;
            for (auto* machine : {&baseline, &actual}) {
                machine->Put(0, operands.left); machine->Put(8, operands.right);
                machine->Put(16, 0x9988776655443322ULL);
                machine->state->host_cpu_flags = (machine->state->host_cpu_flags & ~(u64{1} << 29)) | (u64(carry) << 29);
            }
            const auto expected = ModelArithmetic(op, bits, operands.left, operands.right, carry, requested, actual.state->host_cpu_flags);
            REQUIRE(Run(off, baseline) == HaltReason::CallHost);
            REQUIRE(Run(on, actual) == HaltReason::CallHost); Equal(baseline, actual);
            REQUIRE(actual.Get(32) == expected.result); REQUIRE(actual.state->host_cpu_flags == expected.flags);
            REQUIRE(actual.Get(40) == 0x998877665544331fULL);
            REQUIRE(actual.Get(96) == operands.right); REQUIRE(actual.Get(104) == 0);
            if (raw) {
                REQUIRE(actual.Get(64) == operands.left); REQUIRE(actual.Get(72) == 0);
                REQUIRE(actual.Get(80) == operands.left); REQUIRE(actual.Get(88) == operands.left);
            }
        }
    }
}

TEST_CASE("whole CFG flags-only delayed arithmetic separates scalar result homes from native metadata", "[x64][cfg-allocation][flags]") {
    for (auto op : {ir::OpCode::Add, ir::OpCode::Sub, ir::OpCode::Adc, ir::OpCode::Sbb})
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (auto requested : {ir::Flags::CV, ir::Flags::NZCV, ir::Flags::NZ, ir::Flags::Parity, ir::Flags::All})
    for (bool flags : {false, true}) {
        CAPTURE(op, type, requested, flags);
        backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000}); ir::Assembler as{plan.body.get()};
        auto left = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        auto right = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
        auto producer = ir::Value{as.AppendInst(op, left, ir::Operand{right})}.SetType(type);
        const bool reads_result = requested == ir::Flags::NZ || requested == ir::Flags::Parity || requested == ir::Flags::All;
        // Repeated real result reads make residency profitable without adding
        // a scalar result store. The final publication remains delayed.
        if (reads_result) for (u32 i = 0; i < 8; ++i) as.SaveFlags(producer, requested);
        auto pressure = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        for (u64 i = 1; i <= 6; ++i) pressure = Binary(as, ir::OpCode::Add, pressure, i);
        Store(as, 40, pressure); as.SaveFlags(producer, requested);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator off{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, false, flags}; off.Translate();
        x64::Translator on{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, true, flags}; on.Translate();
        REQUIRE(on.CfgPlan().Interferes(producer.Def(), pressure.Def()) == reads_result);
        REQUIRE_FALSE(on.CfgPlan().RequiresCanonicalHome(producer.Def()));
        REQUIRE(bool(on.CfgPlan().Register(producer.Def())) == reads_result);
        REQUIRE(on.Statistics().flags_snapshot_sites == (requested == ir::Flags::NZ || requested == ir::Flags::Parity ? 0 : 1));
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1, sign = u64{1} << (bits - 1);
        for (auto operands : std::array<std::array<u64, 2>, 5>{{{mask, 1}, {sign - 1, 1}, {sign, mask}, {0, mask}, {15, 0}}})
        for (bool carry : {false, true}) {
            Machine baseline, actual;
            for (auto* machine : {&baseline, &actual}) {
                machine->Put(0, operands[0]); machine->Put(8, operands[1]); machine->Put(16, 100);
                machine->state->host_cpu_flags = (machine->state->host_cpu_flags & ~(u64{1} << 29)) | (u64(carry) << 29);
            }
            const auto expected = ModelArithmetic(op, bits, operands[0], operands[1], carry, requested, actual.state->host_cpu_flags);
            REQUIRE(Run(off, baseline) == HaltReason::CallHost); REQUIRE(Run(on, actual) == HaltReason::CallHost);
            Equal(baseline, actual); REQUIRE(actual.state->host_cpu_flags == expected.flags); REQUIRE(actual.Get(40) == 121);
        }
    }
}
