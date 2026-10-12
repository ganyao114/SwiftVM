#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <utility>
#include <vector>
#include <sys/mman.h>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/common/host_pair_result.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using T = ir::ValueType;
using F = ir::Flags;
using Entry = HaltReason (*)(backend::State*);
constexpr u64 known_flags = (u64{15} << 28) | (u64{1} << 26) | 255;
constexpr u64 initial_flags = 0xabcde987f7654321ULL;
constexpr u32 extent = 256;
struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    Storage() {
        for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 0xa5);
        state->host_cpu_flags = initial_flags;
    }
    ~Storage() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    u64 Get(u32 offset) const { u64 value; std::memcpy(&value, state->uniform_buffer_begin + offset, 8); return value; }
};
IntrusivePtr<ir::Block> Block() { return new ir::Block(ir::Location{0x1000}); }
void Store(ir::Assembler& as, u32 offset, ir::Value value) {
    // These observation slots contain complete u64 snapshots. StoreUniform
    // follows the input view, including BOOL's fixed U8 cast; the Uniform
    // descriptor alone cannot widen a narrow result. Preserve the producer's
    // arithmetic width and explicitly read its canonical zero-extended word.
    as.StoreUniform(ir::Uniform{offset, T::U64}, value.SetCastType(T::U64));
}
ir::Value Imm(ir::Assembler& as, u64 value, T type = T::U64) { return as.LoadImm(ir::Imm{value}).SetType(type); }
Config Configuration(bool cache) {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
            .backend_isa = kX86_64, .uniform_buffer_size = extent, .static_program = true,
            .global_opts = cache ? Optimizations::ScalarRegisterCache : Optimizations::None,
            .stack_alignment = 16};
}
u64 Arithmetic(u64 prior, bool subtract, u64 lhs, u64 rhs, u32 bits, u64& result) {
    const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
    const auto sign = u64{1} << (bits - 1); lhs &= mask; rhs &= mask;
    const unsigned __int128 a = lhs, b = rhs, carry = (prior >> 29) & 1, extra = subtract ? !carry : carry;
    const auto full = subtract ? a - b - extra : a + b + extra; result = u64(full) & mask;
    const auto signed_value = [=](u64 value) -> __int128 { return value & sign ? __int128(value) - (__int128{1} << bits) : __int128(value); };
    const auto total = subtract ? signed_value(lhs) - signed_value(rhs) - __int128(extra) : signed_value(lhs) + signed_value(rhs) + __int128(extra);
    const u64 flags = (u64(bool(result & sign)) << 31) | (u64(result == 0) << 30) |
        (u64(subtract ? a >= b + extra : (full >> bits) != 0) << 29) |
        (u64(total < -(__int128{1} << (bits - 1)) || total >= (__int128{1} << (bits - 1))) << 28) |
        (u64(bool((lhs ^ rhs ^ result) & 16)) << 26) | (result & 255);
    return (prior & ~known_flags) | flags;
}
bool Condition(u32 code, u64 flags) {
    const bool n = flags & (u64{1} << 31), z = flags & (u64{1} << 30), c = flags & (u64{1} << 29), v = flags & (u64{1} << 28);
    const std::array tests{z, c, n, v, c && !z, n == v, !z && n == v, true};
    return code % 2 == 0 || code >= 14 ? tests[code / 2] : !tests[code / 2];
}
void Run(x64::Translator& translator, Storage& storage, HaltReason wanted = HaltReason::CallHost) {
    const auto host_fp = _mm_getcsr();
    REQUIRE(translator.getCode<Entry>()(storage.state) == wanted);
    REQUIRE(_mm_getcsr() == host_fp);
    REQUIRE(storage.state->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
    REQUIRE(storage.state->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
}
struct HelperRequest {
    backend::State* state{};
    u64 expected{}, observed{}, replacement{};
    unsigned calls{};
    bool signal{}, fault{};
};
constexpr u64 helper_result = 0x8123456789abcdefULL;
u64 MutateFlags(u64 pointer, u64, u64) {
    auto& request = *reinterpret_cast<HelperRequest*>(pointer);
    ++request.calls; request.observed = request.state->host_cpu_flags;
    request.state->host_cpu_flags = request.replacement;
    if (request.signal) request.state->exit_request |= u64{1} << 63;
    if (request.fault) request.state->halt_reason = HaltReason::PageFatal;
    asm volatile("xor %%r11d, %%r11d\n\tsub $1, %%r11d" ::: "r11", "cc");
    return helper_result;
}
HostPairResult PairHelper(u64 pointer, u64, u64) {
    auto& request = *reinterpret_cast<HelperRequest*>(pointer);
    (void)MutateFlags(pointer, 0, 0);
    return {helper_result, request.replacement};
}
__attribute__((noinline)) u64 AbandonWithTemporaryRbx(u64 pointer, u64 address, u64) {
    auto& request = *reinterpret_cast<HelperRequest*>(pointer);
    ++request.calls; request.observed = request.state->host_cpu_flags;
    request.state->host_cpu_flags = request.replacement;
    // A returning SysV helper restores RBX. A fault abandons this temporary
    // value, so recovery must reload the committed packed word from State.
    asm volatile("mov %%rbx, %%r11\n\t"
                 "movabs $0xdeadbeef98765432, %%rbx\n\t"
                 "mov (%%rsi), %%rax\n\t"
                 "mov %%r11, %%rbx"
                 : : "S"(address) : "rax", "r11", "memory");
    return helper_result;
}
std::vector<F> PublicationRequests() {
    constexpr std::array bits{F::Negate, F::Zero, F::Carry, F::Overflow, F::AuxiliaryCarry, F::Parity};
    std::vector<F> result;
    for (u32 subset = 0; subset < 64; ++subset) {
        auto request = F::None; for (u32 bit = 0; bit < bits.size(); ++bit) if (subset & (u32{1} << bit)) request |= bits[bit];
        result.push_back(request);
    }
    const auto unknown = static_cast<F>(u64{1} << 61);
    for (auto request : {F::Positive, unknown, F::Positive | unknown, F::All | F::Positive | unknown}) result.push_back(request);
    return result;
}
u64 PublicationMask(F requested) {
    u64 mask{};
    for (auto bit : std::array<std::pair<F, u32>, 5>{{{F::Negate, 31}, {F::Zero, 30}, {F::Carry, 29}, {F::Overflow, 28}, {F::AuxiliaryCarry, 26}}})
        if (True(requested & bit.first)) mask |= u64{1} << bit.second;
    if (True(requested & F::Parity)) mask |= 255;
    return mask;
}
u64 ClearModel(u64 prior, F requested) {
    return (prior & ~PublicationMask(requested)) | (True(requested & F::Parity) ? 1 : 0);
}
u64 ScalarView(u64 value, T actual, T view) {
    for (auto type : {actual, view}) { const u32 bits = ir::GetValueSizeByte(type) * 8; if (bits < 64) value &= (u64{1} << bits) - 1; }
    return value;
}
void CompleteEqual(const Storage& a, const Storage& b) { REQUIRE(std::memcmp(a.bytes.data(), b.bytes.data(), a.bytes.size()) == 0); }
} // namespace

TEST_CASE("resident direct clearing preserves full packed state for every meaningful subset and ignored requests", "[x64][flags][residency][direct][clear]") {
    for (bool cfg : {false, true}) for (auto requested : PublicationRequests()) {
        auto block = Block(); ir::Assembler as{block.get()}; for (u32 i = 0; i < 4; ++i) as.ClearFlags(requested);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator ordinary{block.get(), {}, nullptr, nullptr, true, x64::ExecutionMode::Ordinary, cfg, false}; ordinary.Translate();
        x64::Translator resident{block.get(), {}, nullptr, nullptr, true, x64::ExecutionMode::Ordinary, cfg, true}; resident.Translate();
        const bool effect = PublicationMask(requested) != 0;
        REQUIRE(resident.Statistics().flags_resident_update_sites == (effect ? 4 : 0));
        REQUIRE(ordinary.Statistics().flags_resident_update_sites == 0);
        REQUIRE(resident.Statistics().flags_resident_read_sites == 0); REQUIRE(resident.Statistics().flags_resident_write_sites == 0);
        REQUIRE(resident.Statistics().flags_resident_merge_sites == 0);
        if (effect) {
            REQUIRE(resident.Statistics().At(x64::TransferRegion::StateFlags).scalar_load_sites < ordinary.Statistics().At(x64::TransferRegion::StateFlags).scalar_load_sites);
            REQUIRE(resident.Statistics().At(x64::TransferRegion::StateFlags).scalar_store_sites < ordinary.Statistics().At(x64::TransferRegion::StateFlags).scalar_store_sites);
        }
        for (u64 prior : {initial_flags, UINT64_MAX, u64{0x8000000000000000}, u64{0xfedcba9876543210}}) {
            CAPTURE(cfg, requested, prior); Storage baseline, actual; baseline.state->host_cpu_flags = prior; actual.state->host_cpu_flags = prior;
            std::array<u8, extent> uniforms{}; std::memcpy(uniforms.data(), actual.state->uniform_buffer_begin, extent);
            Run(ordinary, baseline); Run(resident, actual); CompleteEqual(baseline, actual);
            REQUIRE(actual.state->host_cpu_flags == ClearModel(prior, requested));
            REQUIRE(std::memcmp(actual.state->uniform_buffer_begin, uniforms.data(), extent) == 0);
        }
    }
}

TEST_CASE("resident direct bit updates survive value casts conditional joins and helper replacement", "[x64][flags][residency][direct][cfg][helper]") {
    constexpr std::array widths{T::U8, T::U16, T::U32, T::U64};
    for (bool cfg : {false, true}) for (u32 width = 0; width < widths.size(); ++width) {
        const auto actual_type = widths[width], view = widths[widths.size() - 1 - width];
        backend::FunctionPlan plan; plan.body = Block(); ir::Assembler as{plan.body.get()};
        const auto source = as.LoadUniform(ir::Uniform{8, actual_type}).SetType(actual_type);
        const auto input = ir::Value{source.Def()}.SetCastType(view);
        as.SetCarry(input); as.SetOverflow(input); as.InvertCarry();
        const auto selector = as.LoadUniform(ir::Uniform{16, T::U8}).SetType(T::U8); auto skip = as.Goto(ir::BOOL{selector});
        as.InvertCarry(); as.BindLabel(skip);
        const auto pointer = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
        const auto returned = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&MutateFlags)}}, pointer, pointer, pointer).SetType(T::U64); Store(as, 40, returned);
        as.SetCarry(input); as.SetOverflow(input); as.InvertCarry();
        Store(as, 48, as.GetFlags(returned, F::NZCV).SetType(T::U64)); plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator ordinary{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, cfg, false}; ordinary.Translate();
        x64::Translator resident{plan.body.get(), {}, nullptr, &plan, true, x64::ExecutionMode::Ordinary, cfg, true}; resident.Translate();
        REQUIRE(resident.Statistics().flags_resident_update_sites == 7); REQUIRE(ordinary.Statistics().flags_resident_update_sites == 0);
        REQUIRE(resident.Statistics().flags_resident_read_sites == 1); REQUIRE(resident.Statistics().flags_resident_write_sites == 0);
        REQUIRE(resident.Statistics().flags_state_load_sites > 0); REQUIRE(resident.Statistics().flags_state_store_sites > 0);
        REQUIRE(resident.Statistics().At(x64::TransferRegion::StateFlags).scalar_load_sites < ordinary.Statistics().At(x64::TransferRegion::StateFlags).scalar_load_sites);
        REQUIRE(resident.Statistics().At(x64::TransferRegion::StateFlags).scalar_store_sites < ordinary.Statistics().At(x64::TransferRegion::StateFlags).scalar_store_sites);
        for (u64 prior : {initial_flags, UINT64_MAX, u64{0x8000000000000000}})
        for (u64 value : {u64{0}, u64{1}, u64{2}, u64{0x8123456789abcdef}}) for (u64 selected : {u64{0}, u64{1}}) {
            CAPTURE(cfg, width, prior, value, selected); Storage baseline, actual;
            const u64 bit = ScalarView(value, actual_type, view) & 1, replacement = prior ^ 0xdeadbeef7314a982ULL;
            const u64 bit_mask = (u64{1} << 29) | (u64{1} << 28);
            const u64 before_call = (prior & ~bit_mask) | ((selected ? bit ^ 1 : bit) << 29) | (bit << 28);
            const u64 expected = (replacement & ~bit_mask) | ((bit ^ 1) << 29) | (bit << 28);
            HelperRequest base_request{.state = baseline.state, .replacement = replacement}, request{.state = actual.state, .replacement = replacement};
            for (auto* storage : {&baseline, &actual}) { storage->state->host_cpu_flags = prior; storage->Put(8, value); storage->Put(16, selected); }
            baseline.Put(24, reinterpret_cast<u64>(&base_request)); actual.Put(24, reinterpret_cast<u64>(&request));
            Run(ordinary, baseline); Run(resident, actual);
            REQUIRE(base_request.calls == 1); REQUIRE(request.calls == 1); REQUIRE(base_request.observed == before_call); REQUIRE(request.observed == before_call);
            baseline.Put(24, 0); actual.Put(24, 0); CompleteEqual(baseline, actual);
            REQUIRE(actual.Get(40) == helper_result); REQUIRE(actual.Get(48) == expected); REQUIRE(actual.state->host_cpu_flags == expected);
        }
    }
}

