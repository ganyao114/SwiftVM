#include <array>
#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using T = ir::ValueType;
namespace {
constexpr std::array requested_bits{0u, 1u, 2u, 3u, 4u, 6u};
constexpr std::array state_nzcv_bits{29u, 28u, 30u, 31u};
constexpr u64 known_state_bits = 0xffULL | (u64{1} << 26) | (0xfULL << 28);
ir::Flags Requested(u32 subset) {
    u64 mask{};
    for (u32 index = 0; index < requested_bits.size(); ++index)
        if (subset & (1u << index)) mask |= u64{1} << requested_bits[index];
    return static_cast<ir::Flags>(mask);
}
bool ExpectedTest(u64 state, ir::Flags requested) {
    const auto mask = static_cast<u64>(requested);
    bool has_condition{}, result = true, nzcv_requested{}, nzcv_true{};
    for (u32 bit = 0; bit < state_nzcv_bits.size(); ++bit) {
        if (!(mask & (u64{1} << bit))) continue;
        nzcv_requested = true;
        nzcv_true |= ((state >> state_nzcv_bits[bit]) & 1) != 0;
    }
    if (nzcv_requested) { has_condition = true; result = nzcv_true; }
    if (mask & (u64{1} << 4)) {
        bool odd{};
        for (u32 bit = 0; bit < 8; ++bit) odd ^= ((state >> bit) & 1) != 0;
        has_condition = true; result &= !odd;
    }
    if (mask & (u64{1} << 6)) {
        has_condition = true; result &= ((state >> 26) & 1) != 0;
    }
    return has_condition && result;
}
u64 ExpectedClear(u64 state, ir::Flags requested) {
    const auto mask = static_cast<u64>(requested);
    for (u32 bit = 0; bit < state_nzcv_bits.size(); ++bit)
        if (mask & (u64{1} << bit)) state &= ~(u64{1} << state_nzcv_bits[bit]);
    if (mask & (u64{1} << 6)) state &= ~(u64{1} << 26);
    if (mask & (u64{1} << 4)) state = (state & ~u64{255}) | 1;
    return state;
}
struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 256> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    ~Storage() { state->~State(); }
    u64 Get(u32 offset) const { u64 result; std::memcpy(&result, state->uniform_buffer_begin + offset, 8); return result; }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
};
using BlockFn = HaltReason (*)(backend::State*);
IntrusivePtr<ir::Block> FlagBlock(ir::Flags requested) {
    IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
    auto tested = as.TestFlags(requested).SetType(T::U64);
    auto inverted = as.TestNotFlags(requested).SetType(T::U64);
    as.StoreUniform(ir::Uniform{0, T::U64}, tested);
    as.StoreUniform(ir::Uniform{8, T::U64}, inverted);
    as.ClearFlags(requested);
    block->SetTerminal(ir::terminal::ReturnToHost{}); return block;
}
struct Failure {
    u64 mismatches{}, requested{}, initial{}, tested{}, inverted{}, cleared{};
    HaltReason reason{};
    void Observe(ir::Flags request, u64 flags, HaltReason halt, const Storage& storage) {
        const auto wanted = ExpectedTest(flags, request);
        if (halt == HaltReason::CallHost && storage.Get(0) == u64(wanted) &&
            storage.Get(8) == u64(!wanted) && storage.state->host_cpu_flags == ExpectedClear(flags, request)) return;
        if (!mismatches) {
            requested = static_cast<u64>(request); initial = flags; reason = halt;
            tested = storage.Get(0); inverted = storage.Get(8); cleared = storage.state->host_cpu_flags;
        }
        ++mismatches;
    }
};
u64 ScalarMask(u32 bits) { return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }
struct Arithmetic {
    u64 result{};
    bool carry{}, overflow{}, auxiliary{};
};
Arithmetic ExpectedArithmetic(ir::OpCode op, u64 left, u64 right, u32 bits, bool incoming_carry) {
    using O = ir::OpCode;
    const auto mask = ScalarMask(bits), sign = u64{1} << (bits - 1);
    left &= mask; right &= mask;
    if (op == O::Neg) { right = left; left = 0; }
    const bool subtract = op == O::Sub || op == O::Sbb || op == O::Neg;
    const u64 incoming = op == O::Adc ? u64(incoming_carry) : op == O::Sbb ? u64(!incoming_carry) : 0;
    Arithmetic expected;
    if (subtract) {
        const auto subtrahend = static_cast<unsigned __int128>(right) + incoming;
        expected.result = u64(static_cast<unsigned __int128>(left) - subtrahend) & mask;
        expected.carry = static_cast<unsigned __int128>(left) >= subtrahend; // Guest C is no-borrow.
        expected.overflow = ((left ^ right) & (left ^ expected.result) & sign) != 0;
    } else if (op == O::Add || op == O::Adc) {
        const auto sum = static_cast<unsigned __int128>(left) + right + incoming;
        expected.result = u64(sum) & mask; expected.carry = sum > mask;
        expected.overflow = ((~(left ^ right)) & (left ^ expected.result) & sign) != 0;
    } else if (op == O::And) expected.result = left & right;
    else if (op == O::Or) expected.result = left | right;
    else if (op == O::Xor) expected.result = left ^ right;
    else if (op == O::AndNot) expected.result = left & ~right & mask;
    else if (op == O::Mul) expected.result = u64(static_cast<unsigned __int128>(left) * right) & mask;
    expected.auxiliary = ((left ^ right ^ expected.result) & 16) != 0;
    return expected;
}
u64 PublishExpected(u64 previous, const Arithmetic& value, u32 bits, ir::Flags requested, bool arithmetic, bool logical) {
    const auto put = [&](u32 bit, bool enabled) {
        previous = (previous & ~(u64{1} << bit)) | (u64(enabled) << bit);
    };
    if (True(requested & ir::Flags::Negate)) put(31, ((value.result >> (bits - 1)) & 1) != 0);
    if (True(requested & ir::Flags::Zero)) put(30, value.result == 0);
    if (True(requested & ir::Flags::Parity)) previous = (previous & ~u64{255}) | (value.result & 255);
    if (arithmetic || logical) {
        if (True(requested & ir::Flags::Carry)) put(29, arithmetic && value.carry);
        if (True(requested & ir::Flags::Overflow)) put(28, arithmetic && value.overflow);
    }
    if (arithmetic && True(requested & ir::Flags::AuxiliaryCarry)) put(26, value.auxiliary);
    return previous;
}
u64 ChangeIncomingCarryAndNativeFlags(u64 pointer, u64 value, u64) {
    // Make a delayed ADC/SBB publication depend on producer-time metadata,
    // rather than the current guest C or the helper's native arithmetic flags.
    auto* state = reinterpret_cast<backend::State*>(pointer);
    state->host_cpu_flags ^= (u64{1} << 29) | (u64{1} << 26);
    asm volatile("xor %%r11d, %%r11d\n\tsub $1, %%r11d" ::: "r11", "cc");
    return value;
}
u64 RecordAndChangeFlags(u64 pointer, u64 value, u64 unused) {
    auto* state = reinterpret_cast<backend::State*>(pointer);
    std::memcpy(state->uniform_buffer_begin + 56, &state->host_cpu_flags, 8);
    return ChangeIncomingCarryAndNativeFlags(pointer, value, unused);
}
size_t EncodingSites(const x64::Translator& translator, const Xbyak::CodeGenerator& encoding) {
    size_t count{};
    for (size_t offset = 0; offset + encoding.getSize() <= translator.getSize(); ++offset)
        if (std::memcmp(translator.getCode() + offset, encoding.getCode(), encoding.getSize()) == 0) ++count;
    return count;
}
}

