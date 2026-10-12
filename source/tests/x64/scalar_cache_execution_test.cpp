#include <array>
#include <cstring>
#include <sys/mman.h>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/x64/translator.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/code_serial.h"
#include "runtime/backend/riscv64/memory_protocol.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using T = ir::ValueType;
using R = x64::TransferRegion;
namespace {
struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 4096> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    ~Storage() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    u64 Get(u32 offset) const { u64 result; std::memcpy(&result, state->uniform_buffer_begin + offset, 8); return result; }
};
using BlockFn = HaltReason (*)(backend::State*);
struct Compiled {
    x64::Translator translator;
    explicit Compiled(ir::Block* block, bool cache) : translator(block, {}, nullptr, nullptr, cache) { translator.Translate(); }
    HaltReason Run(Storage& storage) { return translator.getCode<BlockFn>()(storage.state); }
};
IntrusivePtr<ir::Block> NewBlock() { return new ir::Block(ir::Location{0x1000}); }
void Store(ir::Assembler& as, u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
void CheckStores(const Compiled& off, const Compiled& on) {
    const auto& baseline = off.translator.Statistics().At(R::CanonicalSSA);
    const auto& cached = on.translator.Statistics().At(R::CanonicalSSA);
    REQUIRE(cached.scalar_store_sites <= baseline.scalar_store_sites);
    REQUIRE(cached.vector_store_sites == baseline.vector_store_sites);
    REQUIRE(cached.store_bytes <= baseline.store_bytes);
    const auto& off_uniform = off.translator.Statistics().At(R::Uniform);
    const auto& on_uniform = on.translator.Statistics().At(R::Uniform);
    REQUIRE(on_uniform.scalar_store_sites == off_uniform.scalar_store_sites);
    REQUIRE(on_uniform.vector_store_sites == off_uniform.vector_store_sites);
    REQUIRE(on_uniform.store_bytes == off_uniform.store_bytes);
    REQUIRE(on.translator.FrameSize() == off.translator.FrameSize());
    REQUIRE(off.translator.Statistics().scalar_cache_read_sites == 0);
    REQUIRE(off.translator.Statistics().scalar_cache_write_sites == 0);
}
Config TestConfig(bool cache) {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
            .backend_isa = kX86_64, .uniform_buffer_size = 4096, .static_program = true,
            .global_opts = cache ? Optimizations::ScalarRegisterCache : Optimizations::None,
            .stack_alignment = 16};
}
constexpr std::array binary_ops{ir::OpCode::Add, ir::OpCode::Sub, ir::OpCode::Adc, ir::OpCode::Sbb,
                                ir::OpCode::And, ir::OpCode::Or, ir::OpCode::Xor, ir::OpCode::AndNot};
u64 BinaryMask(T type) {
    const auto bits = ir::GetValueSizeByte(type) * 8;
    return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
}
u64 BinaryExpected(ir::OpCode op, u64 left, u64 right, bool carry, T type) {
    using O = ir::OpCode;
    u64 result{};
    switch (op) {
        case O::Add: result = left + right; break;
        case O::Sub: result = left - right; break;
        case O::Adc: result = left + right + u64(carry); break;
        case O::Sbb: result = left - right - u64(!carry); break;
        case O::And: result = left & right; break;
        case O::Or: result = left | right; break;
        case O::Xor: result = left ^ right; break;
        case O::AndNot: result = left & ~right; break;
        default: FAIL("unexpected binary model opcode");
    }
    return result & BinaryMask(type);
}
// Generate the exact encodings with Xbyak, including required REX for BPL
// and R15B. This fixture has one input publication and one result read in
// the chosen register. Before coalescing it had two of each 64-bit MOV.
size_t EncodingSites(const x64::Translator& translator, const Xbyak::CodeGenerator& encoding) {
    size_t count{};
    for (size_t offset = 0; offset + encoding.getSize() <= translator.getSize(); ++offset)
        if (std::memcmp(translator.getCode() + offset, encoding.getCode(), encoding.getSize()) == 0) ++count;
    return count;
}
void BinaryEncoding(Xbyak::CodeGenerator& generator, ir::OpCode op, unsigned index, T type) {
    const Xbyak::Reg64 destination{int(index)};
    const auto emit = [&](const auto& left, const auto& right) {
        using O = ir::OpCode;
        switch (op) {
            case O::Add: generator.add(left, right); break;
            case O::Sub: generator.sub(left, right); break;
            case O::Adc: generator.adc(left, right); break;
            case O::Sbb: generator.sbb(left, right); break;
            case O::And: case O::AndNot: generator.and_(left, right); break;
            case O::Or: generator.or_(left, right); break;
            case O::Xor: generator.xor_(left, right); break;
            default: FAIL("unexpected binary encoding opcode");
        }
    };
    const auto bits = ir::GetValueSizeByte(type) * 8;
    const u64 value = (op == ir::OpCode::AndNot ? ~u64{0x19} : u64{0x19}) & BinaryMask(type);
    // Xbyak expects sign extension for negative 16-bit immediates.
    const u32 immediate = bits == 16 && (value & 0x8000) ? u32(value) | 0xffff0000u : u32(value);
    if (bits == 8) emit(destination.cvt8(), immediate);
    else if (bits == 16) emit(destination.cvt16(), immediate);
    else if (bits == 32) emit(destination.cvt32(), immediate);
    else emit(destination, immediate);
}
u64 ClobberCallerSaved(u64 state_address, u64 value, u64) {
    auto* state = reinterpret_cast<backend::State*>(state_address);
    const u64 replacement = 0xaaaa5555bbbb6666ULL;
    std::memcpy(state->uniform_buffer_begin, &replacement, 8);
    asm volatile("pxor %%xmm0, %%xmm0\n\tpxor %%xmm1, %%xmm1\n\tpxor %%xmm2, %%xmm2\n\t"
                 "xor %%r8, %%r8\n\txor %%r9, %%r9\n\txor %%r10, %%r10\n\txor %%r11, %%r11"
                 ::: "xmm0", "xmm1", "xmm2", "r8", "r9", "r10", "r11", "cc");
    return value * 7 + 5;
}

