#include <array>
#include <cstring>
#include <limits>
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
class InstructionMemory final : public MemoryInterface {
public:
    explicit InstructionMemory(u32 instruction) : code{instruction, 0xD4200000} {}
    bool Read(void* destination, size_t address, size_t size) override {
        if (address < 0x1000 || address - 0x1000 + size > sizeof(code)) return false;
        std::memcpy(destination, reinterpret_cast<u8*>(code.data()) + address - 0x1000, size);
        return true;
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
    backend::State* state = new (storage.data()) backend::State{};
    ~Machine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() {
        return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin);
    }
    void Vector(u32 reg, std::array<u64, 2> value) { std::memcpy(&Context().v[reg], value.data(), 16); }
    std::array<u64, 2> Vector(u32 reg) {
        std::array<u64, 2> value; std::memcpy(value.data(), &Context().v[reg], 16); return value;
    }
    void Run(u32 instruction) {
        state->halt_reason = HaltReason::None;
        InstructionMemory memory(instruction);
        ir::Block block(0, 0x1000);
        ir::Assembler assembler(&block);
        swift::arm64::A64Decoder decoder(0x1000, &memory, &assembler);
        decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto initial = Context();
        const auto initial_flags = state->host_cpu_flags;
#endif
        backend::interp::Interpreter interpreter(*state, &block);
        interpreter.Run();
        REQUIRE(Context().padding == u32(swift::arm64::InterruptReason::BRK));
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto expected = Context();
        const auto flags = state->host_cpu_flags;
        Context() = initial;
        state->host_cpu_flags = initial_flags;
        state->halt_reason = HaltReason::None;
        backend::x64::Translator translator(&block);
        translator.Translate();
        translator.getCode<HaltReason (*)(backend::State*)>()(state);
        REQUIRE(std::memcmp(&Context(), &expected, sizeof(expected)) == 0);
        REQUIRE(state->host_cpu_flags == flags);
#endif
    }
};
u64 Mask(u32 bits) { return bits == 64 ? ~u64{0} : (u64{1} << bits) - 1; }
u64 Lane(std::array<u64, 2> value, u32 bits, u32 lane) {
    return (value[lane * bits / 64] >> (lane * bits % 64)) & Mask(bits);
}
void Lane(std::array<u64, 2>& value, u32 bits, u32 lane, u64 bits_value) {
    value[lane * bits / 64] |= (bits_value & Mask(bits)) << (lane * bits % 64);
}
s64 Signed(u64 value, u32 bits) {
    const u64 sign = u64{1} << (bits - 1);
    return s64((value ^ sign) - sign);
}
u32 Indexed(u32 op, u32 bits, bool q, u32 rm, u32 index, u32 rn, u32 rd) {
    u32 encoding = op | (bits == 16 ? 1u : 2u) << 22 | (q ? 1u << 30 : 0) | (rn << 5) | rd;
    if (bits == 16) return encoding | (rm << 16) | ((index & 1) << 20) |
                                  (((index >> 1) & 1) << 21) | ((index >> 2) << 11);
    return encoding | (rm << 16) | ((index & 1) << 21) | ((index >> 1) << 11);
}
}