TEST_CASE("x64 memory BT restores incoming carry after complete RHS preparation in every scalar slot", "[x64][native-flags][incoming-carry]") {
    using O = ir::OpCode;
    for (u32 slot = 0; slot < 3; ++slot)
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (auto op : {O::Adc, O::Sbb}) for (bool cache : {false, true})
    for (u32 shape = 0; shape < 5; ++shape) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const auto mask = ScalarMask(bits), sign = u64{1} << (bits - 1);
        INFO("slot=" << slot << " bits=" << bits << " opcode=" << u32(op) << " cache=" << cache << " RHS=" << shape);
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        std::array<ir::Value, 2> prefix;
        for (u32 i = 0; i < slot; ++i) prefix[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
        const auto input = as.LoadUniform(ir::Uniform{32, T::U64}).SetType(T::U64);
        auto left = input; left = left.SetCastType(type);
        // Both RHS components alias the full input. They must be consumed
        // before narrowing the coalesced lhs and restoring CF from guest C.
        const ir::Operand rhs = shape == 0 ? ir::Operand{input} :
            ir::Operand{input, input, {shape == 1 ? ir::OperandOp::Plus : shape == 2 ? ir::OperandOp::PlusExt :
                                      shape == 3 ? ir::OperandOp::LSL : ir::OperandOp::LSR, 3}};
        const auto result = ir::Value{as.AppendInst(op, left, rhs)}.SetType(type);
        as.StoreUniform(ir::Uniform{48, type}, result);
        for (u32 i = 0; i < slot; ++i) as.StoreUniform(ir::Uniform{64 + i * 8, T::U64}, prefix[i]);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = x64::ScalarRegisterPlan::Build(block.get());
        REQUIRE(plan.Slot(input.Def()) == slot); REQUIRE(plan.Slot(result.Def()) == slot);
        x64::Translator translator{block.get(), {}, nullptr, nullptr, cache}; translator.Translate();
        REQUIRE(translator.Statistics().scalar_coalesced_binary_sites == u64(cache));
        REQUIRE(translator.Statistics().flags_snapshot_sites == 0);
        REQUIRE(translator.Statistics().flags_materialization_ir_sites == 0);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_load_sites == 0);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_store_sites == 0);
        // This census counts explicit MOVs, not the memory operand of BT.
        // The incoming carry still requires one real State.flags memory read.
        REQUIRE(translator.Statistics().At(x64::TransferRegion::StateFlags).scalar_load_sites == 0);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::StateFlags).scalar_store_sites == 0);
        Xbyak::CodeGenerator old_load, memory_bt, carry_binary;
        old_load.mov(old_load.rcx, old_load.qword[old_load.r12 + backend::state_offset_host_flags]);
        memory_bt.bt(memory_bt.qword[memory_bt.r12 + backend::state_offset_host_flags], 29);
        REQUIRE(EncodingSites(translator, old_load) == 0);
        REQUIRE(EncodingSites(translator, memory_bt) == 1);
        carry_binary.bt(carry_binary.qword[carry_binary.r12 + backend::state_offset_host_flags], 29);
        if (op == O::Sbb) carry_binary.cmc();
        const Xbyak::Reg64 destination{cache ? int(x64::ScalarRegisterPlan::host_register_indices[slot]) : carry_binary.rax.getIdx()};
        const auto emit = [&](const auto& a, const auto& b) {
            if (op == O::Adc) carry_binary.adc(a, b); else carry_binary.sbb(a, b);
        };
        if (bits == 8) emit(destination.cvt8(), carry_binary.dl);
        else if (bits == 16) emit(destination.cvt16(), carry_binary.dx);
        else if (bits == 32) emit(destination.cvt32(), carry_binary.edx);
        else emit(destination, carry_binary.rdx);
        // Verify immediate CF consumption and SBB's no-borrow inversion.
        REQUIRE(EncodingSites(translator, carry_binary) == 1);
        const auto emitted_bytes = translator.Statistics().emitted_code_bytes;
        Storage storage;
        for (u64 value : {u64{0}, u64{1}, mask, sign - 1, sign, UINT64_MAX,
                          u64{0x8000000000000001}, u64{0x12345678abcdef21}, u64{0xfedcba987654323f}})
        for (bool carry : {false, true})
        for (u64 poison : {u64{0xfedcba9876abcdef}, u64{0x123456789abcdef0}}) {
            storage.Put(32, value); storage.Put(48, 0);
            for (u32 i = 0; i < slot; ++i) storage.Put(i * 8, 0x1122334455667788ULL + i);
            const auto flags = (poison & ~(u64{1} << 29)) | (u64(carry) << 29);
            storage.state->host_cpu_flags = flags; storage.state->halt_reason = HaltReason::None;
            const u64 rhs_value = shape == 0 ? value : shape == 1 ? value + value :
                shape == 2 ? value + (value << 3) : shape == 3 ? value << (value & 63) : value >> (value & 63);
            const auto expected = ExpectedArithmetic(op, value & mask, rhs_value, bits, carry);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(48) == expected.result);
            REQUIRE(storage.state->host_cpu_flags == flags);
            for (u32 i = 0; i < slot; ++i) REQUIRE(storage.Get(64 + i * 8) == 0x1122334455667788ULL + i);
        }
        REQUIRE(translator.Statistics().emitted_code_bytes == emitted_bytes);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::StateFlags).scalar_load_sites == 0);
    }
}