TEST_CASE("x64 scalar replacement preserves every early and late displaced snapshot", "[x64][scalar-cache][replacement]") {
    for (u32 victim = 0; victim < 3; ++victim) {
        CAPTURE(victim);
        auto block = NewBlock(); ir::Assembler as{block.get()};
        std::array<ir::Value, 3> inputs;
        for (u32 i = 0; i < 3; ++i) inputs[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
        Store(as, 64, inputs[victim]);
        auto candidate = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
        Store(as, 72, candidate); Store(as, 80, inputs[victim]);
        for (u32 i = 0; i < 3; ++i) if (i != victim) Store(as, 88 + i * 8, inputs[i]);
        Store(as, 112, inputs[victim]); block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
        REQUIRE_FALSE(on.translator.ScalarPlan().Slot(inputs[victim].Def()));
        REQUIRE(on.translator.ScalarPlan().RequiresCanonicalHome(inputs[victim].Def()));
        REQUIRE(on.translator.ScalarPlan().Slot(candidate.Def()) == victim);
        REQUIRE(on.translator.Statistics().At(R::CanonicalSSA).scalar_load_sites > 0);
        REQUIRE(on.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites > 0);
        REQUIRE(on.translator.Statistics().scalar_cache_read_sites > 0);
        for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8000000000000000}, u64{0xfedcba9876543210}}) {
            Storage baseline, cached;
            for (auto* storage : {&baseline, &cached}) {
                std::memset(storage->state->uniform_buffer_begin, 0xa5, 4096);
                for (u32 i = 0; i < 4; ++i) storage->Put(i * 8, seed ^ (0x9e3779b97f4a7c15ULL * (i + 1)));
            }
            REQUIRE(off.Run(baseline) == HaltReason::CallHost); REQUIRE(on.Run(cached) == HaltReason::CallHost);
            REQUIRE(std::memcmp(baseline.state->uniform_buffer_begin, cached.state->uniform_buffer_begin, 4096) == 0);
            const auto original = seed ^ (0x9e3779b97f4a7c15ULL * (victim + 1));
            REQUIRE(cached.Get(64) == original); REQUIRE(cached.Get(80) == original); REQUIRE(cached.Get(112) == original);
            REQUIRE(cached.Get(72) == (seed ^ (0x9e3779b97f4a7c15ULL * 4)));
            for (u32 i = 0; i < 3; ++i) if (i != victim)
                REQUIRE(cached.Get(88 + i * 8) == (seed ^ (0x9e3779b97f4a7c15ULL * (i + 1))));
        }
    }
    // D was itself admitted and then displaced: its original Write must also
    // become home-only after final interval compaction.
    auto block = NewBlock(); ir::Assembler as{block.get()};
    std::array<ir::Value, 7> values;
    constexpr std::array<u32, 7> definitions{0, 1, 2, 4, 6, 8, 10};
    constexpr std::array<u32, 7> endpoints{33, 31, 29, 25, 21, 17, 13};
    for (u32 position = 0; position <= 33; ++position) {
        bool emitted{};
        for (u32 i = 0; i < values.size(); ++i) {
            if (position == definitions[i]) {
                values[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64); emitted = true;
            } else if (position == endpoints[i]) { Store(as, 128 + i * 8, values[i]); emitted = true; }
        }
        if (position == 3) { Store(as, 64, values[0]); emitted = true; }
        for (u32 i = 3; i < values.size(); ++i)
            if (position == definitions[i] + 1) { Store(as, 64 + i * 8, values[i]); emitted = true; }
        if (!emitted) as.AppendInst(ir::OpCode::Nop);
    }
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
    REQUIRE(on.translator.ScalarPlan().ValidateNonOverlapping());
    REQUIRE(on.translator.ScalarPlan().Intervals().size() == 3);
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE_FALSE(on.translator.ScalarPlan().Slot(values[i].Def()));
        REQUIRE(on.translator.ScalarPlan().RequiresCanonicalHome(values[i].Def()));
    }
    for (u64 seed : {u64{0}, UINT64_MAX, u64{0xfedcba9876543210}}) {
        Storage baseline, cached;
        for (auto* storage : {&baseline, &cached}) {
            std::memset(storage->state->uniform_buffer_begin, 0xa5, 4096);
            for (u32 i = 0; i < values.size(); ++i) storage->Put(i * 8, seed ^ (0x9e3779b97f4a7c15ULL * (i + 1)));
        }
        REQUIRE(off.Run(baseline) == HaltReason::CallHost); REQUIRE(on.Run(cached) == HaltReason::CallHost);
        REQUIRE(std::memcmp(baseline.state->uniform_buffer_begin, cached.state->uniform_buffer_begin, 4096) == 0);
        REQUIRE(cached.Get(64) == (seed ^ 0x9e3779b97f4a7c15ULL));
        for (u32 i = 0; i < values.size(); ++i) {
            const auto expected = seed ^ (0x9e3779b97f4a7c15ULL * (i + 1));
            REQUIRE(cached.Get(128 + i * 8) == expected);
            if (i >= 3) REQUIRE(cached.Get(64 + i * 8) == expected);
        }
    }
}

