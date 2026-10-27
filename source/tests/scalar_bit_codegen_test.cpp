#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "runtime/backend/arm64/compile/compiler.h"

#include <catch2/catch_test_macros.hpp>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/builder/ir_builder.h"
#include "translator/x86/translator.h"

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

IntrusivePtr<Block> BitOperations(u32 width, bool keep_inputs) {
    IntrusivePtr<Block> block{new Block(0, Location{0xb200})};
    std::memset(&block->GetJitCache(), 0, sizeof(JitCache));
    const auto data = block->LoadUniform(Uniform{0, ValueType::U64});
    const auto acc = block->LoadUniform(Uniform{8, ValueType::U64});
    const auto zero = block->LoadImm(Imm{u64{0}});
    const auto negative = block->Sub(zero, Operand{Imm{u64{1}}});
    block->SaveFlags(negative, Flags::NZCV);
    const auto count = block->PopCount(data);
    block->StoreUniform(Uniform{16, ValueType::U64}, count);
    const auto crc = block->Crc32c(acc, data, Imm{width});
    block->StoreUniform(Uniform{24, ValueType::U32}, crc);
    block->StoreUniform(Uniform{32, ValueType::U64},
                        block->TestFlags(ValueType::U64, Flags::Negate)
                                .SetCastType(ValueType::U64));
    if (keep_inputs) {
        block->StoreUniform(Uniform{48, ValueType::U64}, data);
        block->StoreUniform(Uniform{56, ValueType::U64}, acc);
    }
    block->SetTerminal(terminal::LinkBlock{Location{0xb201}});
    block->ReIdInstr();
    return block;
}

u32 ReferenceCrc(u32 crc, u64 data, u32 width) {
    for (u32 byte = 0; byte < width / 8; ++byte) {
        crc ^= u8(data);
        data >>= 8;
        for (u32 bit = 0; bit < 8; ++bit) {
            const bool low = crc & 1;
            crc >>= 1;
            if (low) crc ^= 0x82f63b78;
        }
    }
    return crc;
}

u64 ReferenceIntegerShift(OpCode op, u64 raw, u32 source_bits, u32 result_bits, u64 count) {
    const u32 input_bits = std::min(source_bits, result_bits);
    const u32 amount = count & (result_bits - 1);
    const bool arithmetic = op == OpCode::AsrImm || op == OpCode::AsrValue;
    u64 result{};
    for (u32 bit = 0; bit < result_bits; ++bit) {
        u32 source{};
        if (op == OpCode::LslImm || op == OpCode::LslValue)
            source = bit >= amount ? bit - amount : result_bits;
        else if (op == OpCode::RorImm || op == OpCode::RorValue)
            source = (bit + amount) % result_bits;
        else source = arithmetic ? std::min(bit + amount, input_bits - 1) : bit + amount;
        if (source < input_bits && ((raw >> source) & 1)) result |= u64{1} << bit;
    }
    return result;
}

Config BitConfig(bool hardware) {
    return Config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = 64,
            .static_program = true,
            .arm64_features = hardware ? Arm64Features::CRC32 : Arm64Features::None,
    };
}
}  // namespace

TEST_CASE("scalar bit operations emit without host helpers", "[scalar-bits][codegen]") {
    for (const bool hardware : {false, true}) {
        CAPTURE(hardware);
        auto config = BitConfig(hardware);
        AddressSpace address_space{config};
        auto block = BitOperations(64, true);
        FeatureSet features{};
        AllocationFixture alloc{block->MaxInstrId(),
                                address_space.GetTrampolines().GetGPRRegs(),
                                address_space.GetTrampolines().GetFPRRegs(),
                                features};
        ::swift::runtime::compile::AllocateBlock(
                block.get(),
                &alloc,
                ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                        block.get(), alloc, features),
                false);
        ::swift::runtime::backend::arm64::CompleteARM64Allocation(address_space.GetDefaultModule(),
                                                                  alloc);
        arm64::JitContext context{address_space.GetDefaultModule(), alloc, alloc.Actions()};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        auto& masm = context.GetMasm();
        auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
        auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        std::string output;
        for (auto* instruction = first; instruction < last;
             instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction);
            output += disassembler.GetOutput();
            output += '\n';
        }
        INFO(output);
        REQUIRE(output.find("cnt v") != std::string::npos);
        REQUIRE(output.find("addv b") != std::string::npos);
        REQUIRE(output.find("blr ") == std::string::npos);
        REQUIRE((output.find("crc32cx ") != std::string::npos) == hardware);
    }
}

