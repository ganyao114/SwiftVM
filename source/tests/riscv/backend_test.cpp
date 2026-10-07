#include "test_support.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/mman.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/atomic_fallback.h"
#include "runtime/backend/riscv64/jit/translator.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/backedge_control.h"
#include "runtime/frontend/ir_assembler.h"
#include "translator/x86/translator.h"
#include "translator/arm64/translator.h"

using namespace swift;
using namespace swift::runtime;
namespace rv = swift::runtime::backend::riscv64;
using ir::OpCode;
using ir::ValueType;

namespace {

using namespace swift::tests::riscv;

#if defined(__riscv) && __riscv_xlen == 64
Runtime* abi_runtime{};
HaltReason abi_halt{};
HaltReason RunRuntimeForABI(backend::State*) { return abi_halt = abi_runtime->Run(); }
#endif

IntrusivePtr<ir::Block> ScalarProgram(OpCode op, ValueType type, bool flags = false) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x1000})};
    ir::Assembler as{block.get()};
    auto left = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto right = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
    ir::Value result;
    switch (op) {
        case OpCode::Add: case OpCode::Sub: case OpCode::Adc: case OpCode::Sbb:
        case OpCode::Mul: case OpCode::Div: case OpCode::And: case OpCode::Or:
        case OpCode::Xor: case OpCode::AndNot:
            result = ir::Value{block->AppendInst(op, left, ir::Operand{right})}.SetType(type); break;
        case OpCode::LslValue: case OpCode::LsrValue: case OpCode::AsrValue: case OpCode::RorValue:
            result = ir::Value{block->AppendInst(op, left, right)}.SetType(type); break;
        case OpCode::LslImm: case OpCode::LsrImm: case OpCode::AsrImm: case OpCode::RorImm:
            result = ir::Value{block->AppendInst(op, left, ir::Imm{u64{65}})}.SetType(type); break;
        case OpCode::MulHigh:
            result = as.MulHigh(left, right, ir::Imm{ir::IsSignValueType(type)}).SetType(ValueType::U64); break;
        case OpCode::MulSub:
            result = as.MulSub(left, right, left).SetType(type); break;
        case OpCode::SignedDiv64:
            result = as.SignedDiv64(left, right).SetType(ValueType::U64); break;
        case OpCode::TestBit:
            result = as.TestBit(left, ir::Imm{u64{7}}).SetType(ValueType::U8); break;
        case OpCode::ZeroExtend32To64: case OpCode::ZeroExtend64:
            result = ir::Value{block->AppendInst(op, left)}.SetType(ValueType::U64); break;
        case OpCode::ZeroExtend32:
            result = as.ZeroExtend32(left).SetType(ValueType::U32); break;
        case OpCode::SignExtend:
            result = as.SignExtend(left).SetType(ValueType::U64); break;
        case OpCode::Neg: case OpCode::Not: case OpCode::TestZero: case OpCode::TestNotZero:
            result = ir::Value{block->AppendInst(op, left)}.SetType(type); break;
        default: throw std::runtime_error("unknown scalar fixture");
    }
    if (flags) as.SaveFlags(result, ir::Flags::Carry | ir::Flags::Overflow | ir::Flags::Negate |
                                   ir::Flags::Zero | ir::Flags::Parity | ir::Flags::AuxiliaryCarry);
    as.StoreUniform(ir::Uniform{16, result.Type()}, result);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

void ScalarDifferential() {
    const std::array ops{OpCode::Add, OpCode::Sub, OpCode::Adc, OpCode::Sbb,
        OpCode::Mul, OpCode::Div, OpCode::And, OpCode::Or, OpCode::Xor, OpCode::AndNot,
        OpCode::Neg, OpCode::Not, OpCode::TestBit, OpCode::TestZero, OpCode::TestNotZero,
        OpCode::LslValue, OpCode::LsrValue, OpCode::AsrValue, OpCode::RorValue,
        OpCode::LslImm, OpCode::LsrImm, OpCode::AsrImm, OpCode::RorImm,
        OpCode::ZeroExtend32, OpCode::ZeroExtend32To64, OpCode::ZeroExtend64,
        OpCode::SignExtend, OpCode::MulHigh, OpCode::MulSub, OpCode::SignedDiv64};
    const std::array types{ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64,
                          ValueType::S8, ValueType::S16, ValueType::S32, ValueType::S64};
    const std::array<u64, 10> edges{0, 1, 2, 63, 64, 65, UINT64_MAX,
                                   0x8000000000000000ULL, 0x7fffffffffffffffULL, 0xabcdef1234567890ULL};
    u64 seed = 0x742305df27b19381ULL;
    for (auto type : types) for (auto op : ops) {
        auto code_ir = ScalarProgram(op, type, true);
        auto reference_ir = ScalarProgram(op, type, true);
        Compiled compiled{code_ir.get()};
        Check(compiled.translator.Stats().direct >= 4, "scalar fixture uses direct RV64 lowering");
        for (unsigned i = 0; i < 96; ++i) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            const u64 left = i < edges.size() ? edges[i] : seed;
            const u64 right = i < edges.size() ? edges[(i + 3) % edges.size()] : std::rotl(seed, 17);
            StateStorage actual, expected;
            actual.Put(0, left); actual.Put(8, right);
            expected.Put(0, left); expected.Put(8, right);
            actual.state->host_cpu_flags = expected.state->host_cpu_flags = i & 1 ? UINT64_MAX : 0;
            backend::interp::Interpreter reference{*expected.state, reference_ir.get()};
            const auto wanted = reference.Run();
            const auto observed = compiled.fn(actual.state);
            const auto detail = std::string{ir::GetIRMetaInfo(op).name} + "/" + ir::ValueTypeString(type) + "/" + std::to_string(i);
            Check(observed == wanted, detail + " halt");
            Check(actual.Get(16) == expected.Get(16), detail + " value");
            Check(actual.state->host_cpu_flags == expected.state->host_cpu_flags, detail + " flags");
            if (op == OpCode::Neg) {
                const auto bits = ir::GetValueSizeByte(type) * 8;
                const auto mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
                Check(((actual.state->host_cpu_flags >> 29) & 1) == ((left & mask) == 0),
                      detail + " independent negate carry");
                Check(((actual.state->host_cpu_flags >> 28) & 1) == ((left & mask) == (u64{1} << (bits - 1))),
                      detail + " independent negate overflow");
            }
        }
#if defined(__riscv) && __riscv_xlen == 64
        StateStorage abi;
        Check(SwiftRiscvCheckABI(compiled.fn, abi.state) == 1, "scalar C ABI preserved");
#endif
    }
    std::cout << "PASS scalar differential: 240 programs, 23040 executions\n";
}