TEST_CASE("x64 displaced snapshot survives uniform mutation and raw reads across a callback", "[x64][scalar-cache][replacement][helper]") {
    for (u64 seed : {u64{0}, UINT64_MAX, u64{0x8000000000000000}, u64{0xfedcba9876543210}}) {
        Storage baseline, cached;
        for (auto* storage : {&baseline, &cached}) {
            std::memset(storage->state->uniform_buffer_begin, 0xa5, 4096);
            for (u32 i = 0; i < 4; ++i) storage->Put(i * 8, seed ^ (0x9e3779b97f4a7c15ULL * (i + 1)));
            auto block = NewBlock(); ir::Assembler as{block.get()};
            std::array<ir::Value, 3> inputs;
            for (u32 i = 0; i < 3; ++i) inputs[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
            Store(as, 72, inputs[0]);
            auto candidate = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
            Store(as, 64, candidate);
            auto pointer = as.LoadImm(ir::Imm{reinterpret_cast<u64>(storage->state)}).SetType(T::U64);
            auto called = as.CallHost(&ClobberCallerSaved, pointer, inputs[0], inputs[0]).SetType(T::U64);
            auto reloaded = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
            auto raw = as.BitCast(inputs[0]).SetType(T::V128); Store(as, 112, raw);
            Store(as, 136, reloaded); Store(as, 144, inputs[1]); Store(as, 152, inputs[2]);
            Store(as, 160, inputs[0]); Store(as, 168, called);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            const auto plan = x64::ScalarRegisterPlan::Build(block.get());
            REQUIRE(plan.ValidateNonOverlapping()); REQUIRE_FALSE(plan.Slot(inputs[0].Def()));
            REQUIRE(plan.RequiresCanonicalHome(inputs[0].Def())); REQUIRE(plan.Slot(candidate.Def()) == 0);
            REQUIRE(plan.Slot(inputs[1].Def()) == 1); REQUIRE(plan.Slot(inputs[2].Def()) == 2);
            const bool cache = storage == &cached;
            Compiled code{block.get(), cache};
            const auto mxcsr = _mm_getcsr();
            REQUIRE(code.Run(*storage) == HaltReason::CallHost); REQUIRE(_mm_getcsr() == mxcsr);
            REQUIRE(code.translator.Statistics().helper_call_sites == 2);
            if (cache) {
                REQUIRE(code.translator.Statistics().scalar_cache_read_sites > 0);
                REQUIRE(code.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites > 0);
            }
            const auto original = seed ^ 0x9e3779b97f4a7c15ULL;
            REQUIRE(storage->Get(0) == 0xaaaa5555bbbb6666ULL); REQUIRE(storage->Get(136) == storage->Get(0));
            REQUIRE(storage->Get(72) == original); REQUIRE(storage->Get(160) == original);
            REQUIRE(storage->Get(112) == original); REQUIRE(storage->Get(120) == 0);
            REQUIRE(storage->Get(168) == original * 7 + 5);
            REQUIRE(storage->Get(64) == (seed ^ (0x9e3779b97f4a7c15ULL * 4)));
            REQUIRE(storage->Get(144) == (seed ^ (0x9e3779b97f4a7c15ULL * 2)));
            REQUIRE(storage->Get(152) == (seed ^ (0x9e3779b97f4a7c15ULL * 3)));
        }
        REQUIRE(std::memcmp(baseline.state->uniform_buffer_begin, cached.state->uniform_buffer_begin, 4096) == 0);
    }
}

TEST_CASE("x64 binary coalescing emits each width in RBX RBP R15 and removes both cache MOVs", "[x64][scalar-cache][coalescing]") {
    for (u32 slot = 0; slot < 3; ++slot)
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (auto op : binary_ops) {
        INFO("slot=" << slot << " bytes=" << ir::GetValueSizeByte(type) << " op=" << u32(op));
        auto block = NewBlock(); ir::Assembler as{block.get()};
        std::array<ir::Value, 2> prefix;
        for (u32 i = 0; i < slot; ++i) prefix[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
        const auto input = as.LoadUniform(ir::Uniform{24, type}).SetType(type);
        const auto result = ir::Value{as.AppendInst(op, input, ir::Operand{ir::Imm{u64{0x19}}})}.SetType(type);
        Store(as, 64, result);
        for (u32 i = 0; i < slot; ++i) Store(as, 128 + i * 8, prefix[i]);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = x64::ScalarRegisterPlan::Build(block.get());
        REQUIRE(plan.Slot(input.Def()) == slot); REQUIRE(plan.Slot(result.Def()) == slot);
        REQUIRE(plan.Find(input.Def())->last_use_phase == x64::ScalarCachePhase::Read);
        Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
        REQUIRE(off.translator.Statistics().scalar_coalesced_binary_sites == 0);
        REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites == 1);
        REQUIRE(off.translator.Statistics().scalar_binary_immediate_sites == 1);
        REQUIRE(on.translator.Statistics().scalar_binary_immediate_sites == 1);
        REQUIRE(on.translator.Statistics().scalar_cache_read_sites == slot + 1);
        REQUIRE(on.translator.Statistics().scalar_cache_write_sites == slot + 1);
        const auto host = x64::ScalarRegisterPlan::host_register_indices[slot];
        Xbyak::CodeGenerator read, write, binary;
        read.mov(read.rax, Xbyak::Reg64{int(host)});
        write.mov(Xbyak::Reg64{int(host)}, write.rax);
        BinaryEncoding(binary, op, host, type);
        REQUIRE(EncodingSites(on.translator, read) == 1);
        REQUIRE(EncodingSites(on.translator, write) == 1);
        REQUIRE(EncodingSites(on.translator, binary) == 1);
        const auto compiled_statistics = on.translator.Statistics();
        const auto sign = u64{1} << (ir::GetValueSizeByte(type) * 8 - 1);
        for (u64 value : {u64{0}, u64{1}, u64{15}, u64{16}, sign - 1, sign,
                          BinaryMask(type), u64{0xfedcba9876543210}})
        for (bool carry : {false, true}) for (auto* code : {&off, &on}) {
            Storage storage; storage.Put(24, value);
            for (u32 i = 0; i < slot; ++i) storage.Put(i * 8, 0x123456789abcdef0ULL + i);
            const u64 flags = (0xfedcba9876abcdefULL & ~(u64{1} << 29)) | (u64(carry) << 29);
            storage.state->host_cpu_flags = flags;
            REQUIRE(code->Run(storage) == HaltReason::CallHost);
            REQUIRE(storage.Get(64) == BinaryExpected(op, value & BinaryMask(type), 0x19, carry, type));
            REQUIRE(storage.state->host_cpu_flags == flags);
            for (u32 i = 0; i < slot; ++i) REQUIRE(storage.Get(128 + i * 8) == 0x123456789abcdef0ULL + i);
        }
        // Counters describe emitted sites and remain unchanged by executions.
        REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites == compiled_statistics.scalar_coalesced_binary_sites);
        REQUIRE(on.translator.Statistics().scalar_cache_read_sites == compiled_statistics.scalar_cache_read_sites);
        REQUIRE(on.translator.Statistics().scalar_cache_write_sites == compiled_statistics.scalar_cache_write_sites);
    }
}