TEST_CASE("ARM64 integer indexed multiplication decodes every lane and register alias") {
    const std::array<u32, 9> operations{NEON_MUL_byelement, NEON_MLA_byelement, NEON_MLS_byelement,
        NEON_SMULL_byelement, NEON_UMULL_byelement, NEON_SMLAL_byelement,
        NEON_UMLAL_byelement, NEON_SMLSL_byelement, NEON_UMLSL_byelement};
    for (u32 bits : {16u, 32u}) for (bool q : {false, true}) {
        for (u32 element_index = 0; element_index < 128 / bits; ++element_index) {
            for (u32 op : operations) for (u32 alias = 0; alias < 4; ++alias) {
                INFO("bits=" << bits << " q=" << q << " index=" << element_index << " op=" << op << " alias=" << alias);
                const u32 rm = bits == 16 ? 15 : 31;
                const u32 rn = alias == 3 ? rm : 1;
                const u32 rd = alias == 1 ? rn : alias >= 2 ? rm : 0;
                Machine machine;
                machine.state->host_cpu_flags = 0xB0000000;
                machine.Context().fpsr = 0x0800009F;
                machine.Vector(0, {0x123456789ABCDEF0, 0xFEDCBA9876543210});
                machine.Vector(1, {0x8001FFFF7FFF0001, 0xFFFE80007FFE0002});
                machine.Vector(rm, {0x800012347FFF0003, 0xDEADFFFF80000002});
                const auto source = machine.Vector(rn), elements = machine.Vector(rm), old = machine.Vector(rd);
                const bool narrow = op == NEON_MUL_byelement || op == NEON_MLA_byelement || op == NEON_MLS_byelement;
                const bool sign = op == NEON_SMULL_byelement || op == NEON_SMLAL_byelement || op == NEON_SMLSL_byelement;
                const bool accumulate = op != NEON_MUL_byelement && op != NEON_SMULL_byelement && op != NEON_UMULL_byelement;
                const bool subtract = op == NEON_MLS_byelement || op == NEON_SMLSL_byelement || op == NEON_UMLSL_byelement;
                const u32 out_bits = narrow ? bits : bits * 2;
                const u32 lanes = narrow ? (q ? 128 : 64) / bits : 64 / bits;
                const u64 element = Lane(elements, bits, element_index);
                std::array<u64, 2> expected{};
                for (u32 lane = 0; lane < lanes; ++lane) {
                    const u64 a = Lane(source, bits, lane + (!narrow && q ? 64 / bits : 0));
                    u64 result = sign ? u64(Signed(a, bits) * Signed(element, bits)) : a * element;
                    if (accumulate) result = subtract ? Lane(old, out_bits, lane) - result : Lane(old, out_bits, lane) + result;
                    Lane(expected, out_bits, lane, result);
                }
                machine.Run(Indexed(op, bits, q, rm, element_index, rn, rd));
                REQUIRE(machine.Vector(rd) == expected);
                REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
                REQUIRE(machine.Context().fpsr == 0x0800009F);
            }
        }
    }
}

TEST_CASE("ARM64 scalar integer comparisons and arithmetic preserve flags and clear upper bits") {
    const std::array<u32, 8> ops{NEON_ADD_scalar, NEON_SUB_scalar, NEON_CMEQ_scalar, NEON_CMGT_scalar,
        NEON_CMGE_scalar, NEON_CMHI_scalar, NEON_CMHS_scalar, NEON_CMTST_scalar};
    const std::array<u64, 6> values{0, 1, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, ~u64{1}, ~u64{0}};
    for (u32 op : ops) for (u64 a : values) for (u64 b : values) for (u32 alias = 0; alias < 3; ++alias) {
        Machine machine;
        const u32 rd = alias;
        machine.Vector(0, {~u64{0}, ~u64{0}});
        machine.Vector(1, {a, ~u64{0}});
        machine.Vector(2, {b, ~u64{0}});
        machine.state->host_cpu_flags = 0xF0000000;
        machine.Context().fpsr = 0x0800009F;
        u64 expected;
        if (op == NEON_ADD_scalar) expected = a + b;
        else if (op == NEON_SUB_scalar) expected = a - b;
        else {
            const bool condition = op == NEON_CMEQ_scalar ? a == b :
                op == NEON_CMGT_scalar ? s64(a) > s64(b) : op == NEON_CMGE_scalar ? s64(a) >= s64(b) :
                op == NEON_CMHI_scalar ? a > b : op == NEON_CMHS_scalar ? a >= b : bool(a & b);
            expected = condition ? ~u64{0} : 0;
        }
        machine.Run(op | (3u << 22) | (2u << 16) | (1u << 5) | rd);
        REQUIRE(machine.Vector(rd) == std::array<u64, 2>{expected, 0});
        REQUIRE(machine.state->host_cpu_flags == 0xF0000000);
        REQUIRE(machine.Context().fpsr == 0x0800009F);
    }
}