TEST_CASE("x64 coalesced binary flags preserve partial publications across delayed helpers in every slot", "[x64][native-flags][coalescing]") {
    using O = ir::OpCode;
    constexpr auto first_request = ir::Flags::Negate | ir::Flags::Parity;
    constexpr auto second_request = ir::Flags::CV | ir::Flags::AuxiliaryCarry;
    constexpr auto last_request = ir::Flags::Zero;
    for (u32 slot = 0; slot < 3; ++slot)
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (auto op : {O::Add, O::Sub, O::Adc, O::Sbb, O::And, O::Or, O::Xor, O::AndNot})
    for (bool cache : {false, true}) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const auto mask = ScalarMask(bits), sign = u64{1} << (bits - 1);
        const bool arithmetic = op == O::Add || op == O::Sub || op == O::Adc || op == O::Sbb;
        INFO("slot=" << slot << " bits=" << bits << " opcode=" << u32(op) << " cache=" << cache);
        Storage storage;
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        std::array<ir::Value, 2> prefix;
        for (u32 i = 0; i < slot; ++i) prefix[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
        const auto left = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        const auto right = as.LoadUniform(ir::Uniform{40, type}).SetType(type);
        const auto result = ir::Value{as.AppendInst(op, left, ir::Operand{right})}.SetType(type);
        as.SaveFlags(result, first_request);
        const auto pointer = as.LoadImm(ir::Imm{reinterpret_cast<u64>(storage.state)}).SetType(T::U64);
        (void)as.CallHost(&RecordAndChangeFlags, pointer, result, result).SetType(T::U64);
        as.SaveFlags(result, second_request);
        as.SaveFlags(result, last_request);
        as.StoreUniform(ir::Uniform{48, type}, result);
        for (u32 i = 0; i < slot; ++i) as.StoreUniform(ir::Uniform{64 + i * 8, T::U64}, prefix[i]);
        // Keep RHS live past every prefix. With all three slots occupied,
        // its interval must not replace a shorter prefix and create an
        // earlier free slot at the arithmetic Read/Write boundary. The lhs
        // then hands its exact physical slot to the coalesced result. Later
        // helper-pointer pressure may displace a prefix/RHS, never this result.
        as.StoreUniform(ir::Uniform{80, type}, right);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = x64::ScalarRegisterPlan::Build(block.get());
        REQUIRE(plan.Slot(left.Def()) == slot); REQUIRE(plan.Slot(result.Def()) == slot);
        if (slot == 2) REQUIRE_FALSE(plan.Slot(right.Def()).has_value());
        REQUIRE(plan.ValidateNonOverlapping());
        REQUIRE(plan.RequiresCanonicalHome(result.Def())); // Helper operands read full initialized homes.
        x64::Translator translator{block.get(), {}, nullptr, nullptr, cache}; translator.Translate();
        REQUIRE(translator.Statistics().scalar_coalesced_binary_sites == u64(cache));
        REQUIRE(translator.Statistics().flags_snapshot_sites == u64(arithmetic));
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_store_sites == u64(arithmetic));
        REQUIRE(translator.Statistics().flags_materialization_ir_sites == 3);
        REQUIRE(translator.Statistics().helper_call_sites == 2);
        for (auto pair : {std::array<u64, 2>{0, 0}, std::array<u64, 2>{mask, 1},
                          std::array<u64, 2>{sign - 1, 1}, std::array<u64, 2>{sign, mask},
                          std::array<u64, 2>{sign, 1}, std::array<u64, 2>{sign - 1, mask},
                          std::array<u64, 2>{0x19, 0x07}, std::array<u64, 2>{0, 1}})
        for (bool carry : {false, true}) for (bool auxiliary : {false, true}) {
            storage.Put(32, pair[0]); storage.Put(40, pair[1]); storage.Put(48, 0); storage.Put(56, 0); storage.Put(80, 0);
            for (u32 i = 0; i < slot; ++i) storage.Put(i * 8, 0x123456789abcdef0ULL + i);
            const u64 initial = (0xfedcba9876abcdefULL & ~((u64{1} << 29) | (u64{1} << 26))) |
                (u64(carry) << 29) | (u64(auxiliary) << 26);
            storage.state->host_cpu_flags = initial; storage.state->halt_reason = HaltReason::None;
            const auto expected = ExpectedArithmetic(op, pair[0], pair[1], bits, carry);
            const auto first = PublishExpected(initial, expected, bits, first_request, arithmetic, !arithmetic);
            const auto changed = first ^ ((u64{1} << 29) | (u64{1} << 26));
            const auto second = PublishExpected(changed, expected, bits, second_request, arithmetic, !arithmetic);
            const auto final = PublishExpected(second, expected, bits, last_request, arithmetic, !arithmetic);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(48) == expected.result); REQUIRE(storage.Get(56) == first);
            REQUIRE(storage.Get(80) == (pair[1] & mask));
            REQUIRE(storage.state->host_cpu_flags == final);
            for (u32 i = 0; i < slot; ++i) REQUIRE(storage.Get(64 + i * 8) == 0x123456789abcdef0ULL + i);
        }
    }
}