void Conditions() {
    for (unsigned condition = 0; condition < 16; ++condition) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2000})};
        ir::Assembler as{block.get()};
        auto result = as.CondSet(ir::Cond(condition)).SetType(ValueType::U8);
        as.StoreUniform(ir::Uniform{0, ValueType::U8}, result);
        block->SetTerminal(ir::terminal::Condition{ir::Cond(condition),
            ir::terminal::LinkBlock{ir::Location{0x1111}}, ir::terminal::LinkBlock{ir::Location{0x2222}}});
        Compiled compiled{block.get()};
        for (unsigned flags = 0; flags < 16; ++flags) {
            StateStorage actual, expected;
            actual.state->host_cpu_flags = expected.state->host_cpu_flags = u64(flags) << 28;
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(reference.Run() == compiled.fn(actual.state), "condition halt");
            Check(actual.Get(0) == expected.Get(0), "condition materializes 0/1");
            Check(actual.state->current_loc == expected.state->current_loc, "condition terminal");
        }
    }
    std::cout << "PASS all 16 conditions and NZCV combinations\n";
}

void FlagPredicates() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2500})};
    ir::Assembler as{block.get()};
    const std::array flags{ir::Flags::Negate, ir::Flags::Zero, ir::Flags::Carry, ir::Flags::Overflow};
    for (u32 i = 0; i < flags.size(); ++i) {
        // Match AArch64 CondPassed: no SetType and no typed template argument.
        auto yes = as.TestFlags(flags[i]);
        auto no = as.TestNotFlags(flags[i]);
        Check(yes.Type() == ValueType::U8 && no.Type() == ValueType::U8,
              "flag-only predicates infer their boolean result type");
        as.StoreUniform(ir::Uniform{i * 2, ValueType::U8}, yes);
        as.StoreUniform(ir::Uniform{i * 2 + 1, ValueType::U8}, no);
    }
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{block.get()};
    for (u32 nzcv = 0; nzcv < 16; ++nzcv) {
        StateStorage actual;
        actual.state->host_cpu_flags = u64(nzcv) << 28;
        Check(compiled.fn(actual.state) == HaltReason::CallHost, "untyped flag predicate halt");
        for (u32 i = 0; i < flags.size(); ++i) {
            const auto expected = (nzcv >> (3 - i)) & 1;
            Check(actual.state->uniform_buffer_begin[i * 2] == expected &&
                  actual.state->uniform_buffer_begin[i * 2 + 1] == (expected ^ 1),
                  "untyped flag predicates match independent NZCV bits");
        }
    }
    std::cout << "PASS canonical flag-only predicates without explicit SetType\n";
}

u64 Helper(u64 a, u64 b, u64 c) { return (a ^ std::rotl(b, 13)) + c; }

void MixedHelpers() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3000})};
    ir::Assembler as{block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto b = as.LoadUniform(ir::Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    auto sum = as.Add(a, ir::Operand{b}).SetType(ValueType::U64);
    auto call = as.CallHost(&Helper, a, b, sum).SetType(ValueType::U64);
    auto result = as.Xor(sum, ir::Operand{call}).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{16, ValueType::U64}, result);
    auto va = as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
    auto vb = as.LoadUniform(ir::Uniform{48, ValueType::V128}).SetType(ValueType::V128);
    auto vc = as.VecAdd(va, vb, ir::Imm{u64{32}}).SetType(ValueType::V128);
    as.StoreUniform(ir::Uniform{64, ValueType::V128}, vc);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{block.get()};
    StateStorage actual, expected;
    for (unsigned offset = 0; offset < 64; offset += 8) {
        actual.Put(offset, 0xabcdef8765432100ULL + offset);
        expected.Put(offset, actual.Get(offset));
    }
    backend::interp::Interpreter reference{*expected.state, block.get()};
    Check(reference.Run() == compiled.fn(actual.state), "mixed helper halt");
    Check(std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 80) == 0,
          "native values survive host helper and vector operations");
    Check(actual.Get(16) == ((actual.Get(0) + actual.Get(8)) ^ Helper(actual.Get(0), actual.Get(8), actual.Get(0) + actual.Get(8))),
          "independent host helper result");
    Check(compiled.translator.Stats().helpers == 0 && compiled.translator.Stats().direct >= 10,
          "mixed fixture uses native values and a direct C-ABI call");
#if defined(__riscv) && __riscv_xlen == 64
    actual.state->halt_reason = HaltReason::None;
    Check(SwiftRiscvCheckABI(compiled.fn, actual.state) == 1, "helper C ABI preserved");
#endif
    std::cout << "PASS mixed scalar, host helper and V128 execution\n";
}

void VectorSelections() {
    using VectorBits = unsigned __int128;
    constexpr VectorBits left = (VectorBits{0x0123456789abcdefULL} << 64) | 0xfedcba9876543210ULL;
    constexpr VectorBits right = (VectorBits{0x8899aabbccddeeffULL} << 64) | 0x0011223344556677ULL;
    for (auto op : {OpCode::Select, OpCode::SelectZero, OpCode::CondSelect}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3400})};
        ir::Assembler as{block.get()};
        auto condition = as.LoadUniform(ir::Uniform{0, ValueType::U8}).SetType(ValueType::U8);
        auto a = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        auto b = as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
        ir::Value result;
        if (op == OpCode::CondSelect) result = as.CondSelect(ir::Cond::EQ, a, b);
        else result = ir::Value{block->AppendInst(op, ir::BOOL{condition}, a, b)};
        as.StoreUniform(ir::Uniform{48, ValueType::V128}, result.SetType(ValueType::V128));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u64 input : {u64{0}, u64{1}}) {
            StateStorage state;
            state.Put(0, input);
            std::memcpy(state.state->uniform_buffer_begin + 16, &left, 16);
            std::memcpy(state.state->uniform_buffer_begin + 32, &right, 16);
            state.state->host_cpu_flags = input ? u64{1} << 30 : 0;
            const auto wanted = (op == OpCode::SelectZero ? input == 0 : input != 0) ? left : right;
            Check(compiled.fn(state.state) == HaltReason::CallHost, "V128 selection halt");
            Check(state.Get(48) == u64(wanted), "V128 selection low half");
            Check(state.Get(56) == u64(wanted >> 64), "V128 selection retains its high half");
        }
    }
    std::cout << "PASS full-width vector selections through semantic helpers\n";
}

u64 ThrowingHelper(u64 fail) {
    if (fail) throw std::runtime_error("intentional semantic helper exception");
    return 42;
}

void SemanticExceptions() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3500})};
    ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto result = as.CallHost(&ThrowingHelper, input).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{8, ValueType::U64}, result);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{block.get()};
    StateStorage state;
    state.Put(0, 1);
    Check(compiled.fn(state.state) == HaltReason::IllegalCode, "helper exception returns through the generated frame");
    Check(state.state->spill_area[backend::kRiscvRecoveryPcSlot] == 0, "exception clears recovery metadata");
    state.state->halt_reason = HaltReason::None;
    state.Put(0, 0);
    Check(compiled.fn(state.state) == HaltReason::CallHost && state.Get(8) == 42,
          "generated block remains usable after helper exception");
    std::cout << "PASS semantic exceptions stay inside C++ helper frames\n";
}