TEST_CASE("resident SSE string flag merges preserve untouched packed bits for every request subset", "[x64][flags][residency][direct][sse]") {
    constexpr std::array widths{T::U8, T::U16, T::U32, T::U64};
    u32 index{};
    for (auto requested : PublicationRequests()) {
        const auto view = widths[index++ % widths.size()];
        for (bool cfg : {false, true}) {
            auto block = Block(); ir::Assembler as{block.get()}; const auto packed = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
            as.PublishSse42StrFlags(ir::Value{packed.Def()}.SetCastType(view), requested); block->SetTerminal(ir::terminal::ReturnToHost{});
            x64::Translator ordinary{block.get(), {}, nullptr, nullptr, true, x64::ExecutionMode::Ordinary, cfg, false}; ordinary.Translate();
            x64::Translator resident{block.get(), {}, nullptr, nullptr, true, x64::ExecutionMode::Ordinary, cfg, true}; resident.Translate();
            const u64 mask = PublicationMask(requested);
            REQUIRE(resident.Statistics().flags_resident_update_sites == u64(mask != 0)); REQUIRE(ordinary.Statistics().flags_resident_update_sites == 0);
            REQUIRE(resident.Statistics().flags_resident_read_sites == 0); REQUIRE(resident.Statistics().flags_resident_write_sites == 0);
            for (u64 prior : {initial_flags, UINT64_MAX, u64{0x8000000000000000}}) for (u64 nibble = 0; nibble < 16; ++nibble) {
                CAPTURE(cfg, requested, view, prior, nibble); Storage baseline, actual;
                const u64 input = (nibble << 24) | 0xfedcba9800abcdefULL;
                for (auto* storage : {&baseline, &actual}) { storage->state->host_cpu_flags = prior; storage->Put(0, input); }
                const u64 value = ScalarView(input, T::U64, view), sign = (value >> 24) & 1, zero = (value >> 25) & 1;
                const u64 carry = (value >> 26) & 1, overflow = (value >> 27) & 1;
                const u64 packed_result = (sign << 31) | (zero << 30) | (carry << 29) | (overflow << 28) | 1;
                const u64 expected = (prior & ~mask) | (packed_result & mask);
                Run(ordinary, baseline); Run(resident, actual); CompleteEqual(baseline, actual); REQUIRE(actual.state->host_cpu_flags == expected);
            }
        }
    }
}