TEST_CASE("ARM64 scalar variable shifts use signed low-byte counts and guard full-width shifts") {
    for (u32 op : {u32(NEON_SSHL_scalar), u32(NEON_USHL_scalar)}) {
        for (u64 input : {u64{0}, u64{1}, u64{0x8000000000000000}, ~u64{0}}) {
            for (s32 count : {-128, -65, -64, -63, -1, 0, 1, 63, 64, 65, 127}) {
                for (u32 rd : {0u, 1u, 2u}) {
                    Machine machine;
                    machine.Vector(1, {input, ~u64{0}});
                    machine.Vector(2, {0xA5A5A5A5A5A50000 | u8(count), ~u64{0}});
                    machine.state->host_cpu_flags = 0x90000000;
                    u64 expected;
                    if (count >= 0) expected = count >= 64 ? 0 : input << count;
                    else if (op == NEON_USHL_scalar) expected = count <= -64 ? 0 : input >> -count;
                    else if (count <= -64) expected = input >> 63 ? ~u64{0} : 0;
                    else expected = input >> -count | (input >> 63 ? (~u64{0} << (64 + count)) : 0);
                    machine.Run(op | (3u << 22) | (2u << 16) | (1u << 5) | rd);
                    REQUIRE(machine.Vector(rd) == std::array<u64, 2>{expected, 0});
                    REQUIRE(machine.state->host_cpu_flags == 0x90000000);
                }
            }
        }
    }
}

TEST_CASE("ARM64 scalar saturation clamps every lane width and accumulates QC") {
    for (u32 size = 0; size < 4; ++size) {
        const u32 bits = 8u << size;
        const u64 mask = Mask(bits), sign = u64{1} << (bits - 1);
        const std::array<u64, 5> values{0, 1, sign - 1, sign, mask};
        for (u32 op : {u32(NEON_SQADD_scalar), u32(NEON_UQADD_scalar), u32(NEON_SQSUB_scalar), u32(NEON_UQSUB_scalar)}) {
            for (u64 a : values) for (u64 b : values) for (bool sticky : {false, true}) {
                Machine machine;
                machine.Vector(1, {a | (bits == 64 ? 0 : ~mask), ~u64{0}});
                machine.Vector(2, {b | (bits == 64 ? 0 : ~mask), ~u64{0}});
                machine.state->host_cpu_flags = 0xA0000000;
                machine.Context().fpsr = 0x9F | (sticky ? 1u << 27 : 0);
                const bool add = op == NEON_SQADD_scalar || op == NEON_UQADD_scalar;
                const bool signed_op = op == NEON_SQADD_scalar || op == NEON_SQSUB_scalar;
                bool saturated;
                u64 expected = (add ? a + b : a - b) & mask;
                if (!signed_op) {
                    saturated = add ? a > mask - b : a < b;
                    if (saturated) expected = add ? mask : 0;
                } else {
                    const s64 left = Signed(a, bits), right = Signed(b, bits);
                    const s64 maximum = s64(sign - 1), minimum = bits == 64 ? std::numeric_limits<s64>::min() : -s64(sign);
                    const bool high = add ? right > 0 && left > maximum - right : right < 0 && left > maximum + right;
                    const bool low = add ? right < 0 && left < minimum - right : right > 0 && left < minimum + right;
                    saturated = high || low;
                    if (saturated) expected = high ? u64(maximum) : u64(minimum) & mask;
                }
                machine.Run(op | (size << 22) | (2u << 16) | (1u << 5) | 1u);
                REQUIRE(machine.Vector(1) == std::array<u64, 2>{expected, 0});
                REQUIRE(machine.Context().fpsr == (0x9F | (sticky || saturated ? 1u << 27 : 0)));
                REQUIRE(machine.state->host_cpu_flags == 0xA0000000);
            }
        }
    }
}