void ScalarCache() {
    for (auto type : {ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64,
                      ValueType::S8, ValueType::S16, ValueType::S32, ValueType::S64}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3600})};
        ir::Assembler as{block.get()};
        auto value = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        for (u64 i = 0; i < 48; ++i)
            value = as.Add(value, ir::Operand{ir::Imm{i * 17 + 3}}).SetType(type);
        as.StoreUniform(ir::Uniform{8, type}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled cached{block.get()}, uncached{block.get(), false};
        const auto& traffic = cached.context.ValueStats();
        const auto& baseline = uncached.context.ValueStats();
        Check(cached.translator.Stats().frame_size == rv::kLeafSavedFrameSize &&
              cached.translator.Stats().saved_fprs == 0 && cached.translator.Stats().saved_gprs == 2 &&
              uncached.translator.Stats().saved_fprs == 0 && uncached.translator.Stats().saved_gprs == 0,
              "leaf chains save only the two reused value GPRs and no floating registers");
        const auto& emitted = cached.context.GetMasm().GetCodeBuffer();
        for (u32 offset = 0; offset < cached.context.CurrentBufferSize(); offset += 4) {
            u32 encoding{}; std::memcpy(&encoding, emitted.GetOffsetPointer(offset), 4);
            Check((encoding & 0x707f) != 0x3027 && (encoding & 0x707f) != 0x3007,
                  "leaf arithmetic code contains no FSD/FLD save or restore instructions");
        }
        Check(traffic.cache_hits > 40 && traffic.spills == 0 && traffic.loads == 0 && traffic.stores == 0,
              "dead SSA values in a straight arithmetic chain never spill or reload");
        Check(traffic.loads + traffic.stores < baseline.loads + baseline.stores,
              "register cache reduces generated SSA memory operations");
        Check(cached.context.CurrentBufferSize() < uncached.context.CurrentBufferSize(),
              "register cache reduces arithmetic chain code size");
        const auto bits = ir::GetValueSizeByte(type) * 8;
        const auto mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        for (u64 input : {u64{0}, u64{1}, u64{127}, u64{255}, u64{0x8000000080000000ULL}, UINT64_MAX}) {
            StateStorage actual, reference;
            actual.Put(0, input); reference.Put(0, input);
            u64 wanted = input & mask;
            for (u64 i = 0; i < 48; ++i) wanted = (wanted + i * 17 + 3) & mask;
            Check(cached.fn(actual.state) == HaltReason::CallHost, "cached chain halt");
            Check(uncached.fn(reference.state) == HaltReason::CallHost, "uncached chain halt");
            Check(actual.Get(8) == wanted && reference.Get(8) == wanted, "independent width-correct chain result");
        }
#if defined(__riscv) && __riscv_xlen == 64
        StateStorage abi;
        Check(SwiftRiscvCheckABI(cached.fn, abi.state) == 1, "all scalar cache GPRs preserve C ABI");
#endif
        if (type == ValueType::U64)
            std::cout << "METRIC U64 chain bytes " << uncached.context.CurrentBufferSize() << " -> "
                      << cached.context.CurrentBufferSize() << ", SSA loads " << baseline.loads << " -> "
                      << traffic.loads << ", stores " << baseline.stores << " -> " << traffic.stores << '\n';
    }
    std::cout << "PASS scalar register cache, eviction, widths and uncached control\n";
}

void RegisterPressureAndHelpers() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3700})};
    ir::Assembler as{block.get()};
    std::array<ir::Value, 20> inputs;
    ir::Value alias;
    for (u32 i = 0; i < inputs.size(); ++i) {
        inputs[i] = as.LoadUniform(ir::Uniform{i * 8, ValueType::U64}).SetType(ValueType::U64);
        if (i == 8)
            alias = as.Add(inputs[8], ir::Operand{inputs[0]}).SetType(ValueType::U64);
    }
    as.StoreUniform(ir::Uniform{168, ValueType::U64}, alias);
    auto total = inputs[0];
    for (u32 i = 1; i < inputs.size(); ++i)
        total = as.Add(total, ir::Operand{inputs[i]}).SetType(ValueType::U64);
    // SaveFlags also reads the producer's original operands through its home.
    as.SaveFlags(total, ir::Flags::Carry | ir::Flags::Zero | ir::Flags::Overflow);
    auto call = as.CallHost(&Helper, inputs[0], inputs[19], total).SetType(ValueType::U64);
    auto result = as.Xor(total, ir::Operand{call}).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{192, ValueType::U64}, result);
    auto quotient = as.Div128(inputs[0], inputs[1], inputs[2], ir::Imm{u64{0}});
    auto remainder = as.Div128Remainder(quotient);
    as.StoreUniform(ir::Uniform{200, ValueType::U64}, quotient);
    as.StoreUniform(ir::Uniform{208, ValueType::U64}, remainder);
    // A scalar -> V128 helper must observe the deferred scalar and a zero high half.
    auto vector = as.BitCast(result).SetType(ValueType::V128);
    as.StoreUniform(ir::Uniform{224, ValueType::V128}, vector);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled cached{block.get()}, uncached{block.get(), false};
    Check(cached.context.ValueStats().spills >= 12 && cached.context.ValueStats().loads > 0,
          "live pressure forces both spills and reloads");
    for (u64 seed : {u64{0}, u64{1}, u64{0xabcdef0123456789ULL}, UINT64_MAX}) {
        StateStorage actual, expected;
        u64 sum{};
        for (u32 i = 0; i < inputs.size(); ++i) {
            const auto value = i == 0 ? u64{1} : i == 2 ? u64{3} : seed + i * 17;
            actual.Put(i * 8, value); expected.Put(i * 8, value);
            sum += value;
        }
        Check(cached.fn(actual.state) == HaltReason::CallHost &&
              uncached.fn(expected.state) == HaltReason::CallHost, "register-pressure helper halt");
        Check(actual.Get(168) == actual.Get(0) + actual.Get(64),
              "destination eviction preserves its right-hand input before overwriting the register");
        const u64 wanted = sum ^ Helper(actual.Get(0), actual.Get(152), sum);
        const auto dividend = (static_cast<unsigned __int128>(actual.Get(0)) << 64) | actual.Get(8);
        Check(actual.Get(192) == wanted, "helper receives all deferred inputs and prior values survive");
        Check(actual.Get(200) == u64(dividend / 3) && actual.Get(208) == u64(dividend % 3),
              "pair-result helper publishes quotient and pseudo remainder");
        Check(actual.Get(224) == wanted && actual.Get(232) == 0, "scalar bitcast preserves zero high half");
        Check(std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 240) == 0 &&
              actual.state->host_cpu_flags == expected.state->host_cpu_flags,
              "cached pressure program matches uncached values and flags");
    }
    std::cout << "PASS live register pressure, semantic homes and pair-result helpers\n";
}