TEST_CASE("x64 binary coalescing reads every full RHS alias before masking a narrow lhs view", "[x64][scalar-cache][coalescing]") {
    for (u32 slot = 0; slot < 3; ++slot)
    for (auto view : {T::U8, T::U16, T::U32}) for (auto op : binary_ops)
    for (u32 shape = 0; shape < 5; ++shape) {
        INFO("slot=" << slot << " view bytes=" << ir::GetValueSizeByte(view) << " op=" << u32(op) << " shape=" << shape);
        auto block = NewBlock(); ir::Assembler as{block.get()};
        std::array<ir::Value, 2> prefix;
        for (u32 i = 0; i < slot; ++i) prefix[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
        const auto input = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
        auto narrow = input; narrow = narrow.SetCastType(view);
        const ir::Operand rhs = shape == 0 ? ir::Operand{input} :
            ir::Operand{input, input, {shape == 1 ? ir::OperandOp::Plus : shape == 2 ? ir::OperandOp::PlusExt :
                                        shape == 3 ? ir::OperandOp::LSL : ir::OperandOp::LSR, 3}};
        const auto result = ir::Value{as.AppendInst(op, narrow, rhs)}.SetType(T::U64);
        Store(as, 64, result);
        for (u32 i = 0; i < slot; ++i) Store(as, 128 + i * 8, prefix[i]);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = x64::ScalarRegisterPlan::Build(block.get());
        REQUIRE(plan.Slot(input.Def()) == slot); REQUIRE(plan.Slot(result.Def()) == slot);
        Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
        REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites == 1);
        REQUIRE(on.translator.Statistics().scalar_cache_read_sites == slot + (shape ? 3 : 2));
        REQUIRE(on.translator.Statistics().scalar_cache_write_sites == slot + 1);
        for (u64 value : {UINT64_MAX, u64{0x12345678abcdef21}, u64{0xfedcba9876543201}, u64{0x800000000000003f}})
        for (bool carry : {false, true}) for (auto* code : {&off, &on}) {
            Storage storage; storage.Put(24, value);
            for (u32 i = 0; i < slot; ++i) storage.Put(i * 8, 0x123456789abcdef0ULL + i);
            storage.state->host_cpu_flags = u64(carry) << 29;
            const u64 rhs_value = shape == 0 ? value : shape == 1 ? value + value :
                shape == 2 ? value + (value << 3) : shape == 3 ? value << (value & 63) : value >> (value & 63);
            REQUIRE(code->Run(storage) == HaltReason::CallHost);
            REQUIRE(storage.Get(64) == BinaryExpected(op, value & BinaryMask(view), rhs_value, carry, T::U64));
            REQUIRE(storage.state->host_cpu_flags == u64(carry) << 29);
            for (u32 i = 0; i < slot; ++i) REQUIRE(storage.Get(128 + i * 8) == 0x123456789abcdef0ULL + i);
        }
    }
}

TEST_CASE("x64 coalesced binary results publish complete canonical homes for raw readers", "[x64][scalar-cache][coalescing]") {
    for (u32 slot = 0; slot < 3; ++slot)
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (auto op : binary_ops) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        std::array<ir::Value, 2> prefix;
        for (u32 i = 0; i < slot; ++i) prefix[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
        const auto input = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
        const auto result = ir::Value{as.AppendInst(op, input, ir::Operand{ir::Imm{u64{0x19}}})}.SetType(type);
        Store(as, 64, result);
        const auto raw = as.BitCast(result).SetType(T::V128); Store(as, 80, raw);
        auto raw_view = result; raw_view = raw_view.SetCastType(T::V128);
        const auto high = as.VecExtract64(raw_view, ir::Imm{u32{1}}).SetType(T::U64); Store(as, 104, high);
        for (u32 i = 0; i < slot; ++i) Store(as, 128 + i * 8, prefix[i]);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = x64::ScalarRegisterPlan::Build(block.get());
        REQUIRE(plan.Slot(input.Def()) == slot); REQUIRE(plan.Slot(result.Def()) == slot);
        REQUIRE(plan.RequiresCanonicalHome(result.Def()));
        Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
        REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites == 1);
        for (u64 value : {UINT64_MAX, u64{0x12345678abcdef21}, u64{0xfedcba9876543201}})
        for (bool carry : {false, true}) for (auto* code : {&off, &on}) {
            Storage storage; storage.Put(24, value);
            for (u32 i = 0; i < slot; ++i) storage.Put(i * 8, 0x123456789abcdef0ULL + i);
            storage.state->host_cpu_flags = u64(carry) << 29;
            const auto wanted = BinaryExpected(op, value, 0x19, carry, type);
            REQUIRE(code->Run(storage) == HaltReason::CallHost);
            REQUIRE(storage.Get(64) == wanted); REQUIRE(storage.Get(80) == wanted);
            REQUIRE(storage.Get(88) == 0); REQUIRE(storage.Get(104) == 0);
            for (u32 i = 0; i < slot; ++i) REQUIRE(storage.Get(128 + i * 8) == 0x123456789abcdef0ULL + i);
        }
    }
}

TEST_CASE("x64 binary coalescing retains lhs copies when a later consumer still needs it", "[x64][scalar-cache][coalescing]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (auto op : binary_ops) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        const auto input = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        const auto result = ir::Value{as.AppendInst(op, input, ir::Operand{ir::Imm{u64{0x19}}})}.SetType(type);
        Store(as, 64, result); Store(as, 72, input); block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = x64::ScalarRegisterPlan::Build(block.get());
        REQUIRE(plan.Slot(input.Def()) != plan.Slot(result.Def()));
        Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
        REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites == 0);
        REQUIRE(on.translator.Statistics().scalar_cache_read_sites == 3);
        REQUIRE(on.translator.Statistics().scalar_cache_write_sites == 2);
        for (bool carry : {false, true}) for (auto* code : {&off, &on}) {
            Storage storage; storage.Put(0, UINT64_MAX); storage.state->host_cpu_flags = u64(carry) << 29;
            REQUIRE(code->Run(storage) == HaltReason::CallHost);
            REQUIRE(storage.Get(64) == BinaryExpected(op, BinaryMask(type), 0x19, carry, type));
            REQUIRE(storage.Get(72) == BinaryMask(type));
        }
    }
}
}

