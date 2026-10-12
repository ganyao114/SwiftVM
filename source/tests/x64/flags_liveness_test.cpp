#include <array>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/x64/flags_liveness.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/include/sruntime.h"
#include "runtime/frontend/arm64/arm64_frontend.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using T = ir::ValueType;
using O = ir::OpCode;
using F = ir::Flags;
namespace {
constexpr u32 extent = 4096;
using BlockFn = HaltReason (*)(backend::State*);
struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    ~Storage() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    u64 Get(u32 offset) const { u64 value; std::memcpy(&value, state->uniform_buffer_begin + offset, 8); return value; }
};
Config NativeConfig(bool cache) {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
            .backend_isa = kX86_64, .uniform_buffer_size = extent, .static_program = true,
            .global_opts = cache ? Optimizations::ScalarRegisterCache : Optimizations::None,
            .stack_alignment = 16};
}
u64 Mask(u32 bits) { return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }
struct Math { u64 result; bool carry, overflow; };
Math Calculate(O op, u64 a, u64 b, u32 bits, bool c) {
    const auto mask = Mask(bits), sign = u64{1} << (bits - 1);
    a &= mask; b &= mask;
    if (op == O::Neg) { b = a; a = 0; }
    const bool subtract = op == O::Sub || op == O::Sbb || op == O::Neg;
    const u64 incoming = op == O::Adc ? u64(c) : op == O::Sbb ? u64(!c) : 0;
    Math value{};
    if (subtract) {
        const auto subtrahend = static_cast<unsigned __int128>(b) + incoming;
        value.result = u64(static_cast<unsigned __int128>(a) - subtrahend) & mask;
        value.carry = static_cast<unsigned __int128>(a) >= subtrahend;
        value.overflow = ((a ^ b) & (a ^ value.result) & sign) != 0;
    } else {
        const auto sum = static_cast<unsigned __int128>(a) + b + incoming;
        value.result = u64(sum) & mask; value.carry = sum > mask;
        value.overflow = ((~(a ^ b)) & (a ^ value.result) & sign) != 0;
    }
    return value;
}
u64 Publish(u64 flags, const Math& value, u32 bits) {
    return (flags & ~(u64{15} << 28)) | (u64(value.result >> (bits - 1)) << 31) |
        (u64(value.result == 0) << 30) | (u64(value.carry) << 29) | (u64(value.overflow) << 28);
}
std::vector<F> Requests(ir::Block* block) {
    std::vector<F> masks;
    for (auto& inst : block->GetInstList())
        if (inst.GetOp() == O::SaveFlags || inst.GetOp() == O::BranchOnlyFlags) masks.push_back(inst.GetArg<F>(1));
    return masks;
}
ir::Value Binary(ir::Assembler& as, O op, ir::Value a, ir::Value b, T type) {
    return op == O::Neg ? as.Neg(a).SetType(type) : ir::Value{as.AppendInst(op, a, ir::Operand{b})}.SetType(type);
}
IntrusivePtr<ir::Block> Chain(T type, O first, O last) {
    IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
    const auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    const auto b = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
    const auto one = Binary(as, first, a, b, type); as.SaveFlags(one, F::NZCV);
    as.StoreUniform(ir::Uniform{32, type}, one); as.AdvancePC(ir::Imm{u32{4}});
    const auto two = Binary(as, O::Adc, one, b, type); as.SaveFlags(two, F::NZCV);
    as.StoreUniform(ir::Uniform{40, type}, two); as.AdvancePC(ir::Imm{u32{4}});
    const auto three = Binary(as, last, two, a, type); as.SaveFlags(three, F::NZCV);
    as.StoreUniform(ir::Uniform{48, type}, three);
    block->SetTerminal(ir::terminal::ReturnToHost{}); return block;
}
u64 ObserveAndFlipCarry(u64 pointer, u64 result, u64) {
    auto* state = reinterpret_cast<backend::State*>(pointer);
    std::memcpy(state->uniform_buffer_begin + 96, &state->host_cpu_flags, 8);
    state->host_cpu_flags ^= u64{1} << 29;
    asm volatile("xor %%r11d, %%r11d\n\tsub $1, %%r11d" ::: "r11", "cc");
    return result;
}
class CodeMemory final : public MemoryInterface {
public:
    explicit CodeMemory(std::vector<u32> instructions) : words(std::move(instructions)) { words.push_back(0xd4200000); }
    bool Read(void* destination, size_t address, size_t size) override {
        if (address < 0x1000 || address - 0x1000 > words.size() * 4 || size > words.size() * 4 - (address - 0x1000)) return false;
        std::memcpy(destination, reinterpret_cast<u8*>(words.data()) + address - 0x1000, size); return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* pointer) override {
        const auto address = reinterpret_cast<size_t>(pointer);
        return address >= 0x1000 && address - 0x1000 < words.size() * 4 ? reinterpret_cast<u8*>(words.data()) + address - 0x1000 : nullptr;
    }
    std::vector<u32> words;
};
}