void LocalCacheJoins() {
    for (bool invert : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3800})};
        ir::Assembler as{block.get()};
        auto condition = as.LoadUniform(ir::Uniform{0, ValueType::U8}).SetType(ValueType::U8);
        auto anchor = as.LoadUniform(ir::Uniform{8, ValueType::U64}).SetType(ValueType::U64);
        auto jump = invert ? as.NotGoto(ir::BOOL{condition}) : as.Goto(ir::BOOL{condition});
        ir::Value skipped;
        for (u64 i = 0; i < 20; ++i)
            skipped = as.Add(anchor, ir::Operand{ir::Imm{i + 1}}).SetType(ValueType::U64);
        as.BindLabel(jump);
        auto sum = as.Add(anchor, ir::Operand{skipped}).SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{16, ValueType::U64}, sum);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        Check(compiled.context.ValueStats().initializations == 1,
              "only the non-dominating value is initialized at a join");
        for (u64 input : {u64{0}, u64{1}}) {
            StateStorage state;
            state.Put(0, input); state.Put(8, 71);
            Check(compiled.fn(state.state) == HaltReason::CallHost, "cache join halt");
            const bool taken = invert ? input == 0 : input != 0;
            Check(state.Get(16) == (taken ? 71 : 162), "join reloads the incoming path's canonical homes");
        }
    }
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3900})};
    ir::Assembler as{block.get()};
    auto counter = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto accumulated = as.LoadUniform(ir::Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    auto sum = as.Add(accumulated, ir::Operand{counter}).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{8, ValueType::U64}, sum);
    auto next = as.Sub(counter, ir::Operand{ir::Imm{u64{1}}}).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{0, ValueType::U64}, next);
    auto running = as.TestNotZero(next);
    auto jump = as.Goto(running);
    as.BindLabel(jump);
    auto* label = &block->GetInstList().back();
    block->RemoveInst(label);
    block->InsertBefore(label, counter.Def());
    as.StoreUniform(ir::Uniform{16, ValueType::U64}, sum);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    block->ReIdInstr();
    Compiled compiled{block.get()};
    for (u64 count : {u64{1}, u64{2}, u64{9}, u64{31}}) {
        StateStorage state;
        state.Put(0, count); state.Put(8, 17);
        Check(compiled.fn(state.state) == HaltReason::CallHost, "finite backward cache loop halt");
        Check(state.Get(0) == 0 && state.Get(8) == 17 + count * (count + 1) / 2 &&
              state.Get(16) == state.Get(8), "backedge updates and fallthrough use the final iteration's values");
    }
    std::cout << "PASS forward joins, NotGoto and finite backward loops with cached values\n";
}

void ScalarAddressing() {
    using namespace biscuit;
    auto config = TestConfig();
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    for (u32 size : {1u, 2u, 4u, 8u})
    for (s64 offset : {-2056, -2048, -8, -1, 0, 2040, 2048, 2047}) {
        if (offset % size) continue;
        rv::JitContext context;
        context.EnsureSpace();
        context.Load(a2, a0, offset, size);
        context.Store(a1, a0, offset, size);
        context.GetMasm().MV(a0, a2);
        context.GetMasm().RET();
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate scalar addressing fixture");
        context.Flush(*buffer);
        const auto fn = reinterpret_cast<u64 (*)(u8*, u64)>(buffer->exec_data);
        alignas(8) std::array<u8, 8192> memory;
        memory.fill(0xa5);
        auto expected = memory;
        constexpr u64 replacement = 0xfedcba9876543210ULL;
        std::memcpy(expected.data() + 4096 + offset, &replacement, size);
        u64 wanted{};
        std::memcpy(&wanted, memory.data() + 4096 + offset, size);
        Check(fn(memory.data() + 4096, replacement) == wanted, "signed offset load is zero extended");
        Check(memory == expected, "signed offset store preserves all neighboring bytes");
        if (offset >= -2048 && offset <= 2047)
            Check(context.CurrentBufferSize() == 16, "encodable offsets need no address instructions");
    }
    std::cout << "PASS immediate offset boundaries, large offsets and adjacent memory\n";
}

void LargeBranches() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4000})};
    ir::Assembler as{block.get()};
    auto condition = as.LoadUniform(ir::Uniform{0, ValueType::U8}).SetType(ValueType::U8);
    auto jump = as.Goto(ir::BOOL{condition});
    for (unsigned i = 0; i < 2000; ++i) {
        auto value = as.LoadImm(ir::Imm{u64(i)}).SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{3072, ValueType::U64}, value);
    }
    as.BindLabel(jump);
    auto high = as.LoadImm(ir::Imm{u64{0x8000000080000000ULL}}).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{16, ValueType::U64}, high);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{block.get()};
    Check(compiled.context.CurrentBufferSize() > 4096, "test reaches long branches and buffer growth");
    for (u64 input : {u64{0}, u64{1}}) {
        StateStorage actual;
        actual.Put(0, input);
        Check(compiled.fn(actual.state) == HaltReason::CallHost, "large block halt");
        Check(actual.Get(16) == 0x8000000080000000ULL, "large stack offset value");
        Check(actual.Get(3072) == (input ? 0 : 1999), "far goto execution");
    }
    std::cout << "PASS large code buffers, stack offsets and distant Goto\n";
}

void Memory() {
    for (auto type : {ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64})
    for (bool ordered : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4500})};
        ir::Assembler as{block.get()};
        auto address = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        auto value = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
        if (ordered) as.StoreMemoryTSO(ir::Operand{address}, value);
        else as.StoreMemory(ir::Operand{address}, value);
        auto loaded = ordered ? as.LoadMemoryTSO(ir::Operand{address}) : as.LoadMemory(ir::Operand{address});
        loaded = loaded.SetType(type);
        as.StoreUniform(ir::Uniform{16, type}, loaded);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u64 address : {u64{0}, u64{1}, u64{7}, u64{31}, u64{63}, UINT64_MAX}) {
            StateStorage actual, expected;
            std::array<u8, 64> actual_memory, expected_memory;
            actual_memory.fill(0xa5); expected_memory.fill(0xa5);
            actual.state->pt = actual_memory.data(); expected.state->pt = expected_memory.data();
            actual.state->guest_addr_limit = expected.state->guest_addr_limit = 64;
            actual.state->guest_addr_mask = expected.state->guest_addr_mask = 63;
            actual.Put(0, address); expected.Put(0, address);
            actual.Put(8, 0x123456789abcdef0ULL); expected.Put(8, actual.Get(8));
            backend::interp::Interpreter reference{*expected.state, block.get()};
            Check(reference.Run() == compiled.fn(actual.state), "memory halt");
            Check(actual_memory == expected_memory, "memory bytes and neighbors");
            Check(actual.Get(16) == expected.Get(16), "memory scalar result");
        }
    }
    std::cout << "PASS bounded, unaligned and ordered guest memory\n";
}