TEST_CASE("x64 scalar cache retains home-only producers under register pressure", "[x64][scalar-cache]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    std::array<ir::Value, 6> inputs;
    for (u32 index = 0; index < inputs.size(); ++index)
        inputs[index] = as.LoadUniform(ir::Uniform{index * 8, T::U64}).SetType(T::U64);
    for (u32 index = 0; index < inputs.size(); ++index) Store(as, 64 + index * 8, inputs[index]);
    for (u32 index = 0; index < inputs.size(); ++index) {
        auto result = as.Add(inputs[index], ir::Operand{ir::Imm{u64{13 + index}}}).SetType(T::U64);
        Store(as, 128 + index * 8, result);
    }
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
    REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites == 1);
    REQUIRE(on.translator.Statistics().scalar_cache_read_sites == 11);
    REQUIRE(on.translator.Statistics().scalar_cache_write_sites == 8);
    REQUIRE(off.translator.Statistics().At(R::CanonicalSSA).scalar_load_sites == 18);
    REQUIRE(on.translator.Statistics().At(R::CanonicalSSA).scalar_load_sites == 6);
    REQUIRE(off.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites == 24);
    REQUIRE(on.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites == 6);
    for (u64 seed : {u64{0}, u64{1}, UINT64_MAX, u64{0xfedcba9876543210}}) {
        for (auto* code : {&off, &on}) {
            Storage storage;
            for (u32 index = 0; index < inputs.size(); ++index) storage.Put(index * 8, seed + index * 19);
            REQUIRE(code->Run(storage) == HaltReason::CallHost);
            for (u32 index = 0; index < inputs.size(); ++index) {
                REQUIRE(storage.Get(64 + index * 8) == seed + index * 19);
                REQUIRE(storage.Get(128 + index * 8) == seed + index * 19 + 13 + index);
            }
        }
    }
}

TEST_CASE("x64 scalar cache omits every value home store for an audited scalar chain", "[x64][scalar-cache]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto sum = as.Add(input, ir::Operand{ir::Imm{u64{17}}}).SetType(T::U64);
    auto shifted = as.LslImm(sum, ir::Imm{u32{5}}).SetType(T::U64);
    Store(as, 64, shifted); block->SetTerminal(ir::terminal::ReturnToHost{});
    const auto plan = x64::ScalarRegisterPlan::Build(block.get());
    for (auto value : {input, sum, shifted}) {
        REQUIRE(plan.Slot(value.Def()));
        REQUIRE_FALSE(plan.RequiresCanonicalHome(value.Def()));
    }
    Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
    REQUIRE(off.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites == 6);
    REQUIRE(on.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites == 0);
    REQUIRE(on.translator.Statistics().At(R::CanonicalSSA).store_bytes == 0);
    REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites == 1);
    REQUIRE(on.translator.Statistics().scalar_cache_read_sites == 2);
    REQUIRE(on.translator.Statistics().scalar_cache_write_sites == 2);
    for (u64 input_value : {u64{0}, u64{1}, UINT64_MAX, u64{0xfedcba9876543210}})
    for (auto* code : {&off, &on}) {
        Storage storage; storage.Put(0, input_value);
        REQUIRE(code->Run(storage) == HaltReason::CallHost);
        REQUIRE(storage.Get(64) == ((input_value + 17) << 5));
    }
}

TEST_CASE("x64 scalar cache reuses the last input only after late and repeated physical reads", "[x64][scalar-cache][phases]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (u32 shape = 0; shape < 16; ++shape) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        const u32 lsb = bits == 8 ? 0 : bits == 16 ? 4 : 8;
        const u32 extent = bits <= 16 ? 4 : 16;
        const u64 inserted_mask = ((u64{1} << extent) - 1) << lsb;
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        auto b = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
        auto c = as.LoadUniform(ir::Uniform{16, type}).SetType(type);
        ir::Value result;
        if (shape == 0) result = as.Add(a, ir::Operand{c, c, {ir::OperandOp::Plus, 0}});
        else if (shape == 1) result = as.MulSub(a, b, c);
        else if (shape == 2) result = as.BitInsert(a, c, ir::Imm{lsb}, ir::Imm{extent});
        else if (shape == 3) result = as.LslValue(a, c);
        else if (shape == 4) result = as.LsrValue(a, c);
        else if (shape == 5) result = as.RorValue(a, c);
        else if (shape == 6) result = as.Mul(a, ir::Operand{c});
        else if (shape == 7) result = as.GetOperand(ir::Operand{c, c, {ir::OperandOp::Plus, 0}});
        else if (shape == 8) {
            auto narrow = c; narrow = narrow.SetCastType(T::U8);
            result = as.Add(a, ir::Operand{narrow});
        }
        else if (shape == 9) result = as.AsrValue(a, c);
        else if (shape == 10) result = as.MulHigh(a, c, ir::Imm{0});
        else if (shape == 11) result = as.Add(a, ir::Operand{b, c, {ir::OperandOp::PlusExt, 3}});
        else if (shape == 12) result = as.Add(a, ir::Operand{b, c, {ir::OperandOp::LSL, 0}});
        else if (shape == 13) result = as.Add(a, ir::Operand{b, c, {ir::OperandOp::LSR, 0}});
        else if (shape == 14) result = as.Add(c, ir::Operand{c, c, {ir::OperandOp::Plus, 0}});
        else result = as.GetOperand(ir::Operand{c, c, {ir::OperandOp::LSL, 0}});
        result = result.SetType(type);
        Store(as, 64, a); Store(as, 72, b); Store(as, 80, result);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = x64::ScalarRegisterPlan::Build(block.get());
        REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
        REQUIRE(plan.Slot(c.Def()) == 2); REQUIRE(plan.Slot(result.Def()) == 2);
        REQUIRE(plan.Find(c.Def())->last_use_phase == x64::ScalarCachePhase::Read);
        Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
        REQUIRE(off.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites == 8);
        REQUIRE(on.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites == 0);
        REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites == (shape == 14 ? 1 : 0));
        REQUIRE(on.translator.Statistics().scalar_cache_write_sites == (shape == 14 ? 3 : 4));
        for (u64 seed : {u64{0}, u64{1}, UINT64_MAX, u64{0x8000000000000000},
                         u64{0x0123456789abcdef}, u64{0xfedcba9876543210}}) {
            const u64 av = seed & mask, bv = (seed ^ 0xa5a5a5a5a5a5a5a5ULL) & mask;
            const u64 cv = (seed * 13 + 7) & mask;
            const auto shift = cv & (bits - 1);
            u64 expected;
            if (shape == 0) expected = av + cv + cv;
            else if (shape == 1) expected = cv - av * bv;
            else if (shape == 2) expected = (av & ~inserted_mask) | ((cv << lsb) & inserted_mask);
            else if (shape == 3) expected = av << shift;
            else if (shape == 4) expected = av >> shift;
            else if (shape == 5) expected = shift ? (av >> shift) | (av << (bits - shift)) : av;
            else if (shape == 6) expected = av * cv;
            else if (shape == 7) expected = cv + cv;
            else if (shape == 8) expected = av + (cv & 255);
            else if (shape == 9) {
                expected = av >> shift;
                if (shift && (av & (u64{1} << (bits - 1)))) expected |= mask ^ (mask >> shift);
            }
            else if (shape == 10) expected = u64((static_cast<unsigned __int128>(av) * cv) >> 64);
            else if (shape == 11) expected = av + bv + (cv << 3);
            else if (shape == 12) expected = av + (bv << (cv & 63));
            else if (shape == 13) expected = av + (bv >> (cv & 63));
            else if (shape == 14) expected = cv + cv + cv;
            else expected = cv << (cv & 63);
            expected &= mask;
            for (auto* code : {&off, &on}) {
                Storage storage; storage.Put(0, seed); storage.Put(8, bv); storage.Put(16, cv);
                storage.state->host_cpu_flags = 0xfedcba9876abcdefULL;
                REQUIRE(code->Run(storage) == HaltReason::CallHost);
                REQUIRE(storage.Get(64) == av); REQUIRE(storage.Get(72) == bv);
                REQUIRE(storage.Get(80) == expected);
                REQUIRE(storage.state->host_cpu_flags == 0xfedcba9876abcdefULL);
            }
        }
    }
}

