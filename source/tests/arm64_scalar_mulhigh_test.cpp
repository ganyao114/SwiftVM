#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <boost/multiprecision/cpp_int.hpp>
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
using HighVectorBits = std::array<u64, 2>;
using Big = boost::multiprecision::cpp_int;
constexpr u32 kQC = 1u << 27;
u64 Mask(u32 bits) { return (u64{1} << bits) - 1; }
Big Signed(u64 value, u32 bits) {
    Big result = value & Mask(bits);
    if (value & (u64{1} << (bits - 1))) result -= Big{1} << bits;
    return result;
}
struct Expected { u64 value; bool saturated; };
Expected Reference(u64 lhs, u64 rhs, u32 bits, bool rounded) {
    Big numerator = 2 * Signed(lhs, bits) * Signed(rhs, bits);
    if (rounded) numerator += Big{1} << (bits - 1);
    const Big denominator = Big{1} << bits;
    // cpp_int division truncates toward zero; ARM discards the low half
    // using signed floor division, including for negative halfway inputs.
    Big quotient = numerator / denominator;
    if (numerator < 0 && numerator % denominator != 0) --quotient;
    const Big lower = -(Big{1} << (bits - 1)), upper = (Big{1} << (bits - 1)) - 1;
    Big result = quotient < lower ? lower : quotient > upper ? upper : quotient;
    const bool saturated = result != quotient;
    if (result < 0) result += Big{1} << bits;
    return {result.convert_to<u64>(), saturated};
}
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
    backend::State* state = new(storage.data()) backend::State{};
    Machine() {
        state->host_cpu_flags = 0xB0000000;
        Context().fpcr = 0x01400000;
        for (u32 reg = 0; reg < 32; ++reg)
            SetVector(reg, {0xDEF0123456789ABC ^ u64(reg) * 0x100000001,
                            0xCAFEFEDCBADF00D5 ^ u64(reg) * 0x100010001});
    }
    ~Machine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() { return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin); }
    void SetVector(u32 reg, HighVectorBits value) { std::memcpy(&Context().v[reg], value.data(), 16); }
    HighVectorBits GetVector(u32 reg) { HighVectorBits result{}; std::memcpy(result.data(), &Context().v[reg], 16); return result; }
};
struct Program {
    InstructionMemory memory;
    ir::Block block{0, 0x1000};
    ir::Assembler assembler{&block};
#if defined(SWIFT_HAS_X64_BACKEND)
    std::array<std::unique_ptr<backend::x64::Translator>, 2> native;
#endif
    explicit Program(u32 code) : memory(code) {
        swift::arm64::A64Decoder decoder(0x1000, &memory, &assembler);
        decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        for (u32 i = 0; i < 2; ++i) {
            native[i] = std::make_unique<backend::x64::Translator>(
                &block, i ? backend::x64::HostFeatures{} : backend::x64::HostFeatures::Detect());
            native[i]->Translate();
        }
#endif
    }
    void Run(Machine& machine, swift::arm64::InterruptReason reason = swift::arm64::InterruptReason::BRK) {
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto initial = machine.Context();
        const auto flags = machine.state->host_cpu_flags;
        const auto location = machine.state->current_loc;
#endif
        backend::interp::Interpreter{*machine.state, &block}.Run();
        REQUIRE(machine.Context().padding == u32(reason));
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto expected = machine.Context();
        const auto expected_flags = machine.state->host_cpu_flags;
        const auto expected_location = machine.state->current_loc;
        for (auto& translator : native) {
            machine.Context() = initial;
            machine.state->host_cpu_flags = flags;
            machine.state->current_loc = location;
            machine.state->halt_reason = HaltReason::None;
            translator->getCode<HaltReason(*)(backend::State*)>()(machine.state);
            REQUIRE(std::memcmp(&machine.Context(), &expected, sizeof(expected)) == 0);
            REQUIRE(machine.state->host_cpu_flags == expected_flags);
            REQUIRE(machine.state->current_loc.Value() == expected_location.Value());
        }
#endif
    }
};
using Registers = std::array<u32, 3>;
std::array<Registers, 5> Aliases(u32 rm) { return {{{0, 31, rm}, {31, 31, rm}, {rm, 31, rm}, {0, rm, rm}, {rm, rm, rm}}}; }
u32 Base(bool indexed, bool rounded) {
    return indexed ? (rounded ? NEON_SQRDMULH_byelement_scalar : NEON_SQDMULH_byelement_scalar)
                   : (rounded ? NEON_SQRDMULH_scalar : NEON_SQDMULH_scalar);
}
u32 Encode(u32 bits, Registers regs, bool indexed, bool rounded, u32 index = 0) {
    u32 code = Base(indexed, rounded) | ((bits == 16 ? 1u : 2u) << 22) | (regs[2] << 16) | (regs[1] << 5) | regs[0];
    if (indexed) {
        code |= ((index >> (bits == 16 ? 2 : 1)) & 1) << 11;
        code |= ((index >> (bits == 16 ? 1 : 0)) & 1) << 21;
        if (bits == 16) code |= (index & 1) << 20;
    }
    return code;
}
u64 Element(HighVectorBits vector, u32 bits, u32 index) {
    return (vector[index * bits / 64] >> (index * bits % 64)) & Mask(bits);
}
void PutElement(HighVectorBits& vector, u32 bits, u32 index, u64 value) {
    const u32 half = index * bits / 64, shift = index * bits % 64;
    vector[half] = (vector[half] & ~(Mask(bits) << shift)) | ((value & Mask(bits)) << shift);
}
void CheckCase(Program& program, u32 bits, Registers regs, bool rounded, u32 index,
               u64 lhs, u64 rhs, bool sticky) {
    Machine machine;
    auto left = machine.GetVector(regs[1]);
    PutElement(left, bits, 0, lhs); machine.SetVector(regs[1], left);
    auto right = machine.GetVector(regs[2]);
    PutElement(right, bits, index, rhs); machine.SetVector(regs[2], right);
    machine.Context().fpsr = 0x91 | (sticky ? kQC : 0);
    const auto before = machine.Context();
    const auto expected = Reference(machine.GetVector(regs[1])[0], Element(machine.GetVector(regs[2]), bits, index), bits, rounded);
    program.Run(machine);
    REQUIRE(machine.GetVector(regs[0]) == HighVectorBits{expected.value, 0});
    REQUIRE(machine.Context().fpsr == (0x91 | ((sticky || expected.saturated) ? kQC : 0)));
    REQUIRE(machine.Context().fpcr == before.fpcr);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    for (u32 reg = 0; reg < 32; ++reg) if (reg != regs[0])
        REQUIRE(std::memcmp(&machine.Context().v[reg], &before.v[reg], sizeof(before.v[reg])) == 0);
}
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
std::vector<u64> Edges(u32 bits) {
    const u64 sign = u64{1} << (bits - 1), mask = Mask(bits), half = u64{1} << (bits - 2);
    return {0, 1, 2, mask, mask - 1, sign, sign + 1, sign - 1,
            half - 1, half, half + 1, (0 - half - 1) & mask, (0 - half) & mask, (0 - half + 1) & mask};
}
}