TEST_CASE("resident FCmp merges use full compact truth values and source access widths", "[x64][flags][residency][direct][fcmp]") {
    constexpr std::array widths{T::U8, T::U16, T::U32, T::U64};
    for (bool cfg : {false, true}) for (u32 width = 0; width < widths.size(); ++width)
    for (u64 compact : {u64{0}, u64{1}, u64{2}, u64{0x100000000}, UINT64_MAX}) {
        const auto actual_type = widths[width], view = widths[widths.size() - 1 - width];
        auto block = Block(); ir::Assembler as{block.get()}; const auto source = as.LoadUniform(ir::Uniform{0, actual_type}).SetType(actual_type);
        as.PublishFCmpFlags(ir::Value{source.Def()}.SetCastType(view), ir::Imm{compact}); block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator ordinary{block.get(), {}, nullptr, nullptr, true, x64::ExecutionMode::Ordinary, cfg, false}; ordinary.Translate();
        x64::Translator resident{block.get(), {}, nullptr, nullptr, true, x64::ExecutionMode::Ordinary, cfg, true}; resident.Translate();
        REQUIRE(resident.Statistics().flags_resident_update_sites == 1); REQUIRE(ordinary.Statistics().flags_resident_update_sites == 0);
        REQUIRE(resident.Statistics().flags_resident_read_sites == 0); REQUIRE(resident.Statistics().flags_resident_write_sites == 0);
        for (u64 prior : {initial_flags, UINT64_MAX, u64{0x8000000000000000}}) for (u64 outcome = 0; outcome < 8; ++outcome) {
            CAPTURE(cfg, width, compact, prior, outcome); Storage baseline, actual; const u64 input = outcome | 0xfedcba9876543210ULL;
            for (auto* storage : {&baseline, &actual}) { storage->state->host_cpu_flags = prior; storage->Put(0, input); }
            const u64 value = ScalarView(input, actual_type, view);
            const bool carry = bool(value & 1) != (compact != 0), equal = value & 4, unordered = value & 2;
            const u64 expected = (prior & ~known_flags) | (u64(carry) << 29) | (u64(equal) << 30) | u64(!unordered);
            Run(ordinary, baseline); Run(resident, actual); CompleteEqual(baseline, actual); REQUIRE(actual.state->host_cpu_flags == expected);
        }
    }
}