TEST_CASE("x64 scalar cache retains a mixed raw container while omitting scalar-only homes", "[x64][scalar-cache]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto sum = as.Add(input, ir::Operand{ir::Imm{u64{17}}}).SetType(T::U64);
    Store(as, 64, sum);
    auto raw = as.BitCast(input).SetType(T::V128); Store(as, 80, raw);
    Store(as, 72, input); block->SetTerminal(ir::terminal::ReturnToHost{});
    const auto plan = x64::ScalarRegisterPlan::Build(block.get());
    REQUIRE(plan.Slot(input.Def())); REQUIRE(plan.RequiresCanonicalHome(input.Def()));
    REQUIRE(plan.Slot(sum.Def())); REQUIRE_FALSE(plan.RequiresCanonicalHome(sum.Def()));
    REQUIRE_FALSE(plan.Slot(raw.Def())); REQUIRE(plan.RequiresCanonicalHome(raw.Def()));
    Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
    REQUIRE(off.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites == 4);
    REQUIRE(on.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites == 2);
    REQUIRE(on.translator.Statistics().At(R::CanonicalSSA).vector_store_sites == 1);
    REQUIRE(on.translator.Statistics().scalar_cache_read_sites == 3);
    REQUIRE(on.translator.Statistics().scalar_cache_write_sites == 2);
    for (u64 input_value : {u64{0}, u64{1}, UINT64_MAX, u64{0xfedcba9876543210}})
    for (auto* code : {&off, &on}) {
        Storage storage; storage.Put(0, input_value);
        REQUIRE(code->Run(storage) == HaltReason::CallHost);
        REQUIRE(storage.Get(64) == input_value + 17); REQUIRE(storage.Get(72) == input_value);
        REQUIRE(storage.Get(80) == input_value); REQUIRE(storage.Get(88) == 0);
    }
}

TEST_CASE("x64 scalar cache publishes both home lanes for direct lane readers", "[x64][scalar-cache]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto raw_view = input; raw_view = raw_view.SetCastType(T::V128);
    auto low = as.VecExtract64(raw_view, ir::Imm{u32{0}}).SetType(T::U64);
    auto high = as.VecExtract64(raw_view, ir::Imm{u32{1}}).SetType(T::U64);
    Store(as, 64, low); Store(as, 72, high);
    auto sum = as.Add(input, ir::Operand{ir::Imm{u64{17}}}).SetType(T::U64);
    Store(as, 80, sum); block->SetTerminal(ir::terminal::ReturnToHost{});
    const auto plan = x64::ScalarRegisterPlan::Build(block.get());
    REQUIRE(plan.Slot(input.Def())); REQUIRE(plan.RequiresCanonicalHome(input.Def()));
    REQUIRE(plan.Slot(sum.Def())); REQUIRE_FALSE(plan.RequiresCanonicalHome(sum.Def()));
    REQUIRE_FALSE(plan.Slot(low.Def())); REQUIRE_FALSE(plan.Slot(high.Def()));
    Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
    REQUIRE(off.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites == 8);
    REQUIRE(on.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites == 6);
    REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites == 1);
    REQUIRE(on.translator.Statistics().scalar_cache_read_sites == 1);
    REQUIRE(on.translator.Statistics().scalar_cache_write_sites == 1);
    for (u64 input_value : {u64{0}, u64{1}, UINT64_MAX, u64{0xfedcba9876543210}})
    for (auto* code : {&off, &on}) {
        Storage storage; storage.Put(0, input_value);
        REQUIRE(code->Run(storage) == HaltReason::CallHost);
        REQUIRE(storage.Get(64) == input_value); REQUIRE(storage.Get(72) == 0);
        REQUIRE(storage.Get(80) == input_value + 17);
    }
}