TEST_CASE("x64 native guest flag operations exhaust saved byte NZCV AF and request combinations", "[x64][native-flags]") {
    Failure failure;
    u64 executions{};
    for (u32 subset = 0; subset < 64; ++subset) {
        const auto requested = Requested(subset);
        auto block = FlagBlock(requested); x64::Translator translator{block.get(), {}}; translator.Translate();
        const auto fn = translator.getCode<BlockFn>();
        // Clear/Test/TestNot lower natively; the mandatory recovery epilogue
        // EndMemory remains one helper site even for an empty requested mask.
        REQUIRE(translator.Statistics().helper_call_sites == 1);
        Storage storage;
        for (u64 poison : {u64{0}, u64{0xa5a55a5adeadbeef} & ~known_state_bits})
        for (u32 nzcv = 0; nzcv < 16; ++nzcv)
        for (u32 auxiliary = 0; auxiliary < 2; ++auxiliary)
        for (u32 byte = 0; byte < 256; ++byte) {
            u64 flags = poison | byte | (u64{auxiliary} << 26);
            for (u32 bit = 0; bit < state_nzcv_bits.size(); ++bit)
                flags |= u64{(nzcv >> bit) & 1} << state_nzcv_bits[bit];
            storage.state->host_cpu_flags = flags;
            storage.state->halt_reason = HaltReason::None;
            const auto reason = fn(storage.state); failure.Observe(requested, flags, reason, storage); ++executions;
        }
    }
    CAPTURE(failure.requested, failure.initial, failure.tested, failure.inverted, failure.cleared);
    REQUIRE(failure.mismatches == 0);
    REQUIRE(executions == 1048576);
}

TEST_CASE("x64 native guest flag operations ignore Positive and unknown request bits", "[x64][native-flags]") {
    constexpr std::array unknown_bits{5u, 7u, 32u, 63u};
    Failure failure;
    for (u32 subset = 0; subset < 64; ++subset) {
      auto reference_block = FlagBlock(Requested(subset));
      x64::Translator reference{reference_block.get(), {}}; reference.Translate();
      // Every combination of Positive and representative low/high unknown
      // bits must have precisely the effective six-flag code shape. The last
      // decoration sets every unsupported bit at once, including bit 63.
      for (u32 decoration = 0; decoration <= 16; ++decoration) {
        u64 unknown{};
        if (decoration == 16) unknown = ~u64{95};
        else for (u32 index = 0; index < unknown_bits.size(); ++index)
            if (decoration & (1u << index)) unknown |= u64{1} << unknown_bits[index];
        const auto requested = static_cast<ir::Flags>(static_cast<u64>(Requested(subset)) | unknown);
        auto block = FlagBlock(requested); x64::Translator translator{block.get(), {}}; translator.Translate();
        REQUIRE(translator.Statistics().helper_call_sites == 1);
        REQUIRE(translator.getSize() == reference.getSize());
        REQUIRE(std::memcmp(translator.getCode(), reference.getCode(), reference.getSize()) == 0);
        Storage storage;
        for (u64 flags : {u64{0}, UINT64_MAX, u64{0x80000000}, u64{0x10000000},
                          u64{0x04000000}, u64{1}, u64{3}, u64{0xdeadbeef12abcdef}}) {
            storage.state->host_cpu_flags = flags; storage.state->halt_reason = HaltReason::None;
            const auto reason = translator.getCode<BlockFn>()(storage.state);
            failure.Observe(requested, flags, reason, storage);
        }
        if (!subset) {
            REQUIRE(translator.Statistics().At(x64::TransferRegion::StateFlags).scalar_store_sites == 0);
            REQUIRE(translator.Statistics().At(x64::TransferRegion::StateFlags).scalar_load_sites == 0);
        }
      }
    }
    CAPTURE(failure.requested, failure.initial, failure.tested, failure.inverted, failure.cleared);
    REQUIRE(failure.mismatches == 0);
}