TEST_CASE("scalar bit operations preserve inputs and pending NZCV", "[scalar-bits][production]") {
#if defined(__aarch64__)
    using Instance = swift::translator::x86::X86Instance;
    std::unique_ptr<Instance, decltype(&Instance::Destroy)> instance{Instance::Make(),
                                                                  Instance::Destroy};
    const bool has_crc = True(instance->GetAddressSpace()->GetConfig().arm64_features &
                              Arm64Features::CRC32);
    for (const bool hardware : {false, true}) {
        if (hardware && !has_crc) continue;
        for (const bool keep_inputs : {false, true}) {
            for (const u32 width : {8u, 16u, 32u, 64u}) {
                CAPTURE(hardware, keep_inputs, width);
                auto config = BitConfig(hardware);
                AddressSpace address_space{config};
                auto block = BitOperations(width, keep_inputs);
                const auto entry = TranslateIR(address_space.GetDefaultModule(), block);
                REQUIRE(entry != nullptr);
                address_space.PushCodeCache(0xb200, entry);
                for (const u64 data : {u64{0}, UINT64_MAX, u64{0x8000000000000000},
                                       u64{0x0123456789abcdef}}) {
                    for (const u64 acc : {u64{0}, UINT64_MAX, u64{0xfedcba9812345678}}) {
                        CAPTURE(data, acc);
                        Runtime runtime{&address_space};
                        std::array<u64, 8> state{data, acc};
                        state[16 / 8] = state[24 / 8] = state[32 / 8] = 0xa5a5a5a5a5a5a5a5;
                        std::memcpy(runtime.GetUniformBuffer().data(), state.data(), sizeof(state));
                        runtime.SetLocation(0xb200);
                        REQUIRE(runtime.Run() == HaltReason::CodeMiss);
                        std::memcpy(state.data(), runtime.GetUniformBuffer().data(), sizeof(state));
                        REQUIRE(state[16 / 8] == std::popcount(data));
                        REQUIRE(u32(state[24 / 8]) == ReferenceCrc(u32(acc), data, width));
                        REQUIRE(state[24 / 8] >> 32 == 0xa5a5a5a5);
                        REQUIRE(state[32 / 8] == 1);
                        if (keep_inputs) {
                            REQUIRE(state[48 / 8] == data);
                            REQUIRE(state[56 / 8] == acc);
                        }
                    }
                }
            }
        }
    }
    // Disable folding so all three zero-width emitter paths execute. Wide
    // stores also expose stale high bits in the 16-bit byte-swap result.
    for (const auto op : {OpCode::BitExtract, OpCode::BitInsert, OpCode::BitClear,
                          OpCode::ByteSwap, OpCode::CountLeadingZeros32,
                          OpCode::CountTrailingZeros32, OpCode::CountLeadingZeros64,
                          OpCode::CountTrailingZeros64}) {
        CAPTURE(static_cast<u32>(op));
        auto config = BitConfig(false);
        config.global_opts = Optimizations::None;
        AddressSpace address_space{config};
        IntrusivePtr<Block> block{new Block{0, Location{0xb200}}};
        const bool word = op == OpCode::CountLeadingZeros32 || op == OpCode::CountTrailingZeros32;
        const auto source = block->LoadUniform(Uniform{0, word ? ValueType::S32 : ValueType::S64});
        const auto other = block->LoadUniform(Uniform{8, ValueType::U64});
        Value result;
        if (op == OpCode::BitInsert)
            result = block->BitInsert(ValueType::U64, source, other, Imm{37u}, Imm{0u});
        else if (op == OpCode::BitExtract || op == OpCode::BitClear)
            result = Value{block->AppendInst(op, ValueType::U64, source, Imm{37u}, Imm{0u})};
        else if (op == OpCode::ByteSwap)
            result = block->ByteSwap(ValueType::U64, source, Imm{16u});
        else result = Value{block->AppendInst(op, ValueType::U64, source)};
        block->StoreUniform(Uniform{16, ValueType::U64}, result);
        block->SetTerminal(terminal::LinkBlock{Location{0xb201}});
        block->ReIdInstr();
        const std::array inputs{u64{0}, u64{1}, u64{0x8000000080000000},
                                u64{0x0123456789abcdef}, UINT64_MAX};
        const auto expected_for = [&](u64 input) {
            u64 expected = input;
            if (op == OpCode::BitExtract) expected = 0;
            if (op == OpCode::ByteSwap)
                expected = ((input & 0xff) << 8) | ((input >> 8) & 0xff);
            if (op == OpCode::CountLeadingZeros32) expected = std::countl_zero(u32(input));
            if (op == OpCode::CountTrailingZeros32) expected = std::countr_zero(u32(input));
            if (op == OpCode::CountLeadingZeros64) expected = std::countl_zero(input);
            if (op == OpCode::CountTrailingZeros64) expected = std::countr_zero(input);
            return expected;
        };
        // TranslateIR may release every instruction. Consume the portable
        // reference first, while this block still owns its IR.
        for (const auto input : inputs) {
            std::array<u64, 8> values{input, ~input, UINT64_MAX};
            alignas(State) std::array<u8, sizeof(State) + sizeof(values)> bytes{};
            auto* state = new (bytes.data()) State{};
            std::memcpy(state->uniform_buffer_begin, values.data(), sizeof(values));
            interp::Interpreter interpreter{*state, block.get()};
            REQUIRE(interpreter.Run() == HaltReason::None);
            u64 interpreted{};
            std::memcpy(&interpreted, state->uniform_buffer_begin + 16, sizeof(interpreted));
            REQUIRE(interpreted == expected_for(input));
        }
        const auto entry = TranslateIR(address_space.GetDefaultModule(), block);
        REQUIRE(entry);
        address_space.PushCodeCache(0xb200, entry);
        for (const auto input : inputs) {
            std::array<u64, 8> values{input, ~input, UINT64_MAX};
            Runtime runtime{&address_space};
            std::memcpy(runtime.GetUniformBuffer().data(), values.data(), sizeof(values));
            runtime.SetLocation(0xb200);
            REQUIRE(runtime.Run() == HaltReason::CodeMiss);
            std::memcpy(values.data(), runtime.GetUniformBuffer().data(), sizeof(values));
            REQUIRE(values[2] == expected_for(input));
        }
    }
#else
    SUCCEED("Native scalar bit operations require an AArch64 host");
#endif
}