TEST_CASE("x64 carry liveness narrows only intermediate full arithmetic publications", "[x64][flags-liveness][execution]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (auto first : {O::Add, O::Sub, O::Adc, O::Sbb, O::Neg})
    for (auto last : {O::Adc, O::Sbb}) for (bool cache : {false, true}) {
        auto baseline = Chain(type, first, last), narrowed = Chain(type, first, last);
        REQUIRE(Requests(baseline.get()) == std::vector<F>{F::NZCV, F::NZCV, F::NZCV});
        const auto stats = x64::NarrowCarryChainFlags(narrowed.get(), extent);
        REQUIRE(stats.full_publications == 3); REQUIRE(stats.narrowed_to_carry == 2); REQUIRE(stats.barriers == 0);
        REQUIRE(Requests(narrowed.get()) == std::vector<F>{F::Carry, F::Carry, F::NZCV});
        const auto config = NativeConfig(cache);
        x64::Translator reference{baseline.get(), {}, &config, nullptr, cache}, optimized{narrowed.get(), {}, &config, nullptr, cache};
        reference.Translate(); optimized.Translate();
        REQUIRE(optimized.Statistics().flags_materialization_ir_sites == 3);
        REQUIRE(optimized.Statistics().flags_snapshot_sites == 3);
        REQUIRE(optimized.getSize() < reference.getSize());
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const auto mask = Mask(bits), sign = u64{1} << (bits - 1);
        for (auto pair : {std::array<u64, 2>{0, 0}, std::array<u64, 2>{mask, 1},
                          std::array<u64, 2>{sign - 1, 1}, std::array<u64, 2>{sign, mask},
                          std::array<u64, 2>{sign, 1}, std::array<u64, 2>{sign - 1, mask}})
        for (bool carry : {false, true}) for (auto* code : {&reference, &optimized}) {
            Storage storage; storage.Put(0, pair[0]); storage.Put(8, pair[1]);
            const u64 flags = (0xfedcba9876abcdefULL & ~(u64{1} << 29)) | (u64(carry) << 29);
            storage.state->host_cpu_flags = flags;
            const auto one = Calculate(first, pair[0], pair[1], bits, carry);
            const auto two = Calculate(O::Adc, one.result, pair[1], bits, one.carry);
            const auto three = Calculate(last, two.result, pair[0], bits, two.carry);
            REQUIRE(code->getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(32) == one.result); REQUIRE(storage.Get(40) == two.result); REQUIRE(storage.Get(48) == three.result);
            REQUIRE(storage.state->host_cpu_flags == Publish(flags, three, bits));
        }
    }
}