TEST_CASE("packed flags residency preserves full carry chains and reduces actual State transfers", "[x64][flags][residency]") {
    for (bool cache : {false, true}) for (u32 bits : {8u, 16u, 32u, 64u}) for (bool subtract : {false, true}) {
        auto block = Block(); ir::Assembler as{block.get()}; const auto type = bits == 8 ? T::U8 : bits == 16 ? T::U16 : bits == 32 ? T::U32 : T::U64;
        auto value = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(type);
        auto rhs = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(type);
        for (u32 i = 0; i < 12; ++i) {
            value = (subtract ? as.Sbb(value, ir::Operand{rhs}) : as.Adc(value, ir::Operand{rhs})).SetType(type);
            as.SaveFlags(value, F::NZCV | F::AuxiliaryCarry | F::Parity);
        }
        Store(as, 16, value); Store(as, 24, as.GetFlags(value, F::NZCV).SetType(T::U64));
        Store(as, 32, as.TestFlags(F::NZCV | F::AuxiliaryCarry | F::Parity).SetType(T::U64));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto config = Configuration(cache);
        std::array<x64::EmitStatistics, 2> stats;
        for (bool resident : {false, true}) {
            x64::Translator translator{block.get(), {}, &config, nullptr, cache, x64::ExecutionMode::Ordinary, false, resident}; translator.Translate();
            Storage storage; storage.Put(0, 0xfffffffffffffff1ULL); storage.Put(8, 0x8123456789abcdefULL);
            u64 expected = storage.Get(0), flags = initial_flags;
            for (u32 i = 0; i < 12; ++i) flags = Arithmetic(flags, subtract, expected, storage.Get(8), bits, expected);
            Run(translator, storage); REQUIRE(storage.Get(16) == expected); REQUIRE(storage.Get(24) == flags);
            const bool test = (flags & (u64{15} << 28)) && (flags & (u64{1} << 26)) && !(std::popcount(u32(flags & 255)) & 1);
            REQUIRE(storage.Get(32) == u64(test)); REQUIRE(storage.state->host_cpu_flags == flags);
            stats[resident] = translator.Statistics();
        }
        REQUIRE(stats[1].At(x64::TransferRegion::StateFlags).scalar_load_sites < stats[0].At(x64::TransferRegion::StateFlags).scalar_load_sites);
        REQUIRE(stats[1].At(x64::TransferRegion::StateFlags).scalar_store_sites < stats[0].At(x64::TransferRegion::StateFlags).scalar_store_sites);
        REQUIRE(stats[1].flags_resident_read_sites == 2); REQUIRE(stats[1].flags_resident_write_sites == 0);
        REQUIRE(stats[1].flags_resident_merge_sites == 12); REQUIRE(stats[0].flags_resident_merge_sites == 0);
    }
}