TEST_CASE("scalar register widths preserve signed 64-bit values", "[scalar-bits][types][production]") {
#if defined(__aarch64__)
    for (const bool function_mode : {false, true})
    for (const auto type : {ValueType::S64, ValueType::U64, ValueType::S32, ValueType::U32})
    for (const auto op : {OpCode::Add, OpCode::Sub, OpCode::Mul, OpCode::Not,
                          OpCode::Xor, OpCode::LslImm, OpCode::LsrImm, OpCode::RorImm,
                          OpCode::BitClear, OpCode::LoadImm}) {
        CAPTURE(function_mode, static_cast<u32>(type), static_cast<u32>(op));
        auto config = BitConfig(false);
        config.global_opts = Optimizations::None;
        AddressSpace address_space{config};
        HIRBuilder hir{1, false, FeatureSet{}};
        auto* function = function_mode
                ? hir.AppendFunction(Location{0xb400}, Location{0xb401}) : nullptr;
        IntrusivePtr<Block> block;
        if (!function) block = new Block{0, Location{0xb400}};
        compile::IrBuilder factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{block.get()};
        const auto left = factory.LoadUniform(Uniform{0, type});
        const auto right = factory.LoadUniform(Uniform{8, type});
        const bool wide = GetValueSizeByte(type) == sizeof(u64);
        const u64 mask = wide ? UINT64_MAX : UINT32_MAX;
        const u64 constant = 0xfedcba9876543210ULL & mask;
        Value result;
        if (op == OpCode::Not) result = factory.Not(left);
        else if (op == OpCode::LoadImm) result = factory.LoadImm(type, Imm{constant});
        else if (op == OpCode::LslImm || op == OpCode::LsrImm || op == OpCode::RorImm)
            result = Value{factory.AppendInst(op, type, left, Imm{1u})};
        else if (op == OpCode::BitClear)
            result = factory.BitClear(type, left, Imm{8u}, Imm{8u});
        else if (op == OpCode::Xor)
            result = factory.Xor(type, left, Operand{Imm{constant}});
        else result = Value{factory.AppendInst(op, type, left, Operand{right})};
        factory.StoreUniform(Uniform{16, ValueType::U64}, factory.ZeroExtend64(result));
        auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
        factory.ReturnToHost();
        if (function) { function->ComputeRPO(); function->IdByRPO(); }
        else body->ReIdInstr();
        const std::array<std::pair<u64, u64>, 6> inputs{{
                {0, 0}, {1, 1}, {0x100000000, 1}, {0x8000000080000000, 0x7fffffff00000001},
                {UINT64_MAX, UINT64_MAX}, {0x0123456789abcdef, 0xfedcba9876543210}}};
        const auto expected = [&](u64 a, u64 b) {
            a &= mask; b &= mask;
            u64 value{};
            if (op == OpCode::Add) value = a + b;
            if (op == OpCode::Sub) value = a - b;
            if (op == OpCode::Mul) value = a * b;
            if (op == OpCode::Not) value = a == 0 ? 1 : 0;
            if (op == OpCode::Xor) value = a ^ constant;
            if (op == OpCode::LslImm) value = a << 1;
            if (op == OpCode::LsrImm) value = a >> 1;
            if (op == OpCode::RorImm) value = (a >> 1) | (a << (wide ? 63 : 31));
            if (op == OpCode::BitClear) value = a & ~u64{0xff00};
            if (op == OpCode::LoadImm) value = constant;
            return value & mask;
        };
        // The production compiler can release IR. Consume it first.
        for (const auto [a, b] : inputs) {
            alignas(State) std::array<u8, sizeof(State) + 64> storage{};
            auto* state = new (storage.data()) State{};
            const std::array<u64, 8> values{a, b, UINT64_MAX};
            std::memcpy(state->uniform_buffer_begin, values.data(), sizeof(values));
            interp::Interpreter interpreter{*state, body};
            REQUIRE(interpreter.Run() == HaltReason::CallHost);
            u64 actual{};
            std::memcpy(&actual, state->uniform_buffer_begin + 16, sizeof(actual));
            REQUIRE(actual == expected(a, b));
        }
        const auto entry = function ? TranslateIR(address_space.GetDefaultModule(), function)
                                    : TranslateIR(address_space.GetDefaultModule(), block);
        REQUIRE(entry);
        address_space.PushCodeCache(0xb400, entry);
        for (const auto [a, b] : inputs) {
            CAPTURE(a, b);
            Runtime runtime{&address_space};
            std::array<u64, 8> values{a, b, UINT64_MAX};
            std::memcpy(runtime.GetUniformBuffer().data(), values.data(), sizeof(values));
            runtime.SetLocation(0xb400);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            std::memcpy(values.data(), runtime.GetUniformBuffer().data(), sizeof(values));
            REQUIRE(values[2] == expected(a, b));
        }
    }
#else
    SUCCEED("Native scalar register widths require an AArch64 host");
#endif
}

TEST_CASE("integer conversions observe narrow views of raw values", "[scalar-bits][types][production]") {
#if defined(__aarch64__)
    for (const bool optimize : {false, true})
    for (const bool function_mode : {false, true})
    for (const auto source_type : {ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                   ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64})
    for (const auto op : {OpCode::ZeroExtend32, OpCode::ZeroExtend32To64,
                          OpCode::ZeroExtend64, OpCode::SignExtend})
    for (const auto result_type : {ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                   ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64})
    for (const u32 transport : {0u, 1u, 2u})
    for (const bool keep_input : {false, true}) {
        CAPTURE(optimize, function_mode, static_cast<u32>(source_type), static_cast<u32>(op),
                static_cast<u32>(result_type), transport, keep_input);
        auto config = BitConfig(false);
        config.global_opts = optimize ? Optimizations::All : Optimizations::None;
        AddressSpace address_space{config};
        HIRBuilder hir{1, false, FeatureSet{}};
        auto* function = function_mode
                ? hir.AppendFunction(Location{0xb500}, Location{0xb501}) : nullptr;
        IntrusivePtr<Block> block;
        if (!function) block = new Block{0, Location{0xb500}};
        compile::IrBuilder factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{block.get()};
        const auto raw = factory.LoadUniform(Uniform{0, ValueType::U64});
        auto input = transport ? factory.BitCast(source_type, raw)
                               : Value{raw}.SetCastType(source_type);
        if (transport == 2) input = factory.GetResult(source_type, input, input);
        const auto result = Value{factory.AppendInst(op, result_type, input)};
        factory.StoreUniform(Uniform{16, ValueType::U64}, factory.ZeroExtend64(result));
        // A conversion must not normalize the borrowed raw input in place.
        if (keep_input)
            factory.StoreUniform(Uniform{24, ValueType::U32}, Value{input}.SetCastType(ValueType::U32));
        auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
        factory.ReturnToHost();
        if (function) { function->ComputeRPO(); function->IdByRPO(); }
        else body->ReIdInstr();
        const u32 source_bits = GetValueSizeByte(source_type) * 8;
        const u32 copied = op == OpCode::ZeroExtend32 || op == OpCode::ZeroExtend32To64
                ? std::min(source_bits, 32u) : source_bits;
        const u64 input_mask = copied == 64 ? UINT64_MAX : (u64{1} << copied) - 1;
        const u32 result_bits = GetValueSizeByte(result_type) * 8;
        const u64 result_mask = result_bits == 64 ? UINT64_MAX : (u64{1} << result_bits) - 1;
        const auto expected = [&](u64 value) {
            value &= input_mask;
            if (op == OpCode::SignExtend && copied < 64 && (value & (u64{1} << (copied - 1))))
                value |= ~input_mask;
            return value & result_mask;
        };
        const std::array<u64, 8> inputs{0, 1, 0xdeadbeef00000080ULL, 0xabcdef0080008000ULL,
                0x100000000ULL, 0x8000000080000000ULL, 0xdeadbeef12345678ULL, UINT64_MAX};
        for (const auto value : inputs) {
            CAPTURE(value);
            alignas(State) std::array<u8, sizeof(State) + 64> storage{};
            auto* state = new (storage.data()) State{};
            const std::array<u64, 8> values{value, 0, UINT64_MAX, UINT64_MAX};
            std::memcpy(state->uniform_buffer_begin, values.data(), sizeof(values));
            interp::Interpreter interpreter{*state, body};
            REQUIRE(interpreter.Run() == HaltReason::CallHost);
            u64 actual{};
            u32 raw_after{};
            std::memcpy(&actual, state->uniform_buffer_begin + 16, sizeof(actual));
            std::memcpy(&raw_after, state->uniform_buffer_begin + 24, sizeof(raw_after));
            REQUIRE(actual == expected(value));
            if (keep_input) REQUIRE(raw_after == static_cast<u32>(value));
        }
        const auto entry = function ? TranslateIR(address_space.GetDefaultModule(), function)
                                    : TranslateIR(address_space.GetDefaultModule(), block);
        REQUIRE(entry);
        address_space.PushCodeCache(0xb500, entry);
        for (const auto value : inputs) {
            CAPTURE(value);
            Runtime runtime{&address_space};
            const std::array<u64, 8> values{value, 0, UINT64_MAX, UINT64_MAX};
            std::memcpy(runtime.GetUniformBuffer().data(), values.data(), sizeof(values));
            runtime.SetLocation(0xb500);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            u64 actual{};
            u32 raw_after{};
            std::memcpy(&actual, runtime.GetUniformBuffer().data() + 16, sizeof(actual));
            std::memcpy(&raw_after, runtime.GetUniformBuffer().data() + 24, sizeof(raw_after));
            REQUIRE(actual == expected(value));
            if (keep_input) REQUIRE(raw_after == static_cast<u32>(value));
        }
    }
#else
    SUCCEED("Native integer conversions require an AArch64 host");
#endif
}