TEST_CASE("x64 carry liveness preserves full flags at helper observation and honors changed C", "[x64][flags-liveness][execution]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool cache : {false, true}) {
        Storage storage;
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        const auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        const auto b = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
        const auto one = Binary(as, O::Add, a, b, type); as.SaveFlags(one, F::NZCV);
        const auto pointer = as.LoadImm(ir::Imm{reinterpret_cast<u64>(storage.state)}).SetType(T::U64);
        (void)as.CallHost(&ObserveAndFlipCarry, pointer, one, one).SetType(T::U64);
        const auto two = Binary(as, O::Adc, one, b, type); as.SaveFlags(two, F::NZCV);
        const auto three = Binary(as, O::Sbb, two, a, type); as.SaveFlags(three, F::NZCV);
        as.StoreUniform(ir::Uniform{48, type}, three); block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto stats = x64::NarrowCarryChainFlags(block.get(), extent);
        REQUIRE(stats.full_publications == 3); REQUIRE(stats.narrowed_to_carry == 1); REQUIRE(stats.barriers == 1);
        REQUIRE(Requests(block.get()) == std::vector<F>{F::NZCV, F::Carry, F::NZCV});
        const auto config = NativeConfig(cache); x64::Translator translator{block.get(), {}, &config, nullptr, cache}; translator.Translate();
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const auto mask = Mask(bits), sign = u64{1} << (bits - 1);
        for (auto pair : {std::array<u64, 2>{0, 0}, std::array<u64, 2>{mask, 1},
                          std::array<u64, 2>{sign - 1, 1}, std::array<u64, 2>{sign, mask}})
        for (bool carry : {false, true}) {
            storage.Put(0, pair[0]); storage.Put(8, pair[1]); storage.Put(48, 0); storage.Put(96, 0);
            const auto flags = (0x123456789abcdef0ULL & ~(u64{1} << 29)) | (u64(carry) << 29);
            storage.state->host_cpu_flags = flags; storage.state->halt_reason = HaltReason::None;
            const auto one_math = Calculate(O::Add, pair[0], pair[1], bits, carry);
            const auto two_math = Calculate(O::Adc, one_math.result, pair[1], bits, !one_math.carry);
            const auto three_math = Calculate(O::Sbb, two_math.result, pair[0], bits, two_math.carry);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(96) == Publish(flags, one_math, bits)); REQUIRE(storage.Get(48) == three_math.result);
            REQUIRE(storage.state->host_cpu_flags == Publish(flags, three_math, bits));
        }
    }
}

TEST_CASE("x64 carry liveness retains precise full flags before a real native memory fault", "[x64][flags-liveness][fault]") {
    for (bool cache : {false, true}) {
        backend::AddressSpace space{NativeConfig(cache)}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
        auto* inaccessible = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        REQUIRE(inaccessible != MAP_FAILED);
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        const auto a = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        const auto one = as.Add(a, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64); as.SaveFlags(one, F::NZCV);
        as.StoreUniform(ir::Uniform{32, T::U64}, one); as.AdvancePC(ir::Imm{u32{4}});
        const auto address = as.LoadImm(ir::Imm{reinterpret_cast<u64>(inaccessible)}).SetType(T::U64);
        const auto loaded = as.LoadMemory(ir::Operand{address}).SetType(T::U64); as.StoreUniform(ir::Uniform{40, T::U64}, loaded);
        const auto two = as.Adc(one, ir::Operand{a}).SetType(T::U64); as.SaveFlags(two, F::NZCV);
        const auto three = as.Sbb(two, ir::Operand{a}).SetType(T::U64); as.SaveFlags(three, F::NZCV);
        as.StoreUniform(ir::Uniform{48, T::U64}, three); block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto stats = x64::NarrowCarryChainFlags(block.get(), extent);
        REQUIRE(stats.full_publications == 3); REQUIRE(stats.narrowed_to_carry == 1); REQUIRE(stats.barriers == 1);
        REQUIRE(Requests(block.get()) == std::vector<F>{F::NZCV, F::Carry, F::NZCV});
        REQUIRE(x64::CompileBlock(module, block.get()) != nullptr);
        const u64 input = 0x7fffffffffffffffULL, flags = 0x123456787abcdef0ULL;
        std::memcpy(runtime.GetUniformBuffer().data(), &input, 8);
        runtime.GetState()->host_cpu_flags = flags;
        // Let the actual native PROT_NONE access fault, rather than rejecting
        // its host address in the preliminary guest-range validation.
        runtime.GetState()->guest_addr_limit = UINT64_MAX; runtime.GetState()->guest_addr_mask = UINT64_MAX;
        runtime.SetLocation(0x1000); const auto reason = runtime.Run();
        REQUIRE(munmap(inaccessible, 4096) == 0);
        REQUIRE(reason == HaltReason::PageFatal); REQUIRE(runtime.GetLocation() == 0x1004);
        REQUIRE(runtime.GetState()->host_cpu_flags == Publish(flags, Calculate(O::Add, input, 1, 64, false), 64));
        u64 committed{}, skipped_load{}, skipped_tail{};
        std::memcpy(&committed, runtime.GetUniformBuffer().data() + 32, 8);
        std::memcpy(&skipped_load, runtime.GetUniformBuffer().data() + 40, 8);
        std::memcpy(&skipped_tail, runtime.GetUniformBuffer().data() + 48, 8);
        REQUIRE(committed == input + 1); REQUIRE(skipped_load == 0); REQUIRE(skipped_tail == 0);
        REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
        REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
    }
}