TEST_CASE("x64 scalar cache applies consumer widths and preserves raw container bytes", "[x64][scalar-cache]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto wide = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto w_view = wide; // SetCastType mutates the Value handle; retain the X view.
    auto w = as.Add(w_view.SetCastType(T::U32), ir::Operand{ir::Imm{u32{1}}}).SetType(T::U32);
    auto extended = as.ZeroExtend64(w).SetType(T::U64);
    Store(as, 64, extended); Store(as, 72, wide);
    auto wide_raw = as.BitCast(wide).SetType(T::V128); Store(as, 80, wide_raw);
    auto vector = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::U64);
    auto selected = as.Select(ir::BOOL{w}, vector, vector).SetType(T::U64);
    auto alias = as.GetResult(selected, selected).SetType(T::U64);
    auto rewidened = as.BitCast(alias).SetType(T::V128); Store(as, 112, rewidened);
    Store(as, 128, alias);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
    REQUIRE(on.translator.Statistics().scalar_cache_write_sites == 3);
    for (u64 input : {u64{0}, u64{0xffffffff}, u64{0xabcdef12ffffffff}, UINT64_MAX})
    for (auto* code : {&off, &on}) {
        Storage storage; storage.Put(0, input);
        storage.Put(16, 0x123456789abcdef0ULL); storage.Put(24, 0xfedcba9876543210ULL);
        REQUIRE(code->Run(storage) == HaltReason::CallHost);
        REQUIRE(storage.Get(64) == u64(u32(input) + u32{1})); REQUIRE(storage.Get(72) == input);
        REQUIRE(storage.Get(80) == input); REQUIRE(storage.Get(88) == 0);
        REQUIRE(storage.Get(112) == 0x123456789abcdef0ULL); REQUIRE(storage.Get(120) == 0xfedcba9876543210ULL);
        REQUIRE(storage.Get(128) == 0x123456789abcdef0ULL);
    }
}

TEST_CASE("x64 scalar cache snapshots survive a returning callback which changes uniforms", "[x64][scalar-cache]") {
    for (bool cache : {false, true}) for (u64 value : {u64{0}, u64{19}, UINT64_MAX}) {
        Storage storage; storage.Put(0, value);
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto original = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto pointer = as.LoadImm(ir::Imm{reinterpret_cast<u64>(storage.state)}).SetType(T::U64);
        auto called = as.CallHost(&ClobberCallerSaved, pointer, original, original).SetType(T::U64);
        auto after = as.Add(original, ir::Operand{ir::Imm{u64{11}}}).SetType(T::U64);
        Store(as, 64, original); Store(as, 72, called); Store(as, 80, after);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = x64::ScalarRegisterPlan::Build(block.get());
        REQUIRE(plan.Slot(original.Def())); REQUIRE(plan.RequiresCanonicalHome(original.Def()));
        REQUIRE(plan.Slot(pointer.Def())); REQUIRE(plan.RequiresCanonicalHome(pointer.Def()));
        REQUIRE(plan.Slot(after.Def())); REQUIRE_FALSE(plan.RequiresCanonicalHome(after.Def()));
        Compiled code{block.get(), cache}; REQUIRE(code.Run(storage) == HaltReason::CallHost);
        REQUIRE(storage.Get(0) == 0xaaaa5555bbbb6666ULL); REQUIRE(storage.Get(64) == value);
        REQUIRE(storage.Get(72) == value * 7 + 5); REQUIRE(storage.Get(80) == value + 11);
        REQUIRE(code.translator.Statistics().helper_call_sites == 2);
        REQUIRE(code.translator.Statistics().scalar_cache_read_sites == (cache ? 6 : 0));
        REQUIRE(code.translator.Statistics().At(R::CanonicalSSA).scalar_store_sites == (cache ? 6 : 8));
    }
}

TEST_CASE("x64 scalar cache keeps cross-CFG inputs at home and caches a local result", "[x64][scalar-cache]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto branch = as.Goto(ir::BOOL{input});
    auto ignored = as.Add(input, ir::Operand{ir::Imm{1}}).SetType(T::U64); Store(as, 64, ignored);
    as.BindLabel(branch); Store(as, 72, input);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled off{block.get(), false}, on{block.get(), true}; CheckStores(off, on);
    REQUIRE(on.translator.Statistics().scalar_cache_read_sites == 1);
    REQUIRE(on.translator.Statistics().scalar_cache_write_sites == 1);
    REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites == 0);
    REQUIRE(on.translator.getSize() < off.translator.getSize());
    for (u64 input_value : {u64{0}, u64{1}}) for (auto* code : {&off, &on}) {
        Storage storage; storage.Put(0, input_value);
        REQUIRE(code->Run(storage) == HaltReason::CallHost);
        REQUIRE(storage.Get(64) == (input_value ? 0 : 1)); REQUIRE(storage.Get(72) == input_value);
    }
}