TEST_CASE("integer shifts share logical views and masked counts", "[scalar-bits][types][production]") {
#if defined(__aarch64__)
    u64 checked{}, programs{}, current_value{}, current_amount{};
    const auto check = [&](u64 actual, u64 expected, const char* label) {
        ++checked;
        // JUnit retains successful assertions and their captures. Keep the
        // complete oracle matrix without retaining millions of report rows.
        if (actual != expected)
            FAIL(label << ": " << actual << " != " << expected
                       << ", value=" << current_value << ", count=" << current_amount);
    };
    for (const bool optimize : {false, true})
    for (const bool function_mode : {false, true})
    for (const auto source_type : {ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                   ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64})
    for (const auto result_type : {ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                   ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64})
    for (const auto op : {OpCode::LsrImm, OpCode::LslImm, OpCode::AsrImm, OpCode::RorImm,
                          OpCode::LsrValue, OpCode::LslValue, OpCode::AsrValue, OpCode::RorValue})
    for (const u32 transport : {0u, 1u, 2u})
    for (const bool keep_inputs : {false, true}) {
        const u32 source_bits = GetValueSizeByte(source_type) * 8;
        const u32 result_bits = GetValueSizeByte(result_type) * 8;
        const bool immediate = op == OpCode::LsrImm || op == OpCode::LslImm ||
                               op == OpCode::AsrImm || op == OpCode::RorImm;
        const std::array<u64, 7> counts{1, 0, result_bits - 1, result_bits, result_bits + 1,
                                       0x100000001ULL, UINT64_MAX};
        const std::array<u64, 5> values{0x180, 0x8000800080008080ULL, 0, UINT64_MAX,
                                       0x1234567800000001ULL};
        for (u32 variant = 0; variant < (immediate ? counts.size() : 2); ++variant) {
            ++programs;
            const bool shared_count = !immediate && variant == 1;
            CAPTURE(optimize, function_mode, static_cast<u32>(source_type),
                    static_cast<u32>(result_type), static_cast<u32>(op), transport,
                    keep_inputs, variant);
            auto config = BitConfig(false);
            config.global_opts = optimize ? Optimizations::All : Optimizations::None;
            AddressSpace address_space{config};
            HIRBuilder hir{1, false, FeatureSet{}};
            auto* function = function_mode
                    ? hir.AppendFunction(Location{0xb600}, Location{0xb601}) : nullptr;
            IntrusivePtr<Block> block;
            if (!function) block = new Block{0, Location{0xb600}};
            compile::IrBuilder factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{block.get()};
            const auto raw = factory.LoadUniform(Uniform{0, ValueType::U64});
            auto input = transport ? factory.BitCast(source_type, raw)
                                   : Value{raw}.SetCastType(source_type);
            if (transport == 2) input = factory.GetResult(source_type, input, input);
            Value count;
            Value raw_count;
            if (!immediate) {
                raw_count = shared_count ? raw : factory.LoadUniform(Uniform{8, ValueType::U64});
                count = shared_count ? input : Value{raw_count}.SetCastType(source_type);
            }
            const auto output = immediate
                    ? Value{factory.AppendInst(op, result_type, input, Imm{counts[variant]})}
                    : Value{factory.AppendInst(op, result_type, input, count)};
            factory.StoreUniform(Uniform{16, ValueType::U64}, factory.ZeroExtend64(output));
            if (keep_inputs) {
                factory.StoreUniform(Uniform{24, ValueType::U64}, raw);
                if (!immediate) factory.StoreUniform(Uniform{32, ValueType::U64}, raw_count);
            }
            auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
            factory.ReturnToHost();
            if (function) { function->ComputeRPO(); function->IdByRPO(); }
            else body->ReIdInstr();
            const u32 samples = immediate || shared_count ? values.size() : values.size() * counts.size();
            const auto run_samples = [&](auto&& run) {
                for (u32 sample = 0; sample < samples; ++sample) {
                    const u64 value = values[sample % values.size()];
                    const u64 amount = immediate ? counts[variant]
                            : shared_count ? value : counts[sample / values.size()];
                    current_value = value;
                    current_amount = amount;
                    run(value, amount, ReferenceIntegerShift(op, value, source_bits, result_bits, amount));
                }
            };
            // The production compiler consumes IR; run its independent control first.
            run_samples([&](u64 value, u64 amount, u64 expected) {
                alignas(State) std::array<u8, sizeof(State) + 64> storage{};
                auto* state = new (storage.data()) State{};
                const std::array<u64, 8> initial{value, amount, UINT64_MAX, UINT64_MAX, UINT64_MAX};
                std::memcpy(state->uniform_buffer_begin, initial.data(), sizeof(initial));
                interp::Interpreter interpreter{*state, body};
                check(static_cast<u32>(interpreter.Run()), static_cast<u32>(HaltReason::CallHost), "interpreter halt");
                std::array<u64, 8> actual{};
                std::memcpy(actual.data(), state->uniform_buffer_begin, sizeof(actual));
                check(actual[2], expected, "shift value");
                if (keep_inputs) {
                    check(actual[3], value, "raw source");
                    if (!immediate) check(actual[4], amount, "raw count");
                }
            });
            const auto entry = function ? TranslateIR(address_space.GetDefaultModule(), function)
                                        : TranslateIR(address_space.GetDefaultModule(), block);
            check(entry != nullptr, 1, "translation");
            address_space.PushCodeCache(0xb600, entry);
            run_samples([&](u64 value, u64 amount, u64 expected) {
                Runtime runtime{&address_space};
                const std::array<u64, 8> initial{value, amount, UINT64_MAX, UINT64_MAX, UINT64_MAX};
                std::memcpy(runtime.GetUniformBuffer().data(), initial.data(), sizeof(initial));
                runtime.SetLocation(0xb600);
                check(static_cast<u32>(runtime.Run()), static_cast<u32>(HaltReason::CallHost), "JIT halt");
                std::array<u64, 8> actual{};
                std::memcpy(actual.data(), runtime.GetUniformBuffer().data(), sizeof(actual));
                check(actual[2], expected, "shift value");
                if (keep_inputs) {
                    check(actual[3], value, "raw source");
                    if (!immediate) check(actual[4], amount, "raw count");
                }
            });
        }
    }
    REQUIRE(programs == 55296);
    REQUIRE(checked == 2605056);
    std::printf("integer-shift-oracle programs=%llu checks=%llu\n",
                static_cast<unsigned long long>(programs), static_cast<unsigned long long>(checked));
#else
    SUCCEED("Native integer shifts require an AArch64 host");
#endif
}