TEST_CASE("x64 carry liveness treats partial flags readers mutations opaque IR and CFG as barriers", "[x64][flags-liveness][analysis]") {
    for (u32 barrier = 0; barrier < 9; ++barrier) {
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        const auto a = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        const auto one = as.Add(a, ir::Operand{a}).SetType(T::U64); as.SaveFlags(one, F::NZCV);
        switch (barrier) {
            case 0: as.SaveFlags(one, F::Negate | F::Zero); break;
            case 1: as.BranchOnlyFlags(one, F::NZCV); break;
            case 2: (void)as.TestFlags(F::Carry).SetType(T::U64); break;
            case 3: as.InvertCarry(); break;
            case 4: (void)as.GetLocation().SetType(T::U64); break;
            case 5: {
                // This producer cannot publish V. Treating its requested mask
                // as an actual NZV overwrite would incorrectly narrow 'one'.
                const auto literal = as.LoadImm(ir::Imm{u64{0}}).SetType(T::U64);
                as.SaveFlags(literal, F::Negate | F::Zero | F::Overflow); break;
            }
            case 6: (void)as.GetFlags(one, F::NZCV).SetType(T::U64); break;
            case 7: as.SetOverflow(a); break;
            case 8: { const auto branch = as.Goto(a); as.BindLabel(branch); break; }
        }
        const auto two = as.Adc(one, ir::Operand{a}).SetType(T::U64); as.SaveFlags(two, F::NZCV);
        block->SetTerminal(ir::terminal::ReturnToHost{}); const auto original = Requests(block.get());
        const auto stats = x64::NarrowCarryChainFlags(block.get(), extent);
        INFO("barrier=" << barrier); REQUIRE(Requests(block.get()) == original);
        REQUIRE(stats.narrowed_to_carry == 0); REQUIRE(stats.barriers == 1);
        REQUIRE(stats.full_publications == (barrier == 8 ? 0 : 2));
    }
}

TEST_CASE("x64 carry liveness validates real uniform access widths and leaves unsafe fixtures unexecuted", "[x64][flags-liveness][analysis]") {
    for (u32 shape = 0; shape < 7; ++shape) {
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        const auto a = as.LoadImm(ir::Imm{u64{1}}).SetType(T::U64);
        const auto one = as.Add(a, ir::Operand{a}).SetType(T::U64); as.SaveFlags(one, F::NZCV);
        if (shape <= 3) {
            const u32 offset = shape <= 1 ? 248 : shape == 2 ? 249 : UINT32_MAX;
            (void)as.LoadUniform(ir::Uniform{offset, T::U64}).SetType(T::U64);
        } else if (shape == 4) {
            // The descriptor looks like one byte, but the real store is 8B.
            as.StoreUniform(ir::Uniform{252, T::U8}, one);
        } else if (shape == 5) {
            auto narrow = one; narrow = narrow.SetCastType(T::U8);
            as.StoreUniform(ir::Uniform{255, T::U64}, narrow); // Real 1B extent fits.
        } else {
            // Conversely, annotating an 8B load with a narrow SSA result
            // does not make its descriptor's cross-boundary access safe.
            (void)as.LoadUniform(ir::Uniform{252, T::U64}).SetType(T::U8);
        }
        const auto two = as.Adc(one, ir::Operand{a}).SetType(T::U64); as.SaveFlags(two, F::NZCV);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const bool safe = shape == 1 || shape == 5;
        const auto stats = x64::NarrowCarryChainFlags(block.get(), shape == 0 ? 0 : 256);
        INFO("uniform shape=" << shape); REQUIRE(stats.full_publications == 2);
        REQUIRE(stats.narrowed_to_carry == u32(safe)); REQUIRE(stats.barriers == u32(!safe));
        REQUIRE(Requests(block.get()) == std::vector<F>{safe ? F::Carry : F::NZCV, F::NZCV});
    }
}

