#include <array>
#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;
using namespace vixl::aarch64;
namespace {
using ScalarVectorBits = std::array<u64, 2>;
u64 Mask(u32 bits) { return bits == 64 ? ~u64{0} : (u64{1} << bits) - 1; }
s64 Signed(u64 value, u32 bits) { const u64 sign = u64{1} << (bits - 1); return s64((value ^ sign) - sign); }
class InstructionMemory final : public MemoryInterface {
public:
    explicit InstructionMemory(u32 instruction) : code{instruction, 0xD4200000} {}
    bool Read(void* destination, size_t address, size_t size) override {
        if (address < 0x1000 || address - 0x1000 + size > sizeof(code)) return false;
        std::memcpy(destination, reinterpret_cast<u8*>(code.data()) + address - 0x1000, size); return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* pointer) override {
        const auto address = reinterpret_cast<size_t>(pointer);
        return address >= 0x1000 && address - 0x1000 < sizeof(code)
            ? reinterpret_cast<u8*>(code.data()) + address - 0x1000 : nullptr;
    }
    std::array<u32, 2> code;
};
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(swift::arm64::ThreadContext64) + 32> storage{};
    backend::State* state = new(storage.data()) backend::State{};
    Machine() { state->host_cpu_flags = 0xB0000000; Context().fpcr = 0x01400000; }
    ~Machine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() { return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin); }
    void SetVector(u32 reg, ScalarVectorBits value) { std::memcpy(&Context().v[reg], value.data(), 16); }
    ScalarVectorBits GetVector(u32 reg) { ScalarVectorBits value{}; std::memcpy(value.data(), &Context().v[reg], 16); return value; }
    void Run(u32 code, swift::arm64::InterruptReason reason = swift::arm64::InterruptReason::BRK) {
        InstructionMemory memory(code); ir::Block block(0, 0x1000); ir::Assembler assembler(&block);
        swift::arm64::A64Decoder decoder(0x1000, &memory, &assembler); decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto initial = Context(); const auto flags = state->host_cpu_flags; const auto location = state->current_loc;
#endif
        backend::interp::Interpreter interpreter(*state, &block); interpreter.Run();
        REQUIRE(Context().padding == u32(reason));
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto expected = Context(); const auto expected_flags = state->host_cpu_flags; const auto expected_location = state->current_loc;
        for (bool baseline : {false, true}) {
            Context() = initial; state->host_cpu_flags = flags; state->current_loc = location; state->halt_reason = HaltReason::None;
            backend::x64::Translator translator(&block, baseline ? backend::x64::HostFeatures{} : backend::x64::HostFeatures::Detect());
            translator.Translate(); translator.getCode<HaltReason(*)(backend::State*)>()(state);
            REQUIRE(std::memcmp(&Context(), &expected, sizeof(expected)) == 0); REQUIRE(state->host_cpu_flags == expected_flags);
            REQUIRE(state->current_loc.Value() == expected_location.Value());
        }
#endif
    }
};
}