TEST_CASE("single resident SaveFlags merges partial requests without truncating the packed word", "[x64][flags][residency][merge]") {
    for (bool cfg : {false, true}) for (bool resident : {false, true})
    for (auto requested : {F::None, F::Positive, F::Negate, F::CV, F::NZCV, F::AuxiliaryCarry, F::Parity, F::All}) {
        CAPTURE(cfg, resident, requested);
        auto block = Block(); ir::Assembler as{block.get()};
        auto left = as.LoadUniform(ir::Uniform{0, T::U16}).SetType(T::U16);
        auto right = as.LoadUniform(ir::Uniform{8, T::U16}).SetType(T::U16);
        auto result = as.Sub(left, ir::Operand{right}).SetType(T::U16);
        Store(as, 16, result);
        // Clobber native RFLAGS before publishing this single delayed request.
        Store(as, 24, as.Add(Imm(as, 17), ir::Operand{ir::Imm{u64{3}}}).SetType(T::U64));
        as.SaveFlags(result, requested); block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator translator{block.get(), {}, nullptr, nullptr, true, x64::ExecutionMode::Ordinary, cfg, resident};
        translator.Translate();
        const bool publishes = requested != F::None && requested != F::Positive;
        const auto& stats = translator.Statistics();
        REQUIRE(translator.FlagsResident() == (resident && publishes));
        REQUIRE(stats.flags_resident_read_sites == 0); REQUIRE(stats.flags_resident_write_sites == 0);
        REQUIRE(stats.flags_resident_merge_sites == u64(resident && publishes));
        u64 mask{};
        if (True(requested & F::Negate)) mask |= u64{1} << 31;
        if (True(requested & F::Zero)) mask |= u64{1} << 30;
        if (True(requested & F::Carry)) mask |= u64{1} << 29;
        if (True(requested & F::Overflow)) mask |= u64{1} << 28;
        if (True(requested & F::AuxiliaryCarry)) mask |= u64{1} << 26;
        if (True(requested & F::Parity)) mask |= 255;
        for (auto operands : std::array<std::array<u64, 2>, 4>{{{0, 1}, {0x8000, 1}, {0x8000, 0xffff}, {15, 15}}})
        for (u64 prior : std::array<u64, 3>{initial_flags, UINT64_MAX, 0x8000000000000000ULL}) {
            Storage storage; storage.Put(0, operands[0]); storage.Put(8, operands[1]); storage.state->host_cpu_flags = prior;
            u64 modeled_result{};
            // Carry=1 makes the independent SBB model a plain subtraction.
            const auto modeled_flags = Arithmetic(prior | (u64{1} << 29), true, operands[0], operands[1], 16, modeled_result);
            const auto expected = (prior & ~mask) | (modeled_flags & mask);
            Run(translator, storage); REQUIRE(storage.Get(16) == modeled_result); REQUIRE(storage.Get(24) == 20);
            REQUIRE(storage.state->host_cpu_flags == expected);
        }
    }
}

TEST_CASE("packed resident flags survive local diamonds and arbitrary native flag clobbers", "[x64][flags][residency][cfg]") {
    for (bool cache : {false, true}) for (bool resident : {false, true}) {
        auto block = Block(); ir::Assembler as{block.get()}; auto selected = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto jump = as.Goto(ir::BOOL{selected}); as.SetCarry(Imm(as, 0)); auto done = as.Goto(ir::BOOL{Imm(as, 1, T::U8)});
        as.BindLabel(jump); as.SetCarry(Imm(as, 1)); as.BindLabel(done);
        as.InvertCarry(); as.SetOverflow(Imm(as, 3)); as.ClearFlags(F::Parity | F::AuxiliaryCarry);
        auto value = as.Adc(Imm(as, UINT64_MAX), ir::Operand{ir::Imm{u64{0}}}).SetType(T::U64);
        as.SaveFlags(value, F::NZCV | F::AuxiliaryCarry | F::Parity); Store(as, 8, value);
        for (u32 code = 0; code < 16; ++code) Store(as, 16 + code * 8, as.CondSet(static_cast<ir::Cond>(code)).SetType(T::U64));
        block->SetTerminal(ir::terminal::ReturnToHost{}); const auto config = Configuration(cache);
        x64::Translator translator{block.get(), {}, &config, nullptr, cache, x64::ExecutionMode::Ordinary, false, resident}; translator.Translate();
        for (bool taken : {false, true}) {
            Storage storage; storage.Put(0, taken); u64 prior = (initial_flags & ~((u64{1} << 29) | (u64{1} << 26) | 255)) | (u64(!taken) << 29) | (u64{1} << 28) | 1;
            u64 result{}; const auto flags = Arithmetic(prior, false, UINT64_MAX, 0, 64, result);
            Run(translator, storage); REQUIRE(storage.Get(8) == result); REQUIRE(storage.state->host_cpu_flags == flags);
            for (u32 code = 0; code < 16; ++code) REQUIRE(storage.Get(16 + code * 8) == u64(Condition(code, flags)));
        }
    }
}

TEST_CASE("inline compare publication preserves all high bits and full compact truth values", "[x64][flags][residency][compare]") {
    for (u64 compact : std::array<u64, 5>{0, 1, 2, 0x100000000ULL, UINT64_MAX}) {
        auto block = Block(); ir::Assembler as{block.get()}; auto packed = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        as.PublishFCmpFlags(packed, ir::Imm{compact}); block->SetTerminal(ir::terminal::ReturnToHost{});
        for (bool resident : {false, true}) {
            x64::Translator translator{block.get(), {}, nullptr, nullptr, false, x64::ExecutionMode::Ordinary, false, resident}; translator.Translate();
            // Only the common EndMemory call remains; publication is inline.
            REQUIRE(translator.Statistics().helper_call_sites == 1);
            for (u64 bits = 0; bits < 8; ++bits) for (u64 prior : std::array<u64, 3>{initial_flags, UINT64_MAX, 0x8000000000000000ULL}) {
                Storage storage; storage.state->host_cpu_flags = prior; storage.Put(0, bits | 0xfedcba9876543210ULL);
                const auto packed_value = storage.Get(0);
                const auto expected = (prior & ~known_flags) | (((packed_value & 1) ^ u64(compact != 0)) << 29) |
                    (((packed_value >> 2) & 1) << 30) | (((packed_value >> 1) & 1) ^ 1);
                Run(translator, storage); REQUIRE(storage.state->host_cpu_flags == expected);
            }
        }
    }
}