TEST_CASE("x64 carry liveness handles repeated publications from the same producer", "[x64][flags-liveness][execution]") {
    for (bool cache : {false, true}) {
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        const auto a = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        const auto one = as.Add(a, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        as.SaveFlags(one, F::NZCV); as.SaveFlags(one, F::NZCV);
        const auto two = as.Adc(one, ir::Operand{a}).SetType(T::U64); as.SaveFlags(two, F::NZCV);
        as.StoreUniform(ir::Uniform{48, T::U64}, two); block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto stats = x64::NarrowCarryChainFlags(block.get(), extent);
        REQUIRE(stats.full_publications == 3); REQUIRE(stats.narrowed_to_carry == 1); REQUIRE(stats.barriers == 0);
        REQUIRE(Requests(block.get()) == std::vector<F>{F::NZCV, F::Carry, F::NZCV});
        const auto config = NativeConfig(cache); x64::Translator translator{block.get(), {}, &config, nullptr, cache}; translator.Translate();
        REQUIRE(translator.Statistics().flags_snapshot_sites == 2);
        for (u64 a_value : {u64{0}, UINT64_MAX, u64{0x7fffffffffffffff}, u64{0x8000000000000000}}) {
            Storage storage; storage.Put(0, a_value); const u64 flags = 0xfedcba9876abcdefULL; storage.state->host_cpu_flags = flags;
            const auto one_math = Calculate(O::Add, a_value, 1, 64, false);
            const auto two_math = Calculate(O::Adc, one_math.result, a_value, 64, one_math.carry);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(48) == two_math.result); REQUIRE(storage.state->host_cpu_flags == Publish(flags, two_math, 64));
        }
    }
}

TEST_CASE("x64 carry liveness tracks delayed ADC publication at its producer and remains safe on rerun", "[x64][flags-liveness][execution]") {
    for (bool cache : {false, true}) for (bool rerun : {false, true}) {
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        const auto a = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        const auto one = as.Add(a, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64); as.SaveFlags(one, F::NZCV);
        // ADC consumes one's C here; its flags publication happens later.
        const auto delayed = as.Adc(one, ir::Operand{a}).SetType(T::U64);
        const auto intervening = as.Sub(a, ir::Operand{a}).SetType(T::U64); as.SaveFlags(intervening, F::NZCV);
        as.SaveFlags(delayed, F::NZCV);
        const auto last = as.Sbb(delayed, ir::Operand{a}).SetType(T::U64); as.SaveFlags(last, F::NZCV);
        as.StoreUniform(ir::Uniform{32, T::U64}, delayed); as.StoreUniform(ir::Uniform{48, T::U64}, last);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto stats = x64::NarrowCarryChainFlags(block.get(), extent);
        REQUIRE(stats.full_publications == 4); REQUIRE(stats.narrowed_to_carry == 2); REQUIRE(stats.barriers == 0);
        const std::vector<F> wanted{F::Carry, F::NZCV, F::Carry, F::NZCV};
        REQUIRE(Requests(block.get()) == wanted);
        if (rerun) {
            const auto repeated = x64::NarrowCarryChainFlags(block.get(), extent);
            REQUIRE(repeated.full_publications == 2); REQUIRE(repeated.narrowed_to_carry == 0);
            REQUIRE(repeated.barriers == 2); REQUIRE(Requests(block.get()) == wanted);
        }
        const auto config = NativeConfig(cache); x64::Translator translator{block.get(), {}, &config, nullptr, cache}; translator.Translate();
        REQUIRE(translator.Statistics().flags_snapshot_sites == 4);
        for (u64 input : {u64{0}, UINT64_MAX, u64{0x7fffffffffffffff}, u64{0x8000000000000000}})
        for (bool carry : {false, true}) {
            Storage storage; storage.Put(0, input);
            const auto flags = (0xfedcba9876abcdefULL & ~(u64{1} << 29)) | (u64(carry) << 29); storage.state->host_cpu_flags = flags;
            const auto one_math = Calculate(O::Add, input, 1, 64, carry);
            const auto delayed_math = Calculate(O::Adc, one_math.result, input, 64, one_math.carry);
            const auto last_math = Calculate(O::Sbb, delayed_math.result, input, 64, delayed_math.carry);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(32) == delayed_math.result); REQUIRE(storage.Get(48) == last_math.result);
            REQUIRE(storage.state->host_cpu_flags == Publish(flags, last_math, 64));
        }
    }
}