void AlignedMemoryAtomicity() {
    constexpr u64 pattern_a = 0xaaaaaaaa55555555ULL;
    constexpr u64 pattern_b = 0x55555555aaaaaaaaULL;
    for (bool ordered : {false, true}) {
        IntrusivePtr<ir::Block> writer_ir{new ir::Block(ir::Location{0x4600})};
        ir::Assembler writer_as{writer_ir.get()};
        auto address = writer_as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        auto value = writer_as.LoadUniform(ir::Uniform{8, ValueType::U64}).SetType(ValueType::U64);
        if (ordered) writer_as.StoreMemoryTSO(ir::Operand{address}, value);
        else writer_as.StoreMemory(ir::Operand{address}, value);
        writer_ir->SetTerminal(ir::terminal::ReturnToHost{});
        IntrusivePtr<ir::Block> reader_ir{new ir::Block(ir::Location{0x4700})};
        ir::Assembler reader_as{reader_ir.get()};
        auto read_address = reader_as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        auto read_value = ordered ? reader_as.LoadMemoryTSO(ir::Operand{read_address})
                                  : reader_as.LoadMemory(ir::Operand{read_address});
        reader_as.StoreUniform(ir::Uniform{8, ValueType::U64}, read_value.SetType(ValueType::U64));
        reader_ir->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled writer{writer_ir.get()}, reader{reader_ir.get()};
        alignas(8) u64 memory = pattern_a;
        std::atomic<bool> start{false};
        StateStorage write_state, read_state;
        write_state.Put(0, reinterpret_cast<u64>(&memory));
        read_state.Put(0, reinterpret_cast<u64>(&memory));
        std::thread writes{[&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned i = 0; i < 5000; ++i) {
                write_state.state->halt_reason = HaltReason::None;
                write_state.Put(8, i & 1 ? pattern_a : pattern_b);
                writer.fn(write_state.state);
            }
        }};
        start.store(true, std::memory_order_release);
        bool intact = true;
        for (unsigned i = 0; i < 5000; ++i) {
            read_state.state->halt_reason = HaltReason::None;
            const auto reason = reader.fn(read_state.state);
            const auto observed = read_state.Get(8);
            intact &= reason == HaltReason::CallHost && (observed == pattern_a || observed == pattern_b);
        }
        writes.join();
        Check(intact, "aligned native scalar loads/stores must not tear under concurrent execution");
    }
    std::cout << "PASS aligned scalar atomicity with concurrent native accesses\n";
}

void RuntimeFaults() {
    auto config = TestConfig();
    backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4800})};
    ir::Assembler as{block.get()};
    auto address = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto loaded = as.LoadMemory(ir::Operand{address}).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{8, ValueType::U64}, loaded);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(block.get()), "publish fault block");
    Check(backend::TranslateIR(module, block) != nullptr, "compile fault block");
    auto* inaccessible = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(inaccessible != MAP_FAILED, "allocate inaccessible guest address");
    Runtime runtime{&space};
    const auto pointer = reinterpret_cast<u64>(inaccessible);
    std::memcpy(runtime.GetUniformBuffer().data(), &pointer, 8);
    runtime.SetLocation(0x4800);
    Check(runtime.Run() == HaltReason::PageFatal, "RV64 native fault unwinds block and dispatcher frames");
    Check(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0,
          "fault recovery clears borrowed frame");
    runtime.SetLocation(0xdead);
    Check(runtime.Run() == HaltReason::CodeMiss, "runtime remains usable after fault");
    munmap(inaccessible, 4096);
    // Also fault in the C++ semantic path, where the interrupted SP/s0/s1
    // no longer describe the generated block's own frame.
    IntrusivePtr<ir::Block> vector_block{new ir::Block(ir::Location{0x4900})};
    ir::Assembler vector_as{vector_block.get()};
    auto vector_address = vector_as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto vector = vector_as.LoadMemoryTSO(ir::Operand{vector_address}).SetType(ValueType::V128);
    vector_as.StoreUniform(ir::Uniform{16, ValueType::V128}, vector);
    vector_block->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(vector_block.get()), "publish semantic fault block");
    Check(backend::TranslateIR(module, vector_block) != nullptr, "compile semantic fault block");
    inaccessible = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    const auto vector_pointer = reinterpret_cast<u64>(inaccessible);
    std::memcpy(runtime.GetUniformBuffer().data(), &vector_pointer, 8);
    runtime.SetLocation(0x4900);
    Check(runtime.Run() == HaltReason::PageFatal, "RV64 semantic helper fault recovers the generated frame");
    munmap(inaccessible, 4096);
    IntrusivePtr<ir::Block> atomic_block{new ir::Block(ir::Location{0x4b00})};
    ir::Assembler atomic_as{atomic_block.get()};
    auto atomic_address = atomic_as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto addend = atomic_as.LoadImm(ir::Imm{u64{3}}).SetType(ValueType::U64);
    auto observed = atomic_as.AtomicFetchAdd(atomic_address, addend).SetType(ValueType::U64);
    atomic_as.StoreUniform(ir::Uniform{8, ValueType::U64}, observed);
    atomic_block->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(atomic_block.get()), "publish atomic fault block");
    Check(backend::TranslateIR(module, atomic_block) != nullptr, "compile atomic fault block");
    inaccessible = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    const auto atomic_pointer = reinterpret_cast<u64>(inaccessible) + 1;
    std::memcpy(runtime.GetUniformBuffer().data(), &atomic_pointer, 8);
    runtime.SetLocation(0x4b00);
    Check(runtime.Run() == HaltReason::PageFatal, "misaligned atomic fault recovers");
    Check(backend::unaligned_atomic_lock.load() == 0, "fault releases only its abandoned helper lock");
    alignas(16) std::array<u8, 16> live{};
    const auto live_pointer = reinterpret_cast<u64>(live.data()) + 1;
    std::memcpy(runtime.GetUniformBuffer().data(), &live_pointer, 8);
    runtime.SetLocation(0x4b00);
    Check(runtime.Run() == HaltReason::CallHost, "atomic helper usable after fault");
    u64 updated{}; std::memcpy(&updated, live.data() + 1, 8);
    Check(updated == 3, "atomic helper writes after fault recovery");
    munmap(inaccessible, 4096);