TEST_CASE("x64 native flag Clear preserves cached scalar values and full unused state", "[x64][native-flags][scalar-cache]") {
    for (bool cache : {false, true}) {
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        auto original = as.LoadUniform(ir::Uniform{32, T::U64}).SetType(T::U64);
        auto result = as.Add(original, ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64);
        as.ClearFlags(ir::Flags::Negate | ir::Flags::Parity);
        auto tested = as.TestFlags(ir::Flags::Carry | ir::Flags::Parity | ir::Flags::AuxiliaryCarry).SetType(T::U64);
        auto inverted = as.TestNotFlags(ir::Flags::None).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, tested); as.StoreUniform(ir::Uniform{8, T::U64}, inverted);
        as.StoreUniform(ir::Uniform{40, T::U64}, result); as.StoreUniform(ir::Uniform{48, T::U64}, original);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator translator{block.get(), {}, nullptr, nullptr, cache}; translator.Translate();
        REQUIRE(translator.Statistics().helper_call_sites == 1);
        REQUIRE(translator.Statistics().scalar_cache_read_sites == (cache ? 3 : 0));
        REQUIRE(translator.Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_store_sites == (cache ? 4 : 8));
        for (u64 input : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}}) {
            Storage storage; storage.Put(32, input); const u64 flags = 0xabcdef13ffffffffULL;
            storage.state->host_cpu_flags = flags;
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(0) == 0); REQUIRE(storage.Get(8) == 1);
            REQUIRE(storage.Get(40) == input + 7); REQUIRE(storage.Get(48) == input);
            REQUIRE(storage.state->host_cpu_flags == ExpectedClear(flags, ir::Flags::Negate | ir::Flags::Parity));
        }
    }
}

TEST_CASE("x64 flags snapshot only producer metadata required by later publications", "[x64][native-flags][snapshot]") {
    using O = ir::OpCode;
    const auto nzp = ir::Flags::NZ | ir::Flags::Parity;
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (auto op : {O::Add, O::Sub, O::Adc, O::Sbb, O::Neg, O::And, O::Or, O::Xor, O::AndNot, O::Mul})
    for (bool cache : {false, true}) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const auto mask = ScalarMask(bits), sign = u64{1} << (bits - 1);
        const bool logical = op == O::And || op == O::Or || op == O::Xor || op == O::AndNot;
        const bool arithmetic = op == O::Add || op == O::Sub || op == O::Adc || op == O::Sbb || op == O::Neg;
        const auto requested = logical ? ir::Flags::All : nzp;
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        auto right = as.LoadUniform(ir::Uniform{40, type}).SetType(type);
        auto* producer = op == O::Neg ? as.AppendInst(op, left) : as.AppendInst(op, left, ir::Operand{right});
        auto result = ir::Value{producer}.SetType(type);
        as.SaveFlags(result, requested); as.StoreUniform(ir::Uniform{48, type}, result);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator translator{block.get(), {}, nullptr, nullptr, cache}; translator.Translate();
        REQUIRE(translator.Statistics().flags_snapshot_sites == 0);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_store_sites == 0);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_load_sites == 0);
        REQUIRE(translator.Statistics().flags_materialization_ir_sites == 1);
        REQUIRE(translator.Statistics().helper_call_sites == 1);
        for (auto pair : {std::array<u64, 2>{0, 0}, std::array<u64, 2>{mask, 1},
                          std::array<u64, 2>{sign - 1, 1}, std::array<u64, 2>{sign, mask},
                          std::array<u64, 2>{0x19, 0x07}})
        for (bool carry : {false, true})
        for (bool auxiliary : {false, true}) {
            Storage storage; storage.Put(32, pair[0]); storage.Put(40, pair[1]);
            const u64 initial = (0xfedcba9876abcdefULL & ~((u64{1} << 29) | (u64{1} << 26))) |
                (u64(carry) << 29) | (u64(auxiliary) << 26);
            storage.state->host_cpu_flags = initial;
            const auto expected = ExpectedArithmetic(op, pair[0], pair[1], bits, carry);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(48) == expected.result);
            REQUIRE(storage.state->host_cpu_flags == PublishExpected(initial, expected, bits, requested, arithmetic, logical));
        }
    }
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (auto op : {O::Add, O::Sub, O::Adc, O::Sbb, O::Neg})
    for (bool cache : {false, true}) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const auto mask = ScalarMask(bits), sign = u64{1} << (bits - 1);
        Storage storage;
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        auto right = as.LoadUniform(ir::Uniform{40, type}).SetType(type);
        auto* producer = op == O::Neg ? as.AppendInst(op, left) : as.AppendInst(op, left, ir::Operand{right});
        auto result = ir::Value{producer}.SetType(type);
        as.SaveFlags(result, ir::Flags::NZ);
        auto pointer = as.LoadImm(ir::Imm{reinterpret_cast<u64>(storage.state)}).SetType(T::U64);
        (void)as.CallHost(&ChangeIncomingCarryAndNativeFlags, pointer, result, result).SetType(T::U64);
        as.SaveFlags(result, ir::Flags::CV | ir::Flags::AuxiliaryCarry);
        as.StoreUniform(ir::Uniform{48, type}, result);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator translator{block.get(), {}, nullptr, nullptr, cache}; translator.Translate();
        REQUIRE(translator.Statistics().flags_snapshot_sites == 1);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_store_sites == 1);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_load_sites == 1);
        REQUIRE(translator.Statistics().flags_materialization_ir_sites == 2);
        REQUIRE(translator.Statistics().helper_call_sites == 2);
        for (auto pair : {std::array<u64, 2>{0, 0}, std::array<u64, 2>{mask, 1},
                          std::array<u64, 2>{sign - 1, 1}, std::array<u64, 2>{sign, mask},
                          std::array<u64, 2>{0x19, 0x07}})
        for (bool carry : {false, true}) {
            storage.Put(32, pair[0]); storage.Put(40, pair[1]); storage.Put(48, 0);
            const u64 initial = (0xfedcba9876abcdefULL & ~(u64{1} << 29)) | (u64(carry) << 29);
            storage.state->host_cpu_flags = initial; storage.state->halt_reason = HaltReason::None;
            const auto expected = ExpectedArithmetic(op, pair[0], pair[1], bits, carry);
            // The helper changes C/AF after the first NZ publication. The
            // second publication must replace them from original metadata.
            const auto first = PublishExpected(initial, expected, bits, ir::Flags::NZ, true, false);
            const auto after_helper = first ^ ((u64{1} << 29) | (u64{1} << 26));
            const auto final = PublishExpected(after_helper, expected, bits, ir::Flags::CV | ir::Flags::AuxiliaryCarry, true, false);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(48) == expected.result); REQUIRE(storage.state->host_cpu_flags == final);
        }
    }
}