TEST_CASE("returning helpers see committed flags and reload replacement without corrupting results", "[x64][flags][residency][helper]") {
    for (bool resident : {false, true}) for (u32 shape = 0; shape < 4; ++shape) for (u32 stop = 0; stop < 3; ++stop) {
        if (shape == 3 && stop) continue; // Cpuid is a pair-return operation, not an internal halt boundary.
        auto block = Block(); ir::Assembler as{block.get()}; as.SetCarry(Imm(as, 0));
        auto argument = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64); ir::Value result;
        if (shape == 3) {
            result = as.Cpuid(argument, argument, ir::Imm{u64{0}}, ir::Imm{reinterpret_cast<u64>(&PairHelper)}).SetType(T::U64);
            Store(as, 24, as.CpuidUpper(result).SetType(T::U64));
        } else {
            ir::Lambda lambda{ir::Imm{reinterpret_cast<u64>(&MutateFlags)}};
            if (!shape) result = as.CallLambda(lambda, argument, argument, argument).SetType(T::U64);
            else { ir::Params params; params.Push(argument); params.Push(argument); params.Push(argument);
                result = (shape == 1 ? as.CallDynamic(lambda, params) : as.CallLocation(lambda, params)).SetType(T::U64); }
        }
        Store(as, 8, result); Store(as, 16, as.GetFlags(result, F::NZCV).SetType(T::U64));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator translator{block.get(), {}, nullptr, nullptr, false, x64::ExecutionMode::Ordinary, false, resident}; translator.Translate();
        Storage storage; HelperRequest request{.state = storage.state, .expected = initial_flags & ~(u64{1} << 29),
            .replacement = 0xfedcba98123456e7ULL, .signal = stop == 1, .fault = stop == 2};
        storage.Put(0, reinterpret_cast<u64>(&request)); const auto sentinel = storage.Get(16), result_sentinel = storage.Get(8);
        Run(translator, storage, stop == 1 ? HaltReason::Signal : stop == 2 ? HaltReason::PageFatal : HaltReason::CallHost);
        REQUIRE(request.calls == 1); REQUIRE(request.observed == request.expected); REQUIRE(storage.state->host_cpu_flags == request.replacement);
        REQUIRE(storage.Get(8) == (stop ? result_sentinel : helper_result));
        REQUIRE(storage.Get(16) == (stop ? sentinel : request.replacement));
        if (shape == 3) REQUIRE(storage.Get(24) == request.replacement);
    }
}

TEST_CASE("resident flags are committed before actual guest data fault and recover without stale RBX", "[x64][flags][residency][fault]") {
    struct Mapping { void* address = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ~Mapping() { if (address != MAP_FAILED) munmap(address, 4096); } } mapping;
    REQUIRE(mapping.address != MAP_FAILED);
    for (bool resident : {false, true}) {
        auto config = Configuration(false); backend::AddressSpace space{config};
        backend::ModuleConfig module_config; module_config.feature_overrides.Set(FeatureId::x64_flags_residency, resident);
        auto module = space.MapModule(0x1000, 0x2000, module_config); Runtime runtime{&space};
        auto block = Block(); ir::Assembler as{block.get()}; as.ClearFlags(F::NZCV | F::Parity | F::AuxiliaryCarry);
        as.SetCarry(Imm(as, 1)); auto address = Imm(as, reinterpret_cast<u64>(mapping.address));
        auto loaded = as.LoadMemory(ir::Operand{address}).SetType(T::U64); Store(as, 8, loaded); block->SetTerminal(ir::terminal::ReturnToHost{});
        REQUIRE(module->Push(block.get())); REQUIRE(backend::TranslateIR(module, block) != nullptr);
        runtime.GetState()->host_cpu_flags = initial_flags; runtime.GetState()->guest_addr_limit = UINT64_MAX;
        runtime.SetLocation(0x1000); const u64 sentinel = 0x123456789abcdef0ULL; std::memcpy(runtime.GetUniformBuffer().data() + 8, &sentinel, 8);
        REQUIRE(runtime.Run() == HaltReason::PageFatal);
        REQUIRE(runtime.GetState()->host_cpu_flags == ((initial_flags & ~known_flags) | (u64{1} << 29) | 1));
        u64 actual; std::memcpy(&actual, runtime.GetUniformBuffer().data() + 8, 8); REQUIRE(actual == sentinel);
        REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
        REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
    }
}