#if defined(__riscv) && __riscv_xlen == 64
    IntrusivePtr<ir::Block> clobber_block{new ir::Block(ir::Location{0x4c00})};
    ir::Assembler clobber_as{clobber_block.get()};
    auto fault_pointer = clobber_as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto flags_value = clobber_as.Zero().SetType(ValueType::U8);
    clobber_as.SaveFlags(flags_value, ir::Flags::All);
    auto fault_call = clobber_as.CallHost(&SwiftRiscvFaultHelper, fault_pointer).SetType(ValueType::U64);
    clobber_as.StoreUniform(ir::Uniform{8, ValueType::U64}, fault_call);
    clobber_block->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(clobber_block.get()), "publish callee-saved fault fixture");
    Check(backend::TranslateIR(module, clobber_block) != nullptr, "compile callee-saved fault fixture");
    inaccessible = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(inaccessible != MAP_FAILED, "allocate callee-saved fault address");
    const auto clobber_pointer = reinterpret_cast<u64>(inaccessible);
    std::memcpy(runtime.GetUniformBuffer().data(), &clobber_pointer, 8);
    runtime.SetLocation(0x4c00);
    const auto initial_flags = (u64{1} << 50) | (u64{1} << 29) | (u64{1} << 26);
    runtime.GetState()->host_cpu_flags = initial_flags;
    abi_runtime = &runtime;
    Check(SwiftRiscvCheckABI(&RunRuntimeForABI, nullptr) == 1 && abi_halt == HaltReason::PageFatal,
          "abandoned helper restores all integer and floating callee-saved registers");
    Check(runtime.GetState()->host_cpu_flags == (initial_flags | (u64{1} << 30)),
          "abandoned helper restores dirty cached guest flags");
    abi_runtime = nullptr;
    munmap(inaccessible, 4096);
#endif
    std::cout << "PASS precise Runtime native memory fault recovery\n";
}

void Atomics() {
    for (auto op : {OpCode::AtomicExchange, OpCode::AtomicFetchAdd, OpCode::CompareAndSwap}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4a00})};
        ir::Assembler as{block.get()};
        auto address = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        auto input = as.LoadUniform(ir::Uniform{8, ValueType::U64}).SetType(ValueType::U64);
        auto expected = as.LoadUniform(ir::Uniform{16, ValueType::U64}).SetType(ValueType::U64);
        ir::Value result;
        if (op == OpCode::CompareAndSwap) result = as.CompareAndSwap(address, expected, input);
        else result = ir::Value{block->AppendInst(op, address, input)};
        result = result.SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{24, ValueType::U64}, result);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u64 initial : {u64{0}, u64{9}, UINT64_MAX}) {
            alignas(16) u64 memory = initial;
            StateStorage state;
            state.Put(0, reinterpret_cast<u64>(&memory));
            state.Put(8, 3); state.Put(16, 9);
            Check(compiled.fn(state.state) == HaltReason::CallHost, "atomic helper halt");
            Check(state.Get(24) == initial, "atomic returns observed value");
            const auto wanted = op == OpCode::AtomicFetchAdd ? initial + 3 :
                    op == OpCode::AtomicExchange ? 3 : initial == 9 ? 3 : initial;
            Check(memory == wanted, "atomic update value");
        }
    }
    std::cout << "PASS atomic exchange, fetch-add and compare-exchange helpers\n";
}

void RuntimeDispatch() {
    auto config = TestConfig();
    backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<ir::Block> first{new ir::Block(ir::Location{0x5000})};
    first->SetTerminal(ir::terminal::LinkBlock{ir::Location{0x6000}});
    IntrusivePtr<ir::Block> last{new ir::Block(ir::Location{0x6000})};
    ir::Assembler as{last.get()};
    auto result = as.LoadImm(ir::Imm{u64{42}}).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{0, ValueType::U64}, result);
    last->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(first.get()) && module->Push(last.get()), "publish runtime blocks");
    Check(backend::TranslateIR(module, first) && backend::TranslateIR(module, last), "runtime selects RV64 compiler");
    Runtime runtime{&space};
    runtime.SetLocation(0x5000);
    Check(runtime.Run() == HaltReason::CallHost, "runtime dispatches multiple RV64 blocks");
    u64 value{}; std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
    Check(value == 42 && runtime.GetLocation() == 0x6000, "runtime output and location");
    Check(runtime.GetState()->halt_reason == HaltReason::None, "runtime consumes halt state");
    runtime.SignalInterrupt();
    Check(runtime.Run() == HaltReason::Signal, "interrupt between Run calls");
    runtime.ClearInterrupt();
    runtime.SetLocation(0x7000);
    Check(runtime.Run() == HaltReason::CodeMiss, "missing runtime entry");
    // Hash(last_key) is the final bucket. Put/Lookup accept that initial
    // bucket even though collision probing stops before the table's end.
    const auto last_key = ((u64{1} << HASH_TABLE_PAGE_BITS) - 1) << 2;
    IntrusivePtr<ir::Block> edge{new ir::Block(ir::Location{last_key})};
    edge->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(edge.get()) && backend::TranslateIR(module, edge), "publish final L2 bucket");
    last->SetTerminal(ir::terminal::LinkBlock{ir::Location{last_key}});
    space.PushCodeCache(ir::Location{0x6000}, nullptr);
    auto* retired = module->DetachNode(last.get());
    module->ReclaimCode(retired);
    Check(module->Push(last.get()) && backend::TranslateIR(module, last), "compile edge dispatch link");
    runtime.SetLocation(0x5000);
    Check(runtime.Run() == HaltReason::CallHost && runtime.GetLocation() == last_key,
          "runtime dispatch probes the final L2 bucket");
    std::cout << "PASS Runtime dispatch, host return, interruption and code miss\n";
}

void RunningInterrupt() {
    auto config = TestConfig();
    backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7200})};
    ir::Assembler as{block.get()};
    auto first = as.LoadImm(ir::Imm{u64{1}}).SetType(ValueType::U8);
    auto jump = as.Goto(ir::BOOL{first});
    as.BindLabel(jump);
    auto* label = &block->GetInstList().back();
    block->RemoveInst(label);
    block->InsertBefore(label, first.Def());
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(block.get()), "publish infinite local loop");
    Check(backend::TranslateIR(module, block) != nullptr, "compile backward local loop");
    Runtime runtime{&space};
    runtime.SetLocation(0x7200);
    std::thread interrupter{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        runtime.SignalInterrupt();
    }};
    const auto reason = runtime.Run();
    interrupter.join();
    Check(reason == HaltReason::Signal, "running local loop observes Signal without ARM64 guard ABI");
    runtime.ClearInterrupt();
    runtime.SetLocation(0xdead);
    Check(runtime.Run() == HaltReason::CodeMiss, "reuse runtime after running interruption");
    std::cout << "PASS backward local loop and running interruption\n";
}