TEST_CASE("x64 flags snapshot supports multiply metadata and repeated consumers", "[x64][native-flags][snapshot]") {
    for (auto type : {T::U8, T::S8, T::U16, T::S16, T::U32, T::S32})
    for (bool cache : {false, true}) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const auto mask = ScalarMask(bits), sign = u64{1} << (bits - 1);
        const bool signed_input = ir::IsSignValueType(type);
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        auto right = as.LoadUniform(ir::Uniform{40, type}).SetType(type);
        auto result = as.Mul(left, ir::Operand{right}).SetType(type);
        as.SaveFlags(result, ir::Flags::CV); as.StoreUniform(ir::Uniform{48, type}, result);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator translator{block.get(), {}, nullptr, nullptr, cache}; translator.Translate();
        // Narrow multiply synthesizes its C/V metadata with mov; it never
        // emits the pop-to-metadata instruction counted by snapshot_sites.
        REQUIRE(translator.Statistics().flags_snapshot_sites == 0);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_store_sites == 1);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_load_sites == 1);
        REQUIRE(translator.Statistics().flags_materialization_ir_sites == 1);
        REQUIRE(translator.Statistics().helper_call_sites == 1);
        for (auto pair : {std::array<u64, 2>{mask, 2}, std::array<u64, 2>{sign, mask},
                          std::array<u64, 2>{sign - 1, 2}, std::array<u64, 2>{2, 3}}) {
            u64 product{}; bool overflow{};
            if (signed_input) {
                const auto signed_value = [&](u64 value) -> __int128 {
                    value &= mask;
                    return static_cast<__int128>(value) - ((value & sign) ? (__int128{1} << bits) : 0);
                };
                const __int128 wide = signed_value(pair[0]) * signed_value(pair[1]);
                product = u64(wide) & mask;
                overflow = wide < -(__int128{1} << (bits - 1)) || wide > (__int128{1} << (bits - 1)) - 1;
            } else {
                const auto wide = static_cast<unsigned __int128>(pair[0] & mask) * (pair[1] & mask);
                product = u64(wide) & mask; overflow = wide > mask;
            }
            Storage storage; storage.Put(32, pair[0]); storage.Put(40, pair[1]);
            const u64 initial = 0xfedcba9876abcdefULL;
            storage.state->host_cpu_flags = initial;
            const u64 expected_flags = (initial & ~((u64{1} << 29) | (u64{1} << 28))) |
                (u64(overflow) << 29) | (u64(overflow) << 28);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(48) == product); REQUIRE(storage.state->host_cpu_flags == expected_flags);
        }
    }
    for (auto type : {T::U64, T::S64})
    for (bool cache : {false, true}) {
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        auto right = as.LoadUniform(ir::Uniform{40, type}).SetType(type);
        auto result = as.Mul(left, ir::Operand{right}).SetType(type);
        as.SaveFlags(result, ir::Flags::CV | ir::Flags::AuxiliaryCarry);
        as.StoreUniform(ir::Uniform{48, type}, result); block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator translator{block.get(), {}, nullptr, nullptr, cache}; translator.Translate();
        REQUIRE(translator.Statistics().flags_snapshot_sites == 0);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_store_sites == 0);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_load_sites == 0);
        for (auto pair : {std::array<u64, 2>{UINT64_MAX, 2}, std::array<u64, 2>{u64{1} << 63, UINT64_MAX},
                          std::array<u64, 2>{(u64{1} << 63) - 1, 2}, std::array<u64, 2>{2, 3}}) {
            Storage storage; storage.Put(32, pair[0]); storage.Put(40, pair[1]);
            const u64 initial = 0xfedcba9876abcdefULL; storage.state->host_cpu_flags = initial;
            // Signed/unsigned 64-bit multiplication has the same low word.
            // This IR's 64-bit Mul deliberately preserves requested C/V/AF.
            const auto product = u64(static_cast<unsigned __int128>(pair[0]) * pair[1]);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(48) == product); REQUIRE(storage.state->host_cpu_flags == initial);
        }
    }
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (bool cache : {false, true}) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const auto mask = ScalarMask(bits), sign = u64{1} << (bits - 1);
        Storage storage;
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        auto right = as.LoadUniform(ir::Uniform{40, type}).SetType(type);
        auto result = as.Add(left, ir::Operand{right}).SetType(type);
        as.SaveFlags(result, ir::Flags::CV);
        auto pointer = as.LoadImm(ir::Imm{reinterpret_cast<u64>(storage.state)}).SetType(T::U64);
        (void)as.CallHost(&ChangeIncomingCarryAndNativeFlags, pointer, result, result).SetType(T::U64);
        // A second C/V consumption must read the same single producer-time
        // snapshot even after the helper changes State C and native flags.
        as.SaveFlags(result, ir::Flags::CV);
        as.StoreUniform(ir::Uniform{48, type}, result); block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator translator{block.get(), {}, nullptr, nullptr, cache}; translator.Translate();
        REQUIRE(translator.Statistics().flags_snapshot_sites == 1);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_store_sites == 1);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_load_sites == 2);
        REQUIRE(translator.Statistics().flags_materialization_ir_sites == 2);
        REQUIRE(translator.Statistics().helper_call_sites == 2);
        for (auto pair : {std::array<u64, 2>{mask, 1}, std::array<u64, 2>{sign - 1, 1},
                          std::array<u64, 2>{sign, sign}, std::array<u64, 2>{2, 3}}) {
            storage.Put(32, pair[0]); storage.Put(40, pair[1]); storage.Put(48, 0);
            const u64 initial = 0xfedcba9876abcdefULL;
            storage.state->host_cpu_flags = initial; storage.state->halt_reason = HaltReason::None;
            const auto expected = ExpectedArithmetic(ir::OpCode::Add, pair[0], pair[1], bits, false);
            const auto first = PublishExpected(initial, expected, bits, ir::Flags::CV, true, false);
            const auto changed = first ^ ((u64{1} << 29) | (u64{1} << 26));
            const auto final = PublishExpected(changed, expected, bits, ir::Flags::CV, true, false);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(48) == expected.result); REQUIRE(storage.state->host_cpu_flags == final);
        }
    }
}