TEST_CASE("integer predicates read logical views and preserve raw sources",
          "[scalar-bits][types][production]") {
#if defined(__aarch64__)
    u64 programs{}, checked{}, current_value{}, interpreter_value{}, native_value{};
    const auto check = [&](u64 actual, u64 expected, const char* label) {
        ++checked;
        if (actual != expected)
            FAIL(label << ": " << actual << " != " << expected << ", raw=" << current_value
                       << ", interpreter=" << interpreter_value << ", native=" << native_value);
    };
    const std::array<u64, 9> values{0x100, 0, 1, 0x8000, 0x10000, 0x100000000ULL,
                                   0x8000000000000000ULL, UINT64_MAX, 0xfedcba9876543200ULL};
    const std::array<u64, 5> positions{0, 7, 15, 31, 63};
    for (const bool optimize : {false, true})
    for (const bool function_mode : {false, true})
    for (const auto source_type : {ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                   ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64})
    for (const auto result_type : {ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                   ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64})
    for (const auto op : {OpCode::TestZero, OpCode::TestNotZero, OpCode::Not, OpCode::TestBit})
    for (const u32 transport : {0u, 1u, 2u})
    for (const bool keep_input : {false, true}) {
        const u32 source_bits = GetValueSizeByte(source_type) * 8;
        const u64 source_mask = source_bits == 64 ? UINT64_MAX : (u64{1} << source_bits) - 1;
        for (u32 variant = 0; variant < (op == OpCode::TestBit ? positions.size() : 1); ++variant) {
            ++programs;
            CAPTURE(optimize, function_mode, static_cast<u32>(source_type),
                    static_cast<u32>(result_type), static_cast<u32>(op), transport,
                    keep_input, variant);
            auto config = BitConfig(false);
            config.global_opts = optimize ? Optimizations::All : Optimizations::None;
            AddressSpace address_space{config};
            HIRBuilder hir{1, false, FeatureSet{}};
            auto* function = function_mode
                    ? hir.AppendFunction(Location{0xb700}, Location{0xb701}) : nullptr;
            IntrusivePtr<Block> block;
            if (!function) block = new Block{0, Location{0xb700}};
            compile::IrBuilder factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{block.get()};
            const auto raw = factory.LoadUniform(Uniform{0, ValueType::U64});
            auto input = transport ? factory.BitCast(source_type, raw)
                                   : Value{raw}.SetCastType(source_type);
            if (transport == 2) input = factory.GetResult(source_type, input, input);
            const auto output = op == OpCode::TestBit
                    ? Value{factory.AppendInst(op, result_type, input, Imm{positions[variant]})}
                    : Value{factory.AppendInst(op, result_type, input)};
            // Observe the complete materialized Boolean through a raw view.
            // A width conversion could fold an out-of-view bit test before emission.
            factory.StoreUniform(Uniform{16, ValueType::U64}, Value{output}.SetCastType(ValueType::U64));
            if (keep_input) factory.StoreUniform(Uniform{24, ValueType::U64}, raw);
            auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
            factory.ReturnToHost();
            if (function) { function->ComputeRPO(); function->IdByRPO(); }
            else body->ReIdInstr();
            std::array<std::array<u64, 8>, values.size()> interpreted{};
            for (u32 sample = 0; sample < values.size(); ++sample) {
                alignas(State) std::array<u8, sizeof(State) + 64> storage{};
                auto* state = new (storage.data()) State{};
                const std::array<u64, 8> initial{values[sample], 0, UINT64_MAX, UINT64_MAX};
                std::memcpy(state->uniform_buffer_begin, initial.data(), sizeof(initial));
                interp::Interpreter interpreter{*state, body};
                if (interpreter.Run() != HaltReason::CallHost) FAIL("predicate interpreter halt");
                std::memcpy(interpreted[sample].data(), state->uniform_buffer_begin, sizeof(initial));
            }
            const auto entry = function ? TranslateIR(address_space.GetDefaultModule(), function)
                                        : TranslateIR(address_space.GetDefaultModule(), block);
            if (!entry) FAIL("predicate translation");
            address_space.PushCodeCache(0xb700, entry);
            for (u32 sample = 0; sample < values.size(); ++sample) {
                current_value = values[sample];
                const u64 logical = current_value & source_mask;
                const u64 expected = op == OpCode::TestBit
                        ? positions[variant] < source_bits && ((current_value >> positions[variant]) & 1)
                        : op == OpCode::TestNotZero ? logical != 0 : logical == 0;
                Runtime runtime{&address_space};
                const std::array<u64, 8> initial{current_value, 0, UINT64_MAX, UINT64_MAX};
                std::memcpy(runtime.GetUniformBuffer().data(), initial.data(), sizeof(initial));
                runtime.SetLocation(0xb700);
                if (runtime.Run() != HaltReason::CallHost) FAIL("predicate native halt");
                std::array<u64, 8> actual{};
                std::memcpy(actual.data(), runtime.GetUniformBuffer().data(), sizeof(actual));
                interpreter_value = interpreted[sample][2];
                native_value = actual[2];
                check(interpreter_value, expected, "interpreter predicate");
                check(native_value, expected, "native predicate");
                if (keep_input) {
                    check(interpreted[sample][3], current_value, "interpreter raw source");
                    check(actual[3], current_value, "native raw source");
                }
            }
        }
    }
    REQUIRE(programs == 12288);
    REQUIRE(checked == 331776);
    std::printf("integer-predicate-oracle programs=%llu checks=%llu\n",
                static_cast<unsigned long long>(programs), static_cast<unsigned long long>(checked));
#else
    SUCCEED("Native integer predicates require an AArch64 host");
#endif
}