TEST_CASE("resident flags reserve RBX while the scalar cache fills both remaining slots", "[x64][flags][residency][scalar]") {
    auto block = Block(); ir::Assembler as{block.get()}; std::array<ir::Value, 4> values;
    for (u32 i = 0; i < values.size(); ++i) values[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
    for (u32 i = 0; i < values.size(); ++i) Store(as, 32 + i * 8, values[i]);
    Store(as, 80, as.GetFlags(values[0], F::NZCV).SetType(T::U64));
    block->SetTerminal(ir::terminal::ReturnToHost{}); const auto config = Configuration(true);
    for (bool resident : {false, true}) {
        x64::Translator translator{block.get(), {}, &config, nullptr, true, x64::ExecutionMode::Ordinary, false, resident}; translator.Translate();
        REQUIRE(translator.ScalarPlan().Enabled()); REQUIRE(translator.ScalarPlan().ValidateNonOverlapping());
        std::array<bool, 3> used{};
        for (const auto& interval : translator.ScalarPlan().Intervals()) {
            REQUIRE(interval.slot < used.size()); used[interval.slot] = true;
            if (resident) REQUIRE(x64::ScalarRegisterPlan::host_register_indices[interval.slot] != 3);
        }
        REQUIRE(std::count(used.begin(), used.end(), true) == (resident ? 2 : 3));
        Storage storage; for (u32 i = 0; i < values.size(); ++i) storage.Put(i * 8, 0x123456789abcdef0ULL + i);
        Run(translator, storage);
        for (u32 i = 0; i < values.size(); ++i) REQUIRE(storage.Get(32 + i * 8) == storage.Get(i * 8));
        REQUIRE(storage.state->host_cpu_flags == initial_flags);
        REQUIRE(storage.Get(80) == initial_flags);
    }
}

TEST_CASE("ordinary Runtime reloads flags after abandoning a helper with temporary RBX", "[x64][flags][residency][fault][helper]") {
    struct Mapping { void* address = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ~Mapping() { if (address != MAP_FAILED) munmap(address, 4096); } } mapping;
    REQUIRE(mapping.address != MAP_FAILED);
    for (bool cache : {false, true}) for (bool resident : {false, true}) {
        REQUIRE(mprotect(mapping.address, 4096, PROT_NONE) == 0);
        auto config = Configuration(cache); backend::AddressSpace space{config};
        backend::ModuleConfig module_config; module_config.feature_overrides.Set(FeatureId::x64_flags_residency, resident);
        auto module = space.MapModule(0x1000, 0x2000, module_config); Runtime runtime{&space};
        auto block = Block(); ir::Assembler as{block.get()}; as.ClearFlags(F::NZCV | F::Parity | F::AuxiliaryCarry); as.SetCarry(Imm(as, 1));
        const auto argument = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        const auto address = Imm(as, reinterpret_cast<u64>(mapping.address));
        const auto result = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&AbandonWithTemporaryRbx)}}, argument, address, argument).SetType(T::U64);
        Store(as, 8, result); Store(as, 16, as.GetFlags(result, F::NZCV).SetType(T::U64)); block->SetTerminal(ir::terminal::ReturnToHost{});
        REQUIRE(module->Push(block.get())); REQUIRE(backend::TranslateIR(module, block) != nullptr);
        HelperRequest request{.state = runtime.GetState(), .expected = (initial_flags & ~known_flags) | (u64{1} << 29) | 1,
                              .replacement = 0xcafe0123456789abULL};
        auto uniforms = runtime.GetUniformBuffer(); const u64 pointer = reinterpret_cast<u64>(&request), sentinel = 0x123456789abcdef0ULL;
        std::memcpy(uniforms.data(), &pointer, 8);
        for (bool accessible : {false, true}) {
            if (accessible) {
                REQUIRE(mprotect(mapping.address, 4096, PROT_READ | PROT_WRITE) == 0);
                std::memcpy(mapping.address, &sentinel, 8);
            }
            runtime.GetState()->host_cpu_flags = initial_flags; runtime.SetLocation(0x1000);
            std::memcpy(uniforms.data() + 8, &sentinel, 8); std::memcpy(uniforms.data() + 16, &sentinel, 8);
            const auto host_fp = _mm_getcsr();
            REQUIRE(runtime.Run() == (accessible ? HaltReason::CallHost : HaltReason::PageFatal));
            REQUIRE(_mm_getcsr() == host_fp); REQUIRE(request.calls == (accessible ? 2 : 1)); REQUIRE(request.observed == request.expected);
            REQUIRE(runtime.GetState()->host_cpu_flags == request.replacement);
            u64 actual; std::memcpy(&actual, uniforms.data() + 8, 8); REQUIRE(actual == (accessible ? helper_result : sentinel));
            std::memcpy(&actual, uniforms.data() + 16, 8); REQUIRE(actual == (accessible ? request.replacement : sentinel));
            REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
            REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
        }
    }
}