TEST_CASE("x64 BranchOnlyFlags immediately feeds a terminal condition with dead flag successors", "[x64][native-flags][snapshot]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (auto condition : {ir::Cond::CS, ir::Cond::VS})
    for (bool cache : {false, true}) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const auto mask = ScalarMask(bits), sign = u64{1} << (bits - 1);
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        auto right = as.LoadUniform(ir::Uniform{40, type}).SetType(type);
        auto result = as.Add(left, ir::Operand{right}).SetType(type);
        // No instruction/helper separates this pseudo from its sole terminal
        // consumer. Both terminal leaves contain no subsequent flag reads and
        // there is no guest-link edge across a unit boundary.
        as.BranchOnlyFlags(result, ir::Flags::CV);
        block->SetTerminal(ir::terminal::Condition{condition,
            ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}});
        x64::Translator translator{block.get(), {}, nullptr, nullptr, cache}; translator.Translate();
        REQUIRE(translator.Statistics().flags_snapshot_sites == 1);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_store_sites == 1);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_load_sites == 1);
        REQUIRE(translator.Statistics().flags_materialization_ir_sites == 1);
        REQUIRE(translator.Statistics().helper_call_sites == 1);
        for (auto pair : {std::array<u64, 2>{mask, 1}, std::array<u64, 2>{sign - 1, 1},
                          std::array<u64, 2>{sign, sign}, std::array<u64, 2>{2, 3}}) {
            Storage storage; storage.Put(32, pair[0]); storage.Put(40, pair[1]);
            storage.state->host_cpu_flags = 0xfedcba9876abcdefULL;
            const auto expected = ExpectedArithmetic(ir::OpCode::Add, pair[0], pair[1], bits, false);
            const bool taken = condition == ir::Cond::CS ? expected.carry : expected.overflow;
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == (taken ? HaltReason::CallHost : HaltReason::None));
        }
    }
}