TEST_CASE("integer selections respect condition views and aliased arms",
          "[scalar-bits][types][production]") {
#if defined(__aarch64__)
    u64 programs{}, checked{}, current_value{};
    const auto check = [&](u64 actual, u64 expected, const char* label) {
        ++checked;
        if (actual != expected) FAIL(label << ": " << actual << " != " << expected << ", raw=" << current_value);
    };
    const std::array<u64, 9> values{0x100, 0, 1, 0x8000, 0x10000, 0x100000000ULL,
                                   0x8000000000000000ULL, UINT64_MAX, 0xfedcba9876543200ULL};
    constexpr u64 left_pattern = 0xfedcba9876543210ULL, right_pattern = 0x1234567890abcdefULL;
    for (const bool optimize : {false, true})
    for (const bool function_mode : {false, true})
    for (const auto condition_type : {ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                      ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64})
    for (const auto result_type : {ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                                   ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64})
    for (const auto op : {OpCode::Select, OpCode::SelectZero})
    for (const u32 transport : {0u, 1u, 2u})
    for (const bool keep_inputs : {false, true})
    for (const u32 arms : {0u, 1u, 2u, 3u}) {
        ++programs;
        CAPTURE(optimize, function_mode, static_cast<u32>(condition_type), static_cast<u32>(result_type),
                static_cast<u32>(op), transport, keep_inputs, arms);
        auto config = BitConfig(false);
        config.global_opts = optimize ? Optimizations::All : Optimizations::None;
        AddressSpace address_space{config};
        HIRBuilder hir{1, false, FeatureSet{}};
        auto* function = function_mode
                ? hir.AppendFunction(Location{0xb800}, Location{0xb801}) : nullptr;
        IntrusivePtr<Block> block;
        if (!function) block = new Block{0, Location{0xb800}};
        compile::IrBuilder factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{block.get()};
        const auto raw = factory.LoadUniform(Uniform{0, ValueType::U64});
        auto condition = transport ? factory.BitCast(condition_type, raw)
                                   : Value{raw}.SetCastType(condition_type);
        if (transport == 2) condition = factory.GetResult(condition_type, condition, condition);
        const auto raw_left = arms == 3 ? factory.LoadImm(Imm{u64{1}})
                : arms == 1 ? raw : factory.LoadUniform(Uniform{8, ValueType::U64});
        const auto raw_right = arms == 3 ? factory.LoadImm(Imm{u64{0}})
                : arms == 2 ? raw : factory.LoadUniform(Uniform{40, ValueType::U64});
        if (arms == 3) condition = factory.TestNotZero(condition);
        const auto selected = Value{factory.AppendInst(op, result_type, condition,
                Value{raw_left}.SetCastType(result_type), Value{raw_right}.SetCastType(result_type))};
        factory.StoreUniform(Uniform{16, result_type}, selected);
        if (keep_inputs) {
            factory.StoreUniform(Uniform{24, ValueType::U64}, raw);
            factory.StoreUniform(Uniform{32, ValueType::U64}, raw_left);
            factory.StoreUniform(Uniform{48, ValueType::U64}, raw_right);
        }
        auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
        factory.ReturnToHost();
        if (function) { function->ComputeRPO(); function->IdByRPO(); }
        else body->ReIdInstr();
        std::array<std::array<u64, 8>, values.size()> interpreted{};
        for (u32 sample = 0; sample < values.size(); ++sample) {
            alignas(State) std::array<u8, sizeof(State) + 64> storage{};
            auto* state = new (storage.data()) State{};
            const std::array<u64, 8> initial{values[sample], left_pattern, UINT64_MAX,
                    UINT64_MAX, UINT64_MAX, right_pattern, UINT64_MAX};
            std::memcpy(state->uniform_buffer_begin, initial.data(), sizeof(initial));
            interp::Interpreter interpreter{*state, body};
            if (interpreter.Run() != HaltReason::CallHost) FAIL("selection interpreter halt");
            std::memcpy(interpreted[sample].data(), state->uniform_buffer_begin, sizeof(initial));
        }
        const auto entry = function ? TranslateIR(address_space.GetDefaultModule(), function)
                                    : TranslateIR(address_space.GetDefaultModule(), block);
        if (!entry) FAIL("selection translation");
        address_space.PushCodeCache(0xb800, entry);
        const u32 condition_bits = GetValueSizeByte(condition_type) * 8;
        const u32 result_bits = GetValueSizeByte(result_type) * 8;
        const u64 condition_mask = condition_bits == 64 ? UINT64_MAX : (u64{1} << condition_bits) - 1;
        const u64 result_mask = result_bits == 64 ? UINT64_MAX : (u64{1} << result_bits) - 1;
        for (u32 sample = 0; sample < values.size(); ++sample) {
            current_value = values[sample];
            const u64 left = arms == 3 ? 1 : arms == 1 ? current_value : left_pattern;
            const u64 right = arms == 3 ? 0 : arms == 2 ? current_value : right_pattern;
            const bool nonzero = (current_value & condition_mask) != 0;
            const u64 expected = ((op == OpCode::SelectZero ? !nonzero : nonzero) ? left : right) & result_mask;
            Runtime runtime{&address_space};
            const std::array<u64, 8> initial{current_value, left_pattern, UINT64_MAX,
                    UINT64_MAX, UINT64_MAX, right_pattern, UINT64_MAX};
            std::memcpy(runtime.GetUniformBuffer().data(), initial.data(), sizeof(initial));
            runtime.SetLocation(0xb800);
            if (runtime.Run() != HaltReason::CallHost) FAIL("selection native halt");
            std::array<u64, 8> actual{};
            std::memcpy(actual.data(), runtime.GetUniformBuffer().data(), sizeof(actual));
            check(interpreted[sample][2] & result_mask, expected, "interpreter selection");
            check(actual[2] & result_mask, expected, "native selection");
            if (keep_inputs) {
                check(interpreted[sample][3], current_value, "interpreter raw condition");
                check(actual[3], current_value, "native raw condition");
                check(interpreted[sample][4], left, "interpreter raw left");
                check(actual[4], left, "native raw left");
                check(interpreted[sample][6], right, "interpreter raw right");
                check(actual[6], right, "native raw right");
            }
        }
    }
    REQUIRE(programs == 12288);
    REQUIRE(checked == 552960);
    std::printf("integer-selection-oracle programs=%llu checks=%llu\n",
                static_cast<unsigned long long>(programs), static_cast<unsigned long long>(checked));
#else
    SUCCEED("Native integer selections require an AArch64 host");
#endif
}