TEST_CASE("ARM64 scalar high doubling multiply matches big integers at signed saturation and rounding ties") {
    for (u32 bits : {16u, 32u}) for (bool rounded : {false, true}) for (auto regs : Aliases(30)) {
        Program program(Encode(bits, regs, false, rounded));
        for (u64 lhs : Edges(bits)) for (u64 rhs : Edges(bits)) for (bool sticky : {false, true})
            CheckCase(program, bits, regs, rounded, 0, lhs, rhs, sticky);
    }
    for (u32 bits : {16u, 32u}) {
        const u64 half = u64{1} << (bits - 2), sign = u64{1} << (bits - 1);
        REQUIRE(Reference(1, half, bits, false).value == 0);
        REQUIRE(Reference(1, half, bits, true).value == 1);
        REQUIRE(Reference(Mask(bits), half, bits, false).value == Mask(bits));
        REQUIRE(Reference(Mask(bits), half, bits, true).value == 0);
        REQUIRE(Reference(sign, sign, bits, false).value == sign - 1);
        REQUIRE(Reference(sign, sign, bits, true).saturated);
    }
}

TEST_CASE("ARM64 scalar high doubling multiply matches deterministic random big integer products") {
    u64 random = 0xA0761D6478BD642F;
    for (u32 bits : {16u, 32u}) for (bool rounded : {false, true}) for (auto regs : Aliases(30)) {
        Program program(Encode(bits, regs, false, rounded));
        for (u32 sample = 0; sample < 256; ++sample) {
            const u64 lhs = Random(random), rhs = Random(random);
            for (bool sticky : {false, true}) CheckCase(program, bits, regs, rounded, 0, lhs, rhs, sticky);
        }
    }
}

TEST_CASE("ARM64 scalar indexed high doubling multiply covers all H L M indexes and source destination aliases") {
    u64 random = 0xE7037ED1A0B428DB;
    for (u32 bits : {16u, 32u}) for (bool rounded : {false, true})
        for (u32 index = 0; index < 128 / bits; ++index) for (auto regs : Aliases(bits == 16 ? 15 : 30)) {
            Program program(Encode(bits, regs, true, rounded, index));
            const u64 sign = u64{1} << (bits - 1), half = u64{1} << (bits - 2);
            const std::array<std::array<u64, 2>, 10> edges{{{sign, sign}, {sign, sign + 1}, {sign, sign - 1},
                {1, half - 1}, {1, half}, {1, half + 1}, {Mask(bits), half - 1},
                {Mask(bits), half}, {Mask(bits), half + 1}, {0, sign}}};
            for (const auto& pair : edges) for (bool sticky : {false, true})
                CheckCase(program, bits, regs, rounded, index, pair[0], pair[1], sticky);
            for (u32 sample = 0; sample < 64; ++sample) {
                const u64 lhs = Random(random), rhs = Random(random);
                CheckCase(program, bits, regs, rounded, index, lhs, rhs, sample & 1);
            }
        }
}

TEST_CASE("ARM64 scalar high doubling multiply rejects reserved widths without modifying guest vector state") {
    for (bool indexed : {false, true}) for (bool rounded : {false, true}) for (u32 size : {0u, 3u}) {
        Program program(Base(indexed, rounded) | (size << 22) | (15u << 16) | (31u << 5));
        Machine machine; machine.Context().fpsr = 0x08000091;
        const auto before = machine.Context();
        program.Run(machine, swift::arm64::InterruptReason::ILL_CODE);
        REQUIRE(std::memcmp(&machine.Context().v, &before.v, sizeof(before.v)) == 0);
        REQUIRE(machine.Context().fpsr == before.fpsr);
        REQUIRE(machine.Context().fpcr == before.fpcr);
        REQUIRE(machine.state->current_loc.Value() == 0x1000);
        REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    }
}