void Selections() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7300})};
    ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto yes = as.LoadImm(ir::Imm{u64{0xaaaa}}).SetType(ValueType::U64);
    auto no = as.LoadImm(ir::Imm{u64{0xbbbb}}).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{8, ValueType::U64}, as.Select(ir::BOOL{input}, yes, no).SetType(ValueType::U64));
    as.StoreUniform(ir::Uniform{16, ValueType::U64}, as.SelectZero(input, yes, no).SetType(ValueType::U64));
    as.StoreUniform(ir::Uniform{24, ValueType::U64}, as.CondSelect(ir::Cond::NE, yes, no).SetType(ValueType::U64));
    ir::terminal::Switch choices{input, {
            {ir::Imm{u64{1}}, ir::terminal::LinkBlockFast{ir::Location{0x1111}}},
            {ir::Imm{u64{2}}, ir::terminal::ExternalLinkBlock{ir::Location{0x2222}}}}};
    block->SetTerminal(ir::terminal::If{ir::BOOL{input}, choices,
            ir::terminal::CheckHalt{ir::terminal::LinkBlock{ir::Location{0x3333}}}});
    Compiled compiled{block.get()};
    for (u64 input : {u64{0}, u64{1}, u64{2}, u64{3}}) {
        StateStorage state;
        state.Put(0, input);
        state.state->host_cpu_flags = input & 1 ? 0 : u64{1} << 30;
        Check(compiled.fn(state.state) == HaltReason::None, "selection terminal halt");
        Check(state.Get(8) == (input ? 0xaaaa : 0xbbbb), "select truthiness");
        Check(state.Get(16) == (input ? 0xbbbb : 0xaaaa), "select zero");
        Check(state.Get(24) == (input & 1 ? 0xaaaa : 0xbbbb), "select condition");
        Check(state.state->current_loc.Value() == (input == 0 ? 0x3333 : input == 1 ? 0x1111 : input == 2 ? 0x2222 : 0),
              "nested If/Switch/CheckHalt and link variants");
    }
    std::cout << "PASS scalar selections and nested terminals\n";
}

void X86Integration() {
    // mov eax,19; mov ecx,23; add eax,ecx; mov edi,eax; mov eax,60; syscall.
    const std::array<u8, 21> code{0xb8,19,0,0,0, 0xb9,23,0,0,0, 0x01,0xc8,
                              0x89,0xc7, 0xb8,60,0,0,0, 0x0f,0x05};
    auto* guest = static_cast<u8*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(guest != MAP_FAILED, "allocate guest code");
    std::memcpy(guest + 0x100, code.data(), code.size());
    std::unique_ptr<translator::x86::X86Instance, decltype(&translator::x86::X86Instance::Destroy)>
        instance{translator::x86::X86Instance::Make(guest, 4095), translator::x86::X86Instance::Destroy};
    std::unique_ptr<translator::x86::X86Core, decltype(&translator::x86::X86Core::Destroy)>
        core{translator::x86::X86Core::Make(instance.get()), translator::x86::X86Core::Destroy};
    core->GetContext().rip.qword = 0x100;
    Check(core->Run() == translator::Syscall, "x86 guest reaches syscall on RV64");
    Check(core->GetContext().rdi.qword == 42 && core->GetSyscallNumber() == 60,
          "x86 guest arithmetic and architectural state");
    std::cout << "PASS x86 frontend -> RV64 backend -> syscall\n";
    core.reset(); instance.reset(); munmap(guest, 4096);
}

void Arm64Integration() {
    // add -> cmp #42 -> b.eq skips a failure value, then exit(42).
    // This exercises the frontend's flag-only predicate without SetType.
    const std::array<u32, 8> code{0xd2800260, 0xd28002e1, 0x8b010000, 0xf100a81f,
                                  0x54000040, 0xd2800000, 0xd2800ba8, 0xd4000001};
    auto* guest = static_cast<u8*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(guest != MAP_FAILED, "allocate ARM64 guest code");
    std::memcpy(guest + 0x100, code.data(), sizeof(code));
    std::unique_ptr<translator::arm64::Arm64Instance, decltype(&translator::arm64::Arm64Instance::Destroy)>
        instance{translator::arm64::Arm64Instance::Make(guest, 4095), translator::arm64::Arm64Instance::Destroy};
    std::unique_ptr<translator::arm64::Arm64Core, decltype(&translator::arm64::Arm64Core::Destroy)>
        core{translator::arm64::Arm64Core::Make(instance.get()), translator::arm64::Arm64Core::Destroy};
    core->GetContext().pc = 0x100;
    Check(core->Run() == translator::Syscall, "ARM64 guest reaches syscall on RV64");
    Check(core->GetContext().r[0] == 42 && core->GetSyscallNumber() == 93,
          "ARM64 guest arithmetic and architectural state");
    core.reset(); instance.reset(); munmap(guest, 4096);
    std::cout << "PASS ARM64 frontend -> RV64 backend -> syscall\n";
}

void RetainedIR() {
    auto config = TestConfig();
    backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7500})};
    ir::Assembler as{block.get()};
    auto value = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto call = as.CallHost(&Helper, value, value, value).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{8, ValueType::U64}, call);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(block.get()), "publish retained IR fixture");
    auto fn = reinterpret_cast<BlockFn>(backend::TranslateIR(module, block));
    Check(fn != nullptr, "compile retained IR fixture");
    auto* allocation = module->DetachNode(block.get());
    block.reset();
    // Deliberately exercise a retired allocation before its simulated grace
    // period ends: its embedded instruction pointers must still be valid.
    StateStorage state;
    state.Put(0, 37);
    Check(fn(state.state) == HaltReason::CallHost && state.Get(8) == Helper(37,37,37),
          "IR survives node detach until code reclamation");
    module->ReclaimCode(allocation);
    std::cout << "PASS semantic IR ownership through code retirement\n";
}

void FunctionEntries() {
    auto config = TestConfig();
    config.global_opts = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove;
    backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    void* entry{};
    {
        ir::HIRBuilder builder{1, true};
        auto* function = builder.AppendFunction(ir::Location{0x8000});
        (void)function->LoadImm(ir::Imm{u64{777}}).SetType(ValueType::U64); // Dead definition.
        auto input = function->LoadUniform<ir::U64>(ir::Uniform{0, ValueType::U64});
        auto result = function->Add<ir::U64>(input, ir::Operand{ir::Imm{u64{5}}});
        function->StoreUniform(ir::Uniform{8, ValueType::U64}, result);
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0x8100}});
        builder.SetCurBlock(ir::Location{0x8100});
        (void)function->LoadImm(ir::Imm{u64{888}}).SetType(ValueType::U64);
        auto value = function->LoadUniform<ir::U64>(ir::Uniform{8, ValueType::U64});
        auto call = function->CallLambda<ir::U64>(
                ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, value, value, value);
        function->StoreUniform(ir::Uniform{16, ValueType::U64}, call);
        function->EndBlock(ir::terminal::ReturnToHost{});
        function->EndFunction();
        entry = backend::TranslateIR(module, function);
        Check(entry != nullptr, "compile canonical HIR function");
    } // Release all HIR wrappers before executing embedded semantic pointers.
    auto* interior = space.GetCodeCache(ir::Location{0x8100});
    Check(interior && interior != entry, "function interior resolves its own canonical entry");
    Runtime runtime{&space};
    const u64 input = 37;
    std::memcpy(runtime.GetUniformBuffer().data(), &input, 8);
    runtime.SetLocation(0x8000);
    Check(runtime.Run() == HaltReason::CallHost, "multi-block function execution");
    u64 output{};
    std::memcpy(&output, runtime.GetUniformBuffer().data() + 16, 8);
    Check(output == Helper(42, 42, 42), "optimized function survives HIR pool release");
    const u64 interior_input = 19;
    std::memcpy(runtime.GetUniformBuffer().data() + 8, &interior_input, 8);
    runtime.SetLocation(0x8100);
    Check(runtime.Run() == HaltReason::CallHost, "enter function at interior block");
    std::memcpy(&output, runtime.GetUniformBuffer().data() + 16, 8);
    Check(output == Helper(19, 19, 19), "interior entry does not replay function root");
    std::cout << "PASS optimized HIR lifetime and interior function entries\n";
}