TEST_CASE("x64 arithmetic NZ reuses an existing snapshot across partial repeated publications", "[x64][native-flags][snapshot]") {
    using O = ir::OpCode;
    for (auto type : {T::U8, T::S8, T::U16, T::S16, T::U32, T::S32, T::U64, T::S64})
    for (auto op : {O::Add, O::Sub, O::Adc, O::Sbb, O::Neg})
    for (bool cache : {false, true})
    for (auto requested : {ir::Flags::NZCV, ir::Flags::Negate | ir::Flags::Carry,
                           ir::Flags::Zero | ir::Flags::AuxiliaryCarry,
                           ir::Flags::NZCV | ir::Flags::Parity, ir::Flags::All}) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const auto mask = ScalarMask(bits), sign = u64{1} << (bits - 1);
        const bool parity = True(requested & ir::Flags::Parity);
        Storage storage;
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        auto right = as.LoadUniform(ir::Uniform{40, type}).SetType(type);
        auto* producer = op == O::Neg ? as.AppendInst(op, left) : as.AppendInst(op, left, ir::Operand{right});
        auto result = ir::Value{producer}.SetType(type);
        as.SaveFlags(result, requested);
        auto pointer = as.LoadImm(ir::Imm{reinterpret_cast<u64>(storage.state)}).SetType(T::U64);
        (void)as.CallHost(&RecordAndChangeFlags, pointer, result, result).SetType(T::U64);
        // Reuse the original SF/ZF and C/V/AF after both native flags and
        // guest C/AF changed. The helper records the first publication too.
        as.SaveFlags(result, requested);
        as.StoreUniform(ir::Uniform{48, type}, result); block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator translator{block.get(), {}, nullptr, nullptr, cache}; translator.Translate();
        REQUIRE(translator.Statistics().flags_snapshot_sites == 1);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_store_sites == 1);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_load_sites == 2);
        REQUIRE(translator.Statistics().flags_materialization_ir_sites == 2);
        REQUIRE(translator.Statistics().helper_call_sites == 2);
        // Only requested parity rereads the result at either SaveFlags.
        const u64 result_reads = (op == O::Neg ? 5 : 6) + (parity ? 2 : 0);
        const u64 coalesced = cache && op != O::Neg ? 1 : 0;
        REQUIRE(translator.Statistics().scalar_coalesced_binary_sites == coalesced);
        REQUIRE(translator.Statistics().scalar_cache_read_sites == (cache ? result_reads - coalesced : 0));
        REQUIRE(translator.Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_load_sites == (cache ? 0 : result_reads));
        for (auto pair : {std::array<u64, 2>{0, 0}, std::array<u64, 2>{mask, 1},
                          std::array<u64, 2>{sign - 1, 1}, std::array<u64, 2>{sign, sign}})
        for (bool carry : {false, true}) {
            storage.Put(32, pair[0]); storage.Put(40, pair[1]); storage.Put(48, 0); storage.Put(56, 0);
            const u64 initial = (0xfedcba9876abcdefULL & ~(u64{1} << 29)) | (u64(carry) << 29);
            storage.state->host_cpu_flags = initial; storage.state->halt_reason = HaltReason::None;
            const auto expected = ExpectedArithmetic(op, pair[0], pair[1], bits, carry);
            const auto first = PublishExpected(initial, expected, bits, requested, true, false);
            const auto changed = first ^ ((u64{1} << 29) | (u64{1} << 26));
            const auto final = PublishExpected(changed, expected, bits, requested, true, false);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(48) == expected.result); REQUIRE(storage.Get(56) == first);
            REQUIRE(storage.state->host_cpu_flags == final);
        }
    }
    // Synthesized narrow-Mul metadata has C/V alone: N/Z/parity must still
    // come from the canonical result even when C/V metadata is also loaded.
    for (auto type : {T::U8, T::S8, T::U16, T::S16, T::U32, T::S32})
    for (bool cache : {false, true}) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const auto mask = ScalarMask(bits), sign = u64{1} << (bits - 1);
        const bool signed_input = ir::IsSignValueType(type);
        constexpr auto requested = ir::Flags::NZCV | ir::Flags::Parity;
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        auto right = as.LoadUniform(ir::Uniform{40, type}).SetType(type);
        auto result = as.Mul(left, ir::Operand{right}).SetType(type);
        as.SaveFlags(result, requested); as.StoreUniform(ir::Uniform{48, type}, result);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator translator{block.get(), {}, nullptr, nullptr, cache}; translator.Translate();
        REQUIRE(translator.Statistics().flags_snapshot_sites == 0);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_store_sites == 1);
        REQUIRE(translator.Statistics().At(x64::TransferRegion::ProducerMetadata).scalar_load_sites == 1);
        REQUIRE(translator.Statistics().scalar_cache_read_sites == (cache ? 4 : 0));
        for (auto pair : {std::array<u64, 2>{0, 0}, std::array<u64, 2>{mask, 1},
                          std::array<u64, 2>{sign, mask}, std::array<u64, 2>{sign - 1, 2}}) {
            Arithmetic expected; bool overflow{};
            if (signed_input) {
                const auto signed_value = [&](u64 value) -> __int128 {
                    value &= mask;
                    return static_cast<__int128>(value) - ((value & sign) ? (__int128{1} << bits) : 0);
                };
                const auto wide = signed_value(pair[0]) * signed_value(pair[1]);
                expected.result = u64(wide) & mask;
                overflow = wide < -(__int128{1} << (bits - 1)) || wide > (__int128{1} << (bits - 1)) - 1;
            } else {
                const auto wide = static_cast<unsigned __int128>(pair[0] & mask) * (pair[1] & mask);
                expected.result = u64(wide) & mask; overflow = wide > mask;
            }
            Storage storage; storage.Put(32, pair[0]); storage.Put(40, pair[1]);
            const u64 initial = 0xfedcba9876abcdefULL; storage.state->host_cpu_flags = initial;
            auto final = PublishExpected(initial, expected, bits, requested, false, false);
            final = (final & ~((u64{1} << 29) | (u64{1} << 28))) | (u64(overflow) << 29) | (u64(overflow) << 28);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(48) == expected.result); REQUIRE(storage.state->host_cpu_flags == final);
        }
    }
}