TEST_CASE("ARM64 scalar miscellaneous unary and zero comparisons clear inactive bits and preserve QC") {
    for (u32 op : {u32(NEON_ABS_scalar), u32(NEON_NEG_scalar), u32(NEON_SQABS_scalar), u32(NEON_SQNEG_scalar),
         u32(NEON_CMGT_zero_scalar), u32(NEON_CMGE_zero_scalar), u32(NEON_CMEQ_zero_scalar),
         u32(NEON_CMLT_zero_scalar), u32(NEON_CMLE_zero_scalar)}) for (u32 size = 0; size < 4; ++size) {
        const bool saturated_op = op == NEON_SQABS_scalar || op == NEON_SQNEG_scalar;
        if (!saturated_op && size != 3) continue;
        const u32 bits = 8u << size; const u64 mask = Mask(bits), sign = u64{1} << (bits - 1);
        std::vector<u64> inputs{0, 1, sign - 1, sign, sign + 1, mask - 1, mask};
        if (bits == 8) for (u32 value = 0; value < 256; ++value) inputs.push_back(value);
        for (u64 input : inputs) for (bool alias : {false, true}) for (bool sticky : {false, true}) {
            Machine machine; const u32 rd = alias ? 31 : 0;
            machine.SetVector(0, {0x123456789ABCDEF0, ~u64{0}});
            const ScalarVectorBits source{(input & mask) | (~mask & 0xDEADBEEFDEADBEEF), 0xBADF00D5BADF00D5};
            machine.SetVector(31, source); machine.Context().fpsr = 0x91 | (sticky ? 1u << 27 : 0);
            const s64 value = Signed(input, bits); u64 expected; bool saturated = false;
            if (op == NEON_ABS_scalar || op == NEON_SQABS_scalar) expected = value < 0 ? 0 - input : input;
            else if (op == NEON_NEG_scalar || op == NEON_SQNEG_scalar) expected = 0 - input;
            else {
                const bool condition = op == NEON_CMGT_zero_scalar ? value > 0 : op == NEON_CMGE_zero_scalar ? value >= 0 :
                    op == NEON_CMEQ_zero_scalar ? value == 0 : op == NEON_CMLT_zero_scalar ? value < 0 : value <= 0;
                expected = condition ? mask : 0;
            }
            if (saturated_op && input == sign) { expected = sign - 1; saturated = true; }
            machine.Run(op | (size << 22) | (31u << 5) | rd);
            REQUIRE(machine.GetVector(rd) == ScalarVectorBits{expected & mask, 0});
            if (!alias) REQUIRE(machine.GetVector(31) == source);
            REQUIRE(machine.Context().fpsr == (0x91 | ((sticky || saturated) ? 1u << 27 : 0)));
            REQUIRE(machine.Context().fpcr == 0x01400000); REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
    }
}

TEST_CASE("ARM64 scalar saturating narrowing clips independently and clears all unused destination bits") {
    for (u32 op : {u32(NEON_SQXTN_scalar), u32(NEON_UQXTN_scalar), u32(NEON_SQXTUN_scalar)})
        for (u32 size = 0; size < 3; ++size) {
            const u32 bits = 8u << size, source_bits = bits * 2; const u64 mask = Mask(bits), source_mask = Mask(source_bits);
            const u64 sign = u64{1} << (bits - 1), source_sign = u64{1} << (source_bits - 1);
            std::vector<u64> inputs{0,1,sign-1,sign,sign+1,mask,mask+1,source_sign-1,source_sign,
                source_sign+1,source_mask-sign,source_mask-sign+1,source_mask};
            for (u64 input : inputs) for (bool alias : {false, true}) for (bool sticky : {false, true}) {
                Machine machine; const u32 rd = alias ? 31 : 0;
                const ScalarVectorBits source{input | (~source_mask & 0xA5A5A5A5A5A5A5A5), ~u64{0}};
                machine.SetVector(0, {~u64{0}, ~u64{0}}); machine.SetVector(31, source);
                machine.Context().fpsr = 0x91 | (sticky ? 1u << 27 : 0);
                u64 expected; bool saturated;
                if (op == NEON_UQXTN_scalar) { saturated = input > mask; expected = saturated ? mask : input; }
                else {
                    const s64 value = Signed(input, source_bits), lower = op == NEON_SQXTN_scalar ? -s64(sign) : 0;
                    const s64 upper = op == NEON_SQXTN_scalar ? s64(sign - 1) : s64(mask);
                    const s64 clamped = value < lower ? lower : value > upper ? upper : value;
                    saturated = value != clamped; expected = u64(clamped) & mask;
                }
                machine.Run(op | (size << 22) | (31u << 5) | rd);
                REQUIRE(machine.GetVector(rd) == ScalarVectorBits{expected, 0});
                if (!alias) REQUIRE(machine.GetVector(31) == source);
                REQUIRE(machine.Context().fpsr == (0x91 | ((sticky || saturated) ? 1u << 27 : 0)));
                REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
            }
        }
}

TEST_CASE("ARM64 scalar pairwise integer addition sums both D lanes and clears its high half") {
    for (u64 low : {u64{0}, u64{1}, u64{0x8000000000000000}, ~u64{0}})
        for (u64 high : {u64{0}, u64{1}, u64{0x7FFFFFFFFFFFFFFF}, ~u64{0}}) for (bool alias : {false, true}) {
            Machine machine; const u32 rd = alias ? 31 : 0; machine.Context().fpsr = 0x08000091;
            machine.SetVector(0, {~u64{0}, ~u64{0}}); machine.SetVector(31, {low, high});
            machine.Run(NEON_ADDP_scalar | (1u << 22) | (31u << 5) | rd);
            REQUIRE(machine.GetVector(rd) == ScalarVectorBits{low + high, 0}); REQUIRE(machine.Context().fpsr == 0x08000091);
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
}

TEST_CASE("ARM64 scalar miscellaneous rejects vector-only counts and reserved scalar widths") {
    std::vector<u32> invalid;
    for (u32 size = 0; size < 4; ++size) {
        invalid.push_back((NEON_Q | NEONScalar | NEON_CLS) | (size << 22));
        invalid.push_back((NEON_Q | NEONScalar | NEON_CLZ) | (size << 22));
        if (size < 3) for (u32 op : {u32(NEON_ABS_scalar), u32(NEON_NEG_scalar), u32(NEON_CMGT_zero_scalar),
            u32(NEON_CMGE_zero_scalar),u32(NEON_CMEQ_zero_scalar),u32(NEON_CMLT_zero_scalar),u32(NEON_CMLE_zero_scalar)}) invalid.push_back(op | (size << 22));
    }
    for (u32 op : {u32(NEON_SQXTN_scalar),u32(NEON_UQXTN_scalar),u32(NEON_SQXTUN_scalar)}) invalid.push_back(op | (3u << 22));
    for (u32 size = 0; size < 3; ++size) invalid.push_back((u32(NEON_ADDP_scalar) & ~(3u << 22)) | (size << 22));
    for (u32 code : invalid) {
        Machine machine; machine.SetVector(0, {0x123456789ABCDEF0, ~u64{0}}); machine.SetVector(31, {~u64{0}, ~u64{0}});
        machine.Context().fpsr = 0x08000091; const auto before = machine.Context();
        machine.Run(code | (31u << 5), swift::arm64::InterruptReason::ILL_CODE);
        REQUIRE(std::memcmp(&machine.Context().v, &before.v, sizeof(before.v)) == 0);
        REQUIRE(machine.Context().fpsr == before.fpsr); REQUIRE(machine.state->current_loc.Value() == 0x1000);
        REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    }
}
