#include <array>
#include <cstring>
#include <memory>
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
using MultiplyVectorBits = std::array<u64, 2>;
using Big = boost::multiprecision::cpp_int;
constexpr u32 kQC = 1u << 27;
u64 Mask(u32 bits) { return bits == 64 ? ~u64{0} : (u64{1} << bits) - 1; }
Big Signed(u64 raw, u32 bits) {
    Big result = raw & Mask(bits);
    if ((raw >> (bits - 1)) & 1) result -= Big{1} << bits;
    return result;
}
struct Expected { u64 value; bool product_saturated, accumulator_saturated; };
Expected Reference(u64 lhs, u64 rhs, u64 old, u32 bits, u32 operation) {
    const u32 out_bits = bits * 2;
    const Big lower = -(Big{1} << (out_bits - 1));
    const Big upper = (Big{1} << (out_bits - 1)) - 1;
    auto clamp = [&](const Big& value) -> Big { return value < lower ? lower : value > upper ? upper : value; };
    const Big exact_product = 2 * Signed(lhs, bits) * Signed(rhs, bits);
    const Big product = clamp(exact_product);
    Big exact_result = product;
    if (operation == 1) exact_result = Signed(old, out_bits) + product;
    if (operation == 2) exact_result = Signed(old, out_bits) - product;
    const Big result = clamp(exact_result);
    Big encoded = result;
    if (encoded < 0) encoded += Big{1} << out_bits;
    return {encoded.convert_to<u64>(), product != exact_product, result != exact_result};
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
    void SetVector(u32 reg, MultiplyVectorBits value) { std::memcpy(&Context().v[reg], value.data(), 16); }
    MultiplyVectorBits GetVector(u32 reg) { MultiplyVectorBits result{}; std::memcpy(result.data(), &Context().v[reg], 16); return result; }
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

constexpr std::array<u32, 3> kRegisterOps{NEON_SQDMULL_scalar, NEON_SQDMLAL_scalar, NEON_SQDMLSL_scalar};
constexpr std::array<u32, 3> kIndexedOps{NEON_SQDMULL_byelement_scalar, NEON_SQDMLAL_byelement_scalar, NEON_SQDMLSL_byelement_scalar};
using Registers = std::array<u32, 3>;  // Rd, Rn, Rm.
std::array<Registers, 5> Aliases(u32 rm) { return {{{0, 31, rm}, {31, 31, rm}, {rm, 31, rm}, {0, rm, rm}, {rm, rm, rm}}}; }
u32 Encode(u32 operation, u32 bits, Registers regs, bool indexed, u32 index = 0) {
    const u32 size = bits == 16 ? 1 : 2;
    u32 code = (indexed ? kIndexedOps[operation] : kRegisterOps[operation]) | (size << 22) | (regs[1] << 5) | regs[0];
    if (!indexed) return code | (regs[2] << 16);
    code |= ((index >> (bits == 16 ? 2 : 1)) & 1) << 11;  // H.
    code |= ((index >> (bits == 16 ? 1 : 0)) & 1) << 21;  // L.
    return code | (regs[2] << 16) | (bits == 16 ? (index & 1) << 20 : 0);
}
u64 Element(MultiplyVectorBits vector, u32 bits, u32 index) {
    const u32 position = bits * index;
    return (vector[position / 64] >> (position % 64)) & Mask(bits);
}
void PutElement(MultiplyVectorBits& vector, u32 bits, u32 index, u64 value) {
    const u32 position = bits * index, half = position / 64, shift = position % 64;
    vector[half] = (vector[half] & ~(Mask(bits) << shift)) | ((value & Mask(bits)) << shift);
}
void CheckCase(Program& program, u32 bits, u32 operation, Registers regs, bool indexed,
               u32 index, u64 lhs, u64 rhs, u64 accumulator, bool sticky) {
    Machine machine;
    auto old = machine.GetVector(regs[0]);
    old[0] = (accumulator & Mask(bits * 2)) | (~Mask(bits * 2) & 0xC0FFEE12BADF00D5);
    machine.SetVector(regs[0], old);
    auto left = machine.GetVector(regs[1]);
    PutElement(left, bits, 0, lhs);
    machine.SetVector(regs[1], left);
    auto right = machine.GetVector(regs[2]);
    PutElement(right, bits, indexed ? index : 0, rhs);
    machine.SetVector(regs[2], right);
    machine.Context().fpsr = 0x91 | (sticky ? kQC : 0);
    // Read the actual snapshots after all fixture writes: aliasing can make
    // a source also provide the accumulator or the indexed element.
    const auto before = machine.Context();
    const auto expected = Reference(machine.GetVector(regs[1])[0],
        Element(machine.GetVector(regs[2]), bits, indexed ? index : 0),
        machine.GetVector(regs[0])[0], bits, operation);
    program.Run(machine);
    REQUIRE(machine.GetVector(regs[0]) == MultiplyVectorBits{expected.value, 0});
    REQUIRE(machine.Context().fpsr == (0x91 | ((sticky || expected.product_saturated || expected.accumulator_saturated) ? kQC : 0)));
    REQUIRE(machine.Context().fpcr == before.fpcr);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    for (u32 reg = 0; reg < 32; ++reg) if (reg != regs[0])
        REQUIRE(std::memcmp(&machine.Context().v[reg], &before.v[reg], sizeof(before.v[reg])) == 0);
}
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 scalar doubling long multiply saturates product before accumulator and records either QC") {
    for (u32 bits : {16u, 32u}) {
        const u64 source_sign = u64{1} << (bits - 1), out_sign = u64{1} << (bits * 2 - 1);
        const std::array<u64, 7> values{0, 1, source_sign - 1, source_sign, source_sign + 1, Mask(bits) - 1, Mask(bits)};
        const std::array<u64, 8> accumulators{0, 1, 2, out_sign - 1, out_sign, out_sign + 1, Mask(bits * 2) - 1, Mask(bits * 2)};
        for (u32 operation = 0; operation < 3; ++operation) for (auto regs : Aliases(15)) {
            Program program(Encode(operation, bits, regs, false));
            for (u64 lhs : values) for (u64 rhs : values) for (u64 accumulator : accumulators)
                for (bool sticky : {false, true}) CheckCase(program, bits, operation, regs, false, 0, lhs, rhs, accumulator, sticky);
        }
        // These values differ from a fused clamp of accumulator +/- 2*a*b.
        REQUIRE(Reference(source_sign, source_sign, Mask(bits * 2), bits, 1).value == out_sign - 2);
        REQUIRE(Reference(source_sign, source_sign, 0, bits, 2).value == out_sign + 1);
        REQUIRE(Reference(source_sign, source_sign, Mask(bits * 2), bits, 1).product_saturated);
        REQUIRE(Reference(1, 1, out_sign - 1, bits, 1).accumulator_saturated);
        REQUIRE(Reference(1, 1, out_sign, bits, 2).accumulator_saturated);
    }
}