TEST_CASE("x64 scalar cache enables production code without changing canonical guest state", "[x64][scalar-cache]") {
    for (bool cache : {false, true}) {
        backend::AddressSpace space{TestConfig(cache)}; auto module = space.GetDefaultModule();
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto value = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto result = as.Mul(value, ir::Operand{ir::Imm{u64{17}}}).SetType(T::U64);
        Store(as, 64, result); Store(as, 72, value); block->SetTerminal(ir::terminal::ReturnToHost{});
        auto* entry = x64::CompileBlock(module, block.get()); REQUIRE(entry != nullptr);
        for (u64 input : {u64{0}, u64{19}, UINT64_MAX}) {
            Storage storage; storage.Put(0, input);
            REQUIRE(reinterpret_cast<BlockFn>(entry)(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(64) == input * 17); REQUIRE(storage.Get(72) == input);
        }
    }
}

TEST_CASE("x64 scalar cache native faults recover committed state and host frame", "[x64][scalar-cache][fault]") {
    for (bool cache : {false, true}) {
        backend::AddressSpace space{TestConfig(cache)}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
        auto* inaccessible = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        REQUIRE(inaccessible != MAP_FAILED);
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto value = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto next = as.Add(value, ir::Operand{ir::Imm{u64{31}}}).SetType(T::U64);
        Store(as, 64, next); as.AdvancePC(ir::Imm{u32{4}});
        auto address = as.LoadImm(ir::Imm{reinterpret_cast<u64>(inaccessible)}).SetType(T::U64);
        auto loaded = as.LoadMemory(ir::Operand{address}).SetType(T::U64);
        Store(as, 72, loaded); Store(as, 80, value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        REQUIRE(x64::CompileBlock(module, block.get()) != nullptr);
        const u64 input = 0xffffffffffffffefULL;
        std::memcpy(runtime.GetUniformBuffer().data(), &input, 8); runtime.SetLocation(0x1000);
        const auto original = _mm_getcsr(); const auto changed = (original & ~0x6000u) | 0x6000u;
        _mm_setcsr(changed); const auto reason = runtime.Run(); const auto restored = _mm_getcsr(); _mm_setcsr(original);
        REQUIRE(munmap(inaccessible, 4096) == 0);
        REQUIRE(reason == HaltReason::PageFatal); REQUIRE(runtime.GetLocation() == 0x1004);
        REQUIRE(restored == changed);
        u64 before{}, after{}, delayed{};
        std::memcpy(&before, runtime.GetUniformBuffer().data() + 64, 8);
        std::memcpy(&after, runtime.GetUniformBuffer().data() + 72, 8);
        std::memcpy(&delayed, runtime.GetUniformBuffer().data() + 80, 8);
        REQUIRE(before == input + 31); REQUIRE(after == 0); REQUIRE(delayed == 0);
        REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
        REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
        REQUIRE(runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] == 0);
        REQUIRE(backend::riscv64::anonymous_memory_readers.load() == 0);
    }
}

TEST_CASE("x64 scalar cache module overrides take precedence and change published code", "[x64][scalar-cache][features]") {
    for (bool optimization : {false, true})
    for (auto override : {std::optional<bool>{}, std::optional<bool>{false}, std::optional<bool>{true}}) {
        const auto config = TestConfig(optimization);
        backend::AddressSpace space{config};
        auto module_config = space.GetDefaultModule()->GetModuleConfig();
        if (override) module_config.feature_overrides.Set(FeatureId::x64_scalar_cache, *override);
        auto module = space.MapModule(0x1000, 0x2000, module_config);
        const bool expected_cache = override.value_or(optimization);
        REQUIRE(backend::ResolveFeatureSet(module->GetModuleConfig()).x64_scalar_cache == expected_cache);
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto value = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto result = as.Mul(value, ir::Operand{ir::Imm{u64{17}}}).SetType(T::U64);
        Store(as, 64, result); Store(as, 72, value); block->SetTerminal(ir::terminal::ReturnToHost{});
        block->SetEndLocation(ir::Location{0x1001}); block->ReIdInstr();
        const auto host_features = x64::HostFeatures::Detect();
        x64::Translator disabled{block.get(), host_features, &config, nullptr, false}; disabled.Translate();
        x64::Translator enabled{block.get(), host_features, &config, nullptr, true}; enabled.Translate();
        REQUIRE(disabled.Statistics().scalar_cache_read_sites == 0);
        REQUIRE(enabled.Statistics().scalar_cache_read_sites == 3);
        REQUIRE(enabled.Statistics().scalar_cache_write_sites == 2);
        // Establish that the code oracle has two different, observed shapes.
        const bool same_shape = disabled.getSize() == enabled.getSize() &&
            std::memcmp(disabled.getCode(), enabled.getCode(), disabled.getSize()) == 0;
        REQUIRE_FALSE(same_shape);
        auto* entry = x64::CompileBlock(module, block.get()); REQUIRE(entry != nullptr);
        const auto& expected = expected_cache ? enabled : disabled;
        REQUIRE(*block->GetJitCache().cache_size >= expected.getSize());
        REQUIRE(std::memcmp(entry, expected.getCode(), expected.getSize()) == 0);
        for (u64 input : {u64{0}, u64{19}, UINT64_MAX}) {
            Storage storage; storage.Put(0, input);
            REQUIRE(reinterpret_cast<BlockFn>(entry)(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(64) == input * 17); REQUIRE(storage.Get(72) == input);
        }
    }
}

TEST_CASE("x64 scalar cache validity fingerprints include effective mapped module policy", "[x64][scalar-cache][features]") {
    const auto off_config = TestConfig(false), on_config = TestConfig(true);
    backend::ModuleConfig off{.optimizations = Optimizations::None};
    backend::ModuleConfig on{.optimizations = Optimizations::ScalarRegisterCache};
    const auto off_hash = backend::ComputeConfigHash(off_config, off);
    const auto on_hash = backend::ComputeConfigHash(on_config, on);
    REQUIRE(off_hash != on_hash);
    REQUIRE(backend::ComputeConfigHash(off_config, off) == off_hash);
    REQUIRE(backend::ComputeConfigHash(on_config, on) == on_hash);
    auto forced_on = off;
    forced_on.feature_overrides.Set(FeatureId::x64_scalar_cache, true);
    auto forced_off = on;
    forced_off.feature_overrides.Set(FeatureId::x64_scalar_cache, false);
    REQUIRE(backend::ComputeConfigHash(off_config, forced_on) != off_hash);
    REQUIRE(backend::ComputeConfigHash(on_config, forced_off) != on_hash);
    REQUIRE(backend::ResolveFeatureSet(forced_on).x64_scalar_cache);
    REQUIRE_FALSE(backend::ResolveFeatureSet(forced_off).x64_scalar_cache);

    // Both mapped modules already contain a different explicit override. The
    // top-level presence/list length is identical; only effective cache policy
    // can distinguish their per-unit feature hashes and mapped hash aggregate.
    auto mapped_on = on, mapped_off = on;
    mapped_on.feature_overrides.Set(FeatureId::const_cse, false);
    mapped_off.feature_overrides.Set(FeatureId::const_cse, false);
    mapped_off.feature_overrides.Set(FeatureId::x64_scalar_cache, false);
    const auto mapped_on_hash = HashFeatureSet(backend::ResolveFeatureSet(mapped_on));
    const auto mapped_off_hash = HashFeatureSet(backend::ResolveFeatureSet(mapped_off));
    REQUIRE(mapped_on_hash != mapped_off_hash);
    REQUIRE(mapped_on_hash == HashFeatureSet(backend::ResolveFeatureSet(mapped_on)));
    const std::array mapped_enabled{mapped_on_hash}, mapped_disabled{mapped_off_hash};
    REQUIRE(backend::ComputeConfigHash(on_config, on, mapped_enabled) !=
            backend::ComputeConfigHash(on_config, on, mapped_disabled));
    auto explicit_on = mapped_on;
    explicit_on.feature_overrides.Set(FeatureId::x64_scalar_cache, true);
    REQUIRE(HashFeatureSet(backend::ResolveFeatureSet(explicit_on)) == mapped_on_hash);
}