TEST_CASE("unused flags residency retains all scalar slots and preserves helper State mutations", "[x64][flags][residency][adaptive]") {
    for (bool cfg : {false, true}) for (bool cache : {false, true}) for (bool requested : {false, true}) for (bool helper : {false, true}) {
        auto block = Block(); ir::Assembler as{block.get()}; std::array<ir::Value, 4> values;
        for (u32 i = 0; i < values.size(); ++i) values[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
        for (u32 i = 0; i < values.size(); ++i) Store(as, 32 + i * 8, values[i]);
        // Constant predicates and no-op requests never access packed flags.
        as.SaveFlags(values[0], F::None); as.BranchOnlyFlags(values[0], F::None); as.ClearFlags(F::Positive);
        Store(as, 64, as.CondSet(ir::Cond::AL).SetType(T::U64));
        Store(as, 72, as.TestFlags(F::None).SetType(T::U64));
        if (helper) {
            const auto argument = as.LoadUniform(ir::Uniform{96, T::U64}).SetType(T::U64);
            const auto result = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&MutateFlags)}}, argument, argument, argument).SetType(T::U64);
            Store(as, 80, result);
        }
        block->SetTerminal(ir::terminal::ReturnToHost{}); const auto config = Configuration(cache);
        x64::Translator translator{block.get(), {}, &config, nullptr, cache, x64::ExecutionMode::Ordinary, cfg, requested}; translator.Translate();
        REQUIRE_FALSE(translator.FlagsResident());
        const auto& stats = translator.Statistics();
        REQUIRE(stats.flags_state_load_sites == 0); REQUIRE(stats.flags_state_store_sites == 0);
        REQUIRE(stats.flags_resident_read_sites == 0); REQUIRE(stats.flags_resident_write_sites == 0);
        REQUIRE(stats.flags_resident_merge_sites == 0);
        if (cfg) {
            const std::vector<unsigned> expected_registers{3, 5, 15};
            REQUIRE(translator.CfgPlan().PhysicalRegisters() == expected_registers);
        }
        else if (cache) {
            std::array<bool, 3> used{};
            for (const auto& interval : translator.ScalarPlan().Intervals()) used[interval.slot] = true;
            REQUIRE(std::count(used.begin(), used.end(), true) == 3);
        }
        for (u64 prior : std::array<u64, 3>{initial_flags, UINT64_MAX, 0x8000000000000000ULL}) {
            Storage storage; storage.state->host_cpu_flags = prior;
            for (u32 i = 0; i < values.size(); ++i) storage.Put(i * 8, 0x123456789abcdef0ULL + i);
            HelperRequest request{.state = storage.state, .replacement = 0xfedcba98123456e7ULL};
            storage.Put(96, reinterpret_cast<u64>(&request));
            Run(translator, storage);
            for (u32 i = 0; i < values.size(); ++i) REQUIRE(storage.Get(32 + i * 8) == storage.Get(i * 8));
            REQUIRE(storage.Get(64) == 1); REQUIRE(storage.Get(72) == 0);
            REQUIRE(storage.state->host_cpu_flags == (helper ? request.replacement : prior));
            if (helper) { REQUIRE(request.calls == 1); REQUIRE(request.observed == prior); REQUIRE(storage.Get(80) == helper_result); }
        }
    }
}

TEST_CASE("implicit carry reads activate residency without any flag publisher", "[x64][flags][residency][adaptive][carry]") {
    for (bool subtract : {false, true}) for (bool cfg : {false, true}) for (bool requested : {false, true}) {
        auto block = Block(); ir::Assembler as{block.get()}; const auto value = Imm(as, 11), rhs = Imm(as, 3);
        auto result = (subtract ? as.Sbb(value, ir::Operand{rhs}) : as.Adc(value, ir::Operand{rhs})).SetType(T::U64);
        Store(as, 0, result); block->SetTerminal(ir::terminal::ReturnToHost{}); const auto config = Configuration(true);
        x64::Translator translator{block.get(), {}, &config, nullptr, true, x64::ExecutionMode::Ordinary, cfg, requested}; translator.Translate();
        REQUIRE(translator.FlagsResident() == requested);
        if (cfg) REQUIRE(translator.CfgPlan().PhysicalRegisters().size() == (requested ? 2 : 3));
        for (bool carry : {false, true}) {
            Storage storage; storage.state->host_cpu_flags = (initial_flags & ~(u64{1} << 29)) | (u64(carry) << 29);
            const auto prior = storage.state->host_cpu_flags;
            Run(translator, storage); REQUIRE(storage.Get(0) == (subtract ? 11 - 3 - u64(!carry) : 11 + 3 + u64(carry)));
            REQUIRE(storage.state->host_cpu_flags == prior);
        }
    }
}

TEST_CASE("adaptive residency follows recursive actual terminals and replaced function markers", "[x64][flags][residency][adaptive][terminal]") {
    for (u32 shape = 0; shape < 8; ++shape) for (bool requested : {false, true}) {
        backend::FunctionPlan plan; plan.body = Block(); ir::Assembler as{plan.body.get()};
        const auto selector = Imm(as, 1);
        ir::Terminal terminal = ir::terminal::Condition{ir::Cond::EQ, ir::terminal::ReturnToHost{}, ir::terminal::Invalid{}};
        if (shape == 1) terminal = ir::terminal::If{ir::BOOL{selector}, terminal, ir::terminal::ReturnToHost{}};
        if (shape == 2) terminal = ir::terminal::CheckHalt{terminal};
        if (shape == 3) terminal = ir::terminal::Switch{selector, {{ir::Imm{u64{1}}, terminal}}};
        if (shape == 7) terminal = ir::terminal::Condition{ir::Cond::AL, ir::terminal::ReturnToHost{}, ir::terminal::Invalid{}};
        const bool uses_flags = shape < 5;
        if (shape >= 4) {
            // These opcodes are replaced by metadata dispatch and never
            // emitted. Only the real exit terminal may require residency.
            auto* marker = as.AppendInst(ir::OpCode::SetCarry, selector);
            if (shape == 6) plan.halt_checks.insert(marker);
            else {
                plan.stops.insert(marker);
                plan.exits.emplace(marker, shape == 4 || shape == 7 ? terminal : ir::Terminal{ir::terminal::ReturnToHost{}});
            }
            plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        } else plan.body->SetTerminal(terminal);
        const auto config = Configuration(false);
        x64::Translator translator{plan.body.get(), {}, &config, shape >= 4 ? &plan : nullptr, false, x64::ExecutionMode::Ordinary, false, requested}; translator.Translate();
        REQUIRE(translator.FlagsResident() == (requested && uses_flags));
        for (bool zero : {false, true}) {
            Storage storage; storage.state->host_cpu_flags = (initial_flags & ~(u64{1} << 30)) | (u64(zero) << 30);
            const auto prior = storage.state->host_cpu_flags;
            Run(translator, storage, uses_flags && !zero ? HaltReason::IllegalCode : HaltReason::CallHost);
            REQUIRE(storage.state->host_cpu_flags == prior);
        }
    }
}