TEST_CASE("ARM64 scalar doubling long multiply matches independent big integers on deterministic random inputs") {
    u64 random = 0x8CB92BA72F3D8DD7;
    for (u32 bits : {16u, 32u}) for (u32 operation = 0; operation < 3; ++operation)
        for (auto regs : Aliases(30)) {
            Program program(Encode(operation, bits, regs, false));
            for (u32 sample = 0; sample < 128; ++sample) {
                const u64 lhs = Random(random), rhs = Random(random), old = Random(random);
                for (bool sticky : {false, true}) CheckCase(program, bits, operation, regs, false, 0, lhs, rhs, old, sticky);
            }
        }
}

TEST_CASE("ARM64 scalar indexed doubling long multiply covers every H L M selection with aliases and poison") {
    u64 random = 0xD1B54A32D192ED03;
    for (u32 bits : {16u, 32u}) for (u32 operation = 0; operation < 3; ++operation)
        for (u32 index = 0; index < 128 / bits; ++index) for (auto regs : Aliases(bits == 16 ? 15 : 30)) {
            Program program(Encode(operation, bits, regs, true, index));
            const u64 sign = u64{1} << (bits - 1), out_sign = u64{1} << (bits * 2 - 1);
            const std::array<std::array<u64, 3>, 10> cases{{{sign, sign, Mask(bits * 2)}, {sign, sign, 0},
                {sign, sign, out_sign - 1}, {1, 1, out_sign - 1}, {1, 1, out_sign},
                {Mask(bits), 1, out_sign}, {Mask(bits), 1, out_sign - 1}, {sign, sign - 1, out_sign},
                {sign + 1, sign + 1, 0}, {0, sign, Mask(bits * 2)}}};
            for (const auto& values : cases) for (bool sticky : {false, true})
                CheckCase(program, bits, operation, regs, true, index, values[0], values[1], values[2], sticky);
            for (u32 sample = 0; sample < 32; ++sample) {
                const u64 lhs = Random(random), rhs = Random(random), old = Random(random);
                CheckCase(program, bits, operation, regs, true, index, lhs, rhs, old, sample & 1);
            }
        }
}

TEST_CASE("ARM64 scalar doubling long multiply rejects reserved byte and doubleword widths without mutation") {
    for (bool indexed : {false, true}) for (u32 operation = 0; operation < 3; ++operation)
        for (u32 size : {0u, 3u}) {
            Program program((indexed ? kIndexedOps[operation] : kRegisterOps[operation]) | (size << 22) | (15u << 16) | (31u << 5));
            Machine machine;
            machine.Context().fpsr = 0x08000091;
            const auto before = machine.Context();
            program.Run(machine, swift::arm64::InterruptReason::ILL_CODE);
            REQUIRE(std::memcmp(&machine.Context().v, &before.v, sizeof(before.v)) == 0);
            REQUIRE(machine.Context().fpsr == before.fpsr);
            REQUIRE(machine.Context().fpcr == before.fpcr);
            REQUIRE(machine.state->current_loc.Value() == 0x1000);
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
}