TEST_CASE("integer predicates preserve pending flags and full raw sources",
          "[scalar-bits][flags][production]") {
#if defined(__aarch64__)
    u64 programs{}, checked{}, current_value{};
    const auto check = [&](u64 actual, u64 expected, const char* label) {
        ++checked;
        if (actual != expected) FAIL(label << ": " << actual << " != " << expected << ", raw=" << current_value);
    };
    const std::array<u64, 9> values{0x100, 0, 1, 0x8000, 0x10000, 0x100000000ULL,
                                   0x8000000000000000ULL, UINT64_MAX, 0xfedcba9876543200ULL};
    for (const bool optimize : {false, true})
    for (const bool function_mode : {false, true})
    for (const auto type : {ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                            ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64})
    for (const auto op : {OpCode::TestZero, OpCode::TestNotZero, OpCode::Not, OpCode::TestBit})
    for (const u32 transport : {0u, 1u, 2u}) {
        ++programs;
        CAPTURE(optimize, function_mode, static_cast<u32>(type), static_cast<u32>(op), transport);
        auto config = BitConfig(false);
        config.global_opts = optimize ? Optimizations::All : Optimizations::None;
        AddressSpace address_space{config};
        HIRBuilder hir{1, false, FeatureSet{}};
        auto* function = function_mode
                ? hir.AppendFunction(Location{0xba00}, Location{0xba01}) : nullptr;
        IntrusivePtr<Block> block;
        if (!function) block = new Block{0, Location{0xba00}};
        compile::IrBuilder factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{block.get()};
        const auto raw = factory.LoadUniform(Uniform{0, ValueType::U64});
        auto input = transport ? factory.BitCast(type, raw) : Value{raw}.SetCastType(type);
        if (transport == 2) input = factory.GetResult(type, input, input);
        const auto zero = factory.LoadImm(Imm{u64{0}});
        const auto negative = factory.Sub(ValueType::U64, zero, Operand{Imm{u64{1}}});
        factory.SaveFlags(negative, Flags::NZCV);
        const auto result = op == OpCode::TestBit
                ? Value{factory.AppendInst(op, ValueType::U64, input, Imm{u64{7}})}
                : Value{factory.AppendInst(op, ValueType::U64, input)};
        factory.StoreUniform(Uniform{16, ValueType::U64}, result);
        for (const auto [offset, flag] : std::array{
                std::pair{32u, Flags::Negate}, std::pair{40u, Flags::Zero}, std::pair{48u, Flags::Overflow}})
            factory.StoreUniform(Uniform{offset, ValueType::U64},
                                 Value{factory.TestFlags(flag)}.SetCastType(ValueType::U64));
        factory.StoreUniform(Uniform{56, ValueType::U64}, raw);
        auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
        factory.ReturnToHost();
        if (function) { function->ComputeRPO(); function->IdByRPO(); }
        else body->ReIdInstr();
        const u32 bits = GetValueSizeByte(type) * 8;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        const auto validate = [&](const std::array<u64, 8>& actual) {
            const u64 value = current_value & mask;
            const u64 expected = op == OpCode::TestBit ? (value >> 7) & 1
                    : op == OpCode::TestNotZero ? value != 0 : value == 0;
            check(actual[2], expected, "predicate with live flags");
            check(actual[4], 1, "negative flag");
            check(actual[5], 0, "zero flag");
            check(actual[6], 0, "overflow flag");
            check(actual[7], current_value, "full raw source after predicate");
        };
        for (const auto value : values) {
            current_value = value;
            alignas(State) std::array<u8, sizeof(State) + 64> storage{};
            auto* state = new (storage.data()) State{};
            const std::array<u64, 8> initial{value, 0, UINT64_MAX, UINT64_MAX,
                    UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
            std::memcpy(state->uniform_buffer_begin, initial.data(), sizeof(initial));
            interp::Interpreter interpreter{*state, body};
            if (interpreter.Run() != HaltReason::CallHost) FAIL("flags predicate interpreter halt");
            std::array<u64, 8> actual{};
            std::memcpy(actual.data(), state->uniform_buffer_begin, sizeof(actual));
            validate(actual);
        }
        const auto entry = function ? TranslateIR(address_space.GetDefaultModule(), function)
                                    : TranslateIR(address_space.GetDefaultModule(), block);
        if (!entry) FAIL("flags predicate translation");
        address_space.PushCodeCache(0xba00, entry);
        for (const auto value : values) {
            current_value = value;
            Runtime runtime{&address_space};
            const std::array<u64, 8> initial{value, 0, UINT64_MAX, UINT64_MAX,
                    UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
            std::memcpy(runtime.GetUniformBuffer().data(), initial.data(), sizeof(initial));
            runtime.SetLocation(0xba00);
            if (runtime.Run() != HaltReason::CallHost) FAIL("flags predicate native halt");
            std::array<u64, 8> actual{};
            std::memcpy(actual.data(), runtime.GetUniformBuffer().data(), sizeof(actual));
            validate(actual);
        }
    }
    REQUIRE(programs == 384);
    REQUIRE(checked == 34560);
    std::printf("integer-predicate-flags-oracle programs=%llu checks=%llu\n",
                static_cast<unsigned long long>(programs), static_cast<unsigned long long>(checked));
#else
    SUCCEED("Native predicate flag preservation requires an AArch64 host");
#endif
}

TEST_CASE("integer conditions retain logical views across local and Function edges",
          "[scalar-bits][types][control][production]") {
#if defined(__aarch64__)
    u64 programs{}, checked{}, current_value{};
    const auto check = [&](u64 actual, u64 expected, const char* label) {
        ++checked;
        if (actual != expected) FAIL(label << ": " << actual << " != " << expected << ", raw=" << current_value);
    };
    const std::array<u64, 9> values{0x100, 0, 1, 0x8000, 0x10000, 0x100000000ULL,
                                   0x8000000000000000ULL, UINT64_MAX, 0xfedcba9876543200ULL};
    constexpr u64 entry_key = 0xb900, then_key = 0xb910, else_key = 0xb920;
    for (const bool optimize : {false, true})
    for (const bool function_mode : {false, true})
    for (const auto type : {ValueType::U8, ValueType::S8, ValueType::U16, ValueType::S16,
                            ValueType::U32, ValueType::S32, ValueType::U64, ValueType::S64})
    for (const u32 transport : {0u, 1u, 2u})
    for (const bool keep_input : {false, true})
    for (const u32 branch : {0u, 1u, 2u}) {
        ++programs;
        CAPTURE(optimize, function_mode, static_cast<u32>(type), transport, keep_input, branch);
        auto config = BitConfig(false);
        config.global_opts = optimize ? Optimizations::All : Optimizations::None;
        config.has_local_operation = true;
        AddressSpace address_space{config};
        HIRBuilder hir{1, false, FeatureSet{}};
        auto* function = function_mode ? hir.AppendFunction(Location{entry_key}, Location{0xba00}) : nullptr;
        IntrusivePtr<Block> block;
        if (!function) block = new Block{0, Location{entry_key}};
        compile::IrBuilder factory = function ? compile::IrBuilder{&hir} : compile::IrBuilder{block.get()};
        const auto raw = factory.LoadUniform(Uniform{0, ValueType::U64});
        auto input = transport ? factory.BitCast(type, raw) : Value{raw}.SetCastType(type);
        if (transport == 2) input = factory.GetResult(type, input, input);
        auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
        std::array<IntrusivePtr<Block>, 2> successors;
        if (branch == 2) {
            const terminal::If choice{BOOL{input}, terminal::LinkBlock{Location{then_key}},
                                                   terminal::LinkBlock{Location{else_key}}};
            if (function) {
                const auto [no, yes] = hir.If(choice);
                for (const auto [child, marker] : std::array{std::pair{yes, u64{1}}, std::pair{no, u64{0}}}) {
                    hir.SetCurBlock(child);
                    const auto value = function->LoadImm(Imm{marker});
                    function->StoreUniform(Uniform{16, ValueType::U64}, value);
                    // This is a true cross-block SSA read, after the condition.
                    if (keep_input) function->StoreUniform(Uniform{24, ValueType::U64}, raw);
                    function->EndBlock(terminal::ReturnToHost{});
                }
                function->EndFunction();
            } else {
                body->SetTerminal(choice);
                for (u32 i = 0; i < successors.size(); ++i) {
                    successors[i] = new Block{0, Location{i ? else_key : then_key}};
                    const auto marker = successors[i]->LoadImm(Imm{u64{i ? 0u : 1u}});
                    successors[i]->StoreUniform(Uniform{16, ValueType::U64}, marker);
                    if (keep_input) {
                        const auto source = successors[i]->LoadUniform(Uniform{0, ValueType::U64});
                        successors[i]->StoreUniform(Uniform{24, ValueType::U64}, source);
                    }
                    successors[i]->SetTerminal(terminal::ReturnToHost{});
                    successors[i]->ReIdInstr();
                }
            }
        } else {
            const auto zero = factory.LoadImm(Imm{u64{0}});
            const auto one = factory.LoadImm(Imm{u64{1}});
            factory.StoreUniform(Uniform{16, ValueType::U64}, zero);
            const auto* jump = factory.AppendInst(branch ? OpCode::NotGoto : OpCode::Goto,
                                                  ValueType::U8, input);
            factory.StoreUniform(Uniform{16, ValueType::U64}, one);
            factory.BindLabel(Value{jump});
            if (keep_input) factory.StoreUniform(Uniform{24, ValueType::U64}, raw);
            factory.ReturnToHost();
        }
        if (function) { function->ComputeRPO(); function->IdByRPO(); }
        else body->ReIdInstr();
        const u32 bits = branch == 2 ? 8 : GetValueSizeByte(type) * 8;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        const auto expected_marker = [&](u64 value) -> u64 {
            const bool nonzero = (value & mask) != 0;
            if (branch == 2) return nonzero;
            const bool taken = branch ? !nonzero : nonzero;
            return taken ? 0 : 1;
        };
        // The interpreter owns one Block stack. A multi-block Function instead
        // uses the independent marker/raw-value oracle below on the actual JIT.
        if (!function || branch != 2) {
            for (const auto value : values) {
                current_value = value;
                alignas(State) std::array<u8, sizeof(State) + 64> storage{};
                auto* state = new (storage.data()) State{};
                const std::array<u64, 8> initial{value, 0, UINT64_MAX, UINT64_MAX};
                std::memcpy(state->uniform_buffer_begin, initial.data(), sizeof(initial));
                interp::Interpreter interpreter{*state, body};
                auto halt = interpreter.Run();
                if (branch == 2) {
                    if (halt != HaltReason::None) FAIL("conditional Block terminal halt");
                    const auto next = state->current_loc.Value();
                    if (next != then_key && next != else_key) FAIL("conditional Block target");
                    interp::Interpreter tail{*state, successors[next == else_key ? 1 : 0].get()};
                    halt = tail.Run();
                }
                if (halt != HaltReason::CallHost) FAIL("condition interpreter halt");
                std::array<u64, 8> actual{};
                std::memcpy(actual.data(), state->uniform_buffer_begin, sizeof(actual));
                check(actual[2], expected_marker(value), "interpreter branch");
                if (keep_input) check(actual[3], value, "interpreter raw after branch");
            }
        }
        for (u32 i = 0; i < successors.size(); ++i) if (successors[i]) {
            const auto code = TranslateIR(address_space.GetDefaultModule(), successors[i]);
            if (!code) FAIL("conditional Block successor translation");
            address_space.PushCodeCache(i ? else_key : then_key, code);
        }
        const auto code = function ? TranslateIR(address_space.GetDefaultModule(), function)
                                   : TranslateIR(address_space.GetDefaultModule(), block);
        if (!code) FAIL("condition translation");
        address_space.PushCodeCache(entry_key, code);
        for (const auto value : values) {
            current_value = value;
            Runtime runtime{&address_space};
            const std::array<u64, 8> initial{value, 0, UINT64_MAX, UINT64_MAX};
            std::memcpy(runtime.GetUniformBuffer().data(), initial.data(), sizeof(initial));
            runtime.SetLocation(entry_key);
            if (runtime.Run() != HaltReason::CallHost) FAIL("condition native halt");
            std::array<u64, 8> actual{};
            std::memcpy(actual.data(), runtime.GetUniformBuffer().data(), sizeof(actual));
            check(actual[2], expected_marker(value), "native branch");
            if (keep_input) check(actual[3], value, "native raw after branch");
        }
    }
    REQUIRE(programs == 576);
    REQUIRE(checked == 14256);
    std::printf("integer-condition-oracle programs=%llu checks=%llu\n",
                static_cast<unsigned long long>(programs), static_cast<unsigned long long>(checked));
#else
    SUCCEED("Native integer conditions require an AArch64 host");
#endif
}