TEST_CASE("x64 production FlagElimination gate applies carry narrowing during CompileBlock", "[x64][flags-liveness][production]") {
    for (bool enabled : {false, true}) for (bool cache : {false, true}) {
        auto config = NativeConfig(cache);
        if (enabled) config.global_opts |= Optimizations::FlagElimination;
        backend::AddressSpace space{config}; auto module = space.GetDefaultModule();
        auto block = Chain(T::U64, O::Sub, O::Sbb);
        REQUIRE(Requests(block.get()) == std::vector<F>{F::NZCV, F::NZCV, F::NZCV});
        auto* code = x64::CompileBlock(module, block.get()); REQUIRE(code != nullptr);
        REQUIRE(Requests(block.get()) == std::vector<F>{enabled ? F::Carry : F::NZCV, enabled ? F::Carry : F::NZCV, F::NZCV});
        for (auto pair : {std::array<u64, 2>{0, 1}, std::array<u64, 2>{UINT64_MAX, 1},
                          std::array<u64, 2>{0x8000000000000000ULL, 1}})
        for (bool carry : {false, true}) {
            Storage storage; storage.Put(0, pair[0]); storage.Put(8, pair[1]);
            const auto flags = (0x123456787abcdef0ULL & ~(u64{1} << 29)) | (u64(carry) << 29); storage.state->host_cpu_flags = flags;
            const auto one = Calculate(O::Sub, pair[0], pair[1], 64, carry);
            const auto two = Calculate(O::Adc, one.result, pair[1], 64, one.carry);
            const auto three = Calculate(O::Sbb, two.result, pair[0], 64, two.carry);
            REQUIRE(reinterpret_cast<BlockFn>(code)(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(32) == one.result); REQUIRE(storage.Get(40) == two.result); REQUIRE(storage.Get(48) == three.result);
            REQUIRE(storage.state->host_cpu_flags == Publish(flags, three, 64));
        }
    }
}

TEST_CASE("x64 carry liveness triggers on decoded ARM64 ADCS SBCS chains", "[x64][flags-liveness][arm64][execution]") {
    using namespace vixl::aarch64;
    for (bool wide : {false, true}) for (bool cache : {false, true}) {
        CodeMemory memory{{u32(wide ? ADCS_x : ADCS_w) | (2u << 16) | (1u << 5),
                           u32(wide ? SBCS_x : SBCS_w) | (2u << 16) | 3u,
                           u32(wide ? ADCS_x : ADCS_w) | (1u << 16) | (3u << 5) | 4u}};
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        swift::arm64::A64Decoder{0x1000, &memory, &as}.Decode();
        REQUIRE(Requests(block.get()) == std::vector<F>{F::NZCV, F::NZCV, F::NZCV});
        const auto stats = x64::NarrowCarryChainFlags(block.get(), sizeof(swift::arm64::ThreadContext64));
        REQUIRE(stats.full_publications == 3); REQUIRE(stats.narrowed_to_carry == 2);
        REQUIRE(Requests(block.get()) == std::vector<F>{F::Carry, F::Carry, F::NZCV});
        const auto config = NativeConfig(cache); x64::Translator translator{block.get(), {}, &config, nullptr, cache}; translator.Translate();
        const u32 bits = wide ? 64 : 32; const auto mask = Mask(bits), sign = u64{1} << (bits - 1);
        for (auto pair : {std::array<u64, 2>{mask, 1}, std::array<u64, 2>{sign - 1, 1}, std::array<u64, 2>{sign, mask}})
        for (bool carry : {false, true}) {
            Storage storage; auto& context = *reinterpret_cast<swift::arm64::ThreadContext64*>(storage.state->uniform_buffer_begin);
            context.r[1] = pair[0] | (wide ? 0 : 0xabcdef0100000000ULL);
            context.r[2] = pair[1] | (wide ? 0 : 0xfedcba9800000000ULL);
            const auto flags = (0x123456787abcdef0ULL & ~(u64{1} << 29)) | (u64(carry) << 29); storage.state->host_cpu_flags = flags;
            const auto one = Calculate(O::Adc, pair[0], pair[1], bits, carry);
            const auto two = Calculate(O::Sbb, one.result, pair[1], bits, one.carry);
            const auto three = Calculate(O::Adc, two.result, pair[0], bits, two.carry);
            REQUIRE(translator.getCode<BlockFn>()(storage.state) == HaltReason::CallHost);
            REQUIRE(context.r[0] == one.result); REQUIRE(context.r[3] == two.result); REQUIRE(context.r[4] == three.result);
            REQUIRE(storage.state->host_cpu_flags == Publish(flags, three, bits));
            REQUIRE(context.padding == u32(swift::arm64::InterruptReason::BRK));
        }
    }
}