void RejectCrossBlockTerminals() {
    auto config = TestConfig();
    backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    IntrusivePtr<ir::Block> source{new ir::Block(ir::Location{0x8200})};
    ir::Assembler source_as{source.get()};
    auto foreign = source_as.LoadImm<ir::U8>(ir::Imm{u8{1}});
    const std::array<ir::Terminal, 3> terminals{
        ir::terminal::If{ir::BOOL{foreign}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}},
        ir::terminal::CheckHalt{ir::terminal::Condition{ir::Cond::EQ,
            ir::terminal::If{ir::BOOL{foreign}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}},
            ir::terminal::ReturnToHost{}}},
        ir::terminal::Switch{foreign, {{ir::Imm{u8{1}}, ir::terminal::ReturnToHost{}}}}};
    for (const auto& terminal : terminals) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x8300})};
        block->SetTerminal(terminal);
        bool rejected{};
        try { (void)backend::TranslateIR(module, block); }
        catch (const std::runtime_error& error) {
            rejected = std::string{error.what()}.find("cross-block SSA") != std::string::npos;
        }
        Check(rejected && space.GetCodeCache(block->GetStartLocation()) == nullptr,
              "cross-block terminal SSA is rejected before publishing code");
    }
    std::cout << "PASS cross-block SSA rejection in nested terminals\n";
}

void CodeReuse() {
    auto config = TestConfig();
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    u8* previous{};
    for (u64 generation = 0; generation < 8; ++generation) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x9000})};
        ir::Assembler as{block.get()};
        auto value = as.LoadImm(ir::Imm{generation + 19}).SetType(ValueType::U64);
        auto call = as.CallHost(&Helper, value, value, value).SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{0, ValueType::U64}, call);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        rv::JitContext context;
        rv::JitTranslator translator{context};
        translator.Translate(block.get());
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate reused code");
        if (previous) Check(buffer->exec_data == previous, "exercise the same RX allocation");
        context.Flush(*buffer);
        StateStorage state;
        auto fn = reinterpret_cast<BlockFn>(buffer->exec_data);
        Check(fn(state.state) == HaltReason::CallHost &&
              state.Get(0) == Helper(generation + 19, generation + 19, generation + 19),
              "reused dual-map code must execute new bytes (QEMU must honor flush_icache)");
        previous = buffer->exec_data;
        cache.FreeCode(buffer->exec_data);
    }
    std::cout << "PASS dual-map code reuse and instruction-cache synchronization\n";
}

}  // namespace

int main(int argc, char** argv) {
#if !defined(__riscv) || __riscv_xlen != 64
    std::cerr << "swift_riscv_backend_test requires an RV64 process (native or QEMU)\n";
    return 1;
#else
    try {
        if (argc == 2 && std::string{argv[1]} == "--cache-reuse") {
            CodeReuse();
            std::cout << "OK " << checks << " checks\n";
            return 0;
        }
        if (argc == 2 && std::string{argv[1]} == "--zbb") {
            NativeScalarBits(true);
            NativeScalarALU(true);
            std::cout << "OK " << checks << " checks\n";
            return 0;
        }
        if (argc == 2 && std::string{argv[1]} == "--rvv") {
            NativeVectors(true); NativeVectorInteger(true); NativeVectorShuffle(true); NativeLocals(true); NativeVectorFloat(true); NativeCalls(true); NativeMemoryCopy(true); NativeCrypto(true);
            std::cout << "OK " << checks << " checks\n";
            return 0;
        }
        if (argc == 2 && (std::string{argv[1]} == "--fp" || std::string{argv[1]} == "--rvv-fp")) {
            NativeVectorFloat(std::string{argv[1]} == "--rvv-fp");
            std::cout << "OK " << checks << " checks\n"; return 0;
        }
        if (argc == 2 && (std::string{argv[1]} == "--calls" || std::string{argv[1]} == "--rvv-calls")) {
            NativeCalls(std::string{argv[1]} == "--rvv-calls");
            std::cout << "PASS " << checks << " checks\n";
            return 0;
        }
        if (argc == 2 && (std::string{argv[1]} == "--crypto" || std::string{argv[1]} == "--scalar-crypto" || std::string{argv[1]} == "--vector-crypto")) {
            const auto mode = std::string{argv[1]};
            NativeCrypto(mode == "--vector-crypto", mode == "--scalar-crypto", mode == "--vector-crypto");
            std::cout << "OK " << checks << " checks\n"; return 0;
        }
        if (argc == 2 && (std::string{argv[1]} == "--memcopy" || std::string{argv[1]} == "--rvv-memcopy" || std::string{argv[1]} == "--zacas")) {
            NativeMemoryCopy(std::string{argv[1]} == "--rvv-memcopy", std::string{argv[1]} == "--zacas");
            std::cout << "OK " << checks << " checks\n"; return 0;
        }
        NativeScalarBits(); NativeScalarALU(); NativeFlags(); NativeVectors(); NativeVectorInteger(); NativeVectorShuffle(); NativeLocals(); NativeVectorFloat(); NativeCalls(); NativeMemoryCopy(); NativeCrypto();
        NativeAtomics();
        ScalarDifferential(); Conditions(); FlagPredicates(); MixedHelpers(); VectorSelections(); SemanticExceptions();
        ScalarCache(); RegisterPressureAndHelpers(); LocalCacheJoins(); ScalarAddressing();
        LargeBranches(); Memory(); AlignedMemoryAtomicity();
        RuntimeDispatch(); RunningInterrupt(); Selections(); RuntimeFaults(); Atomics();
        RetainedIR(); FunctionEntries(); RejectCrossBlockTerminals();
        X86Integration(); Arm64Integration();
        Check(checks > 70000, "backend execution coverage is nonempty");
        std::cout << "OK " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
#endif
}
