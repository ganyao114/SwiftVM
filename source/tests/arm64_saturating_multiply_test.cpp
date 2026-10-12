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
        for (u32 reg = 0; reg < Context().r.size(); ++reg)
            Context().r[reg] = 0xA0123456789BCDEF ^ u64(reg) * 0x100000001;
        Context().fp = 0x8123456789ABCDEF;
        Context().lr = 0x76543210FEDCBA98;
        Context().sp = 0x1234567890ABCDEF;
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
struct Expected { u64 value; bool saturated; };
Expected Reference(u64 lhs, u64 rhs, u64 acc, u32 bits, u32 mode) {
    Big numerator = 2 * Signed(lhs, bits) * Signed(rhs, bits);
    if (mode == 2) numerator += Signed(acc, bits) * (Big{1} << bits);
    if (mode == 3) numerator = Signed(acc, bits) * (Big{1} << bits) - numerator;
    if (mode != 0) numerator += Big{1} << (bits - 1);
    const Big denominator = Big{1} << bits;
    Big quotient = numerator / denominator;
    if (numerator < 0 && numerator % denominator != 0) --quotient;
    const Big lower = -(Big{1} << (bits - 1)), upper = (Big{1} << (bits - 1)) - 1;
    Big result = quotient < lower ? lower : quotient > upper ? upper : quotient;
    const bool saturated = result != quotient;
    if (result < 0) result += Big{1} << bits;
    return {result.convert_to<u64>(), saturated};
}
using Registers = std::array<u32, 3>;
std::array<Registers, 5> Aliases(u32 rm) {
    return {{{0, 31, rm}, {31, 31, rm}, {rm, 31, rm}, {0, rm, rm}, {rm, rm, rm}}};
}
struct Form {
    u32 bits, mode;
    bool scalar, q, indexed;
    u32 index = 0;
    u32 Active() const { return scalar ? 1 : (q ? 128 : 64) / bits; }
};
u32 Base(Form form) {
    const std::array<u32, 4> reg{NEON_SQDMULH, NEON_SQRDMULH, NEON_SQRDMLAH, NEON_SQRDMLSH};
    const std::array<u32, 4> indexed{NEON_SQDMULH_byelement, NEON_SQRDMULH_byelement,
                                   NEON_SQRDMLAH_byelement, NEON_SQRDMLSH_byelement};
    return (form.indexed ? indexed : reg)[form.mode] |
           (form.scalar ? NEON_Q | NEONScalar : form.q ? NEON_Q : 0);
}
u32 Encode(Form form, Registers regs) {
    u32 code = Base(form) | ((form.bits == 16 ? 1u : 2u) << 22) |
               (regs[2] << 16) | (regs[1] << 5) | regs[0];
    if (form.indexed) {
        code |= ((form.index >> (form.bits == 16 ? 2 : 1)) & 1) << 11;
        code |= ((form.index >> (form.bits == 16 ? 1 : 0)) & 1) << 21;
        if (form.bits == 16) code |= (form.index & 1) << 20;
    }
    return code;
}
u64 Element(HighVectorBits value, u32 bits, u32 index) {
    return (value[index * bits / 64] >> (index * bits % 64)) & Mask(bits);
}
void PutElement(HighVectorBits& value, u32 bits, u32 index, u64 element) {
    const u32 half = index * bits / 64, shift = index * bits % 64;
    value[half] = (value[half] & ~(Mask(bits) << shift)) | ((element & Mask(bits)) << shift);
}
u64 Random(u64& state) {
    state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
    return state * 0x2545F4914F6CDD1D;
}
void CheckCase(Program& program, Form form, Registers regs, HighVectorBits lhs,
               HighVectorBits rhs, HighVectorBits accumulator, bool sticky) {
    Machine machine;
    // Writes intentionally overlap for alias configurations. Expected results
    // are derived from the final register snapshot, rather than the arguments.
    machine.SetVector(regs[0], accumulator);
    machine.SetVector(regs[1], lhs);
    machine.SetVector(regs[2], rhs);
    machine.Context().fpsr = 0x91 | (sticky ? kQC : 0);
    const auto before = machine.Context();
    const auto left = machine.GetVector(regs[1]), right = machine.GetVector(regs[2]);
    const auto old = machine.GetVector(regs[0]);
    HighVectorBits expected{};
    bool qc = sticky;
    for (u32 lane = 0; lane < form.Active(); ++lane) {
        const auto ref = Reference(Element(left, form.bits, lane),
            Element(right, form.bits, form.indexed ? form.index : lane),
            Element(old, form.bits, lane), form.bits, form.mode);
        PutElement(expected, form.bits, lane, ref.value);
        qc |= ref.saturated;
    }
    program.Run(machine);
    REQUIRE(machine.GetVector(regs[0]) == expected);
    REQUIRE(machine.Context().fpsr == (0x91 | (qc ? kQC : 0)));
    REQUIRE(machine.Context().fpcr == before.fpcr);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    REQUIRE(machine.state->current_loc.Value() == 0x1004);
    REQUIRE(machine.Context().r == before.r);
    REQUIRE(machine.Context().fp == before.fp);
    REQUIRE(machine.Context().lr == before.lr);
    REQUIRE(machine.Context().sp == before.sp);
    for (u32 reg = 0; reg < 32; ++reg) if (reg != regs[0])
        REQUIRE(std::memcmp(&machine.Context().v[reg], &before.v[reg], sizeof(before.v[reg])) == 0);
}
HighVectorBits Fill(u32 bits, u64 value) {
    HighVectorBits result{};
    for (u32 lane = 0; lane < 128 / bits; ++lane) PutElement(result, bits, lane, value);
    return result;
}
}

TEST_CASE("ARM64 saturating multiply high and RDM obey one final rounding and saturation") {
    for (u32 bits : {16u, 32u}) {
        const u64 sign = u64{1} << (bits - 1), half = u64{1} << (bits - 2), mask = Mask(bits);
        REQUIRE(Reference(sign, sign, mask, bits, 2).value == sign - 1);
        REQUIRE_FALSE(Reference(sign, sign, mask, bits, 2).saturated);
        REQUIRE(Reference(sign, sign, 1, bits, 3).value == sign + 1);
        REQUIRE_FALSE(Reference(sign, sign, 1, bits, 3).saturated);
        REQUIRE(Reference(mask, half, 0, bits, 3).value == 1);
        REQUIRE(Reference(1, half, 0, bits, 3).value == 0);
        const std::array<std::array<u64, 3>, 18> cases{{
            {sign, sign, mask}, {sign, sign, 1}, {sign, sign, 0}, {sign, sign, sign},
            {mask, half, 0}, {1, half, 0}, {1, half - 1, 0}, {1, half + 1, 0},
            {mask, half - 1, 0}, {mask, half + 1, 0}, {sign, sign - 1, sign - 1},
            {sign - 1, sign - 1, sign - 1}, {sign + 1, sign - 1, sign},
            {0, sign, mask}, {mask, mask, mask}, {1, 1, sign}, {2, half, mask}, {sign, mask, 1}}};
        for (u32 mode = 0; mode < 4; ++mode) for (u32 shape = 0; shape < 3; ++shape)
            for (bool indexed : {false, true}) {
                Form form{bits, mode, shape == 0, shape == 2, indexed, indexed ? 128 / bits - 1 : 0};
                Registers regs{0, 31, bits == 16 && indexed ? 15u : 30u};
                Program program(Encode(form, regs));
                for (auto values : cases) for (bool sticky : {false, true})
                    CheckCase(program, form, regs, Fill(bits, values[0]), Fill(bits, values[1]),
                              Fill(bits, values[2]), sticky);
            }
    }
}

TEST_CASE("ARM64 saturating multiply covers all indexed lanes aliases and random vector state") {
    u64 random = 0xD6E8FEB86659FD93;
    for (u32 bits : {16u, 32u}) for (u32 mode = 0; mode < 4; ++mode)
        for (u32 shape = 0; shape < 3; ++shape) for (bool indexed : {false, true})
            for (u32 index = 0; index < (indexed ? 128 / bits : 1); ++index)
                for (auto regs : Aliases(indexed && bits == 16 ? 15 : 30)) {
                    Form form{bits, mode, shape == 0, shape == 2, indexed, index};
                    Program program(Encode(form, regs));
                    for (u32 sample = 0; sample < 24; ++sample) {
                        const HighVectorBits lhs{Random(random), Random(random)};
                        const HighVectorBits rhs{Random(random), Random(random)};
                        const HighVectorBits acc{Random(random), Random(random)};
                        CheckCase(program, form, regs, lhs, rhs, acc, sample & 1);
                    }
                }
}

TEST_CASE("ARM64 saturating multiply ignores inactive lanes when accumulating QC") {
    for (u32 bits : {16u, 32u}) for (u32 mode = 0; mode < 4; ++mode)
        for (bool scalar : {false, true}) for (bool indexed : {false, true}) {
            const u64 sign = u64{1} << (bits - 1);
            Form form{bits, mode, scalar, false, indexed, 0};
            Registers regs{0, 31, 15};
            auto lhs = Fill(bits, sign), rhs = Fill(bits, sign);
            auto acc = Fill(bits, mode == 3 ? sign : sign - 1);
            for (u32 lane = 0; lane < form.Active(); ++lane) {
                PutElement(lhs, bits, lane, 0);
                PutElement(acc, bits, lane, 0);
            }
            Program program(Encode(form, regs));
            for (bool sticky : {false, true}) CheckCase(program, form, regs, lhs, rhs, acc, sticky);
        }
}

TEST_CASE("ARM64 saturating multiply rejects reserved widths without changing guest arithmetic state") {
    for (u32 mode = 0; mode < 4; ++mode) for (u32 shape = 0; shape < 3; ++shape)
        for (bool indexed : {false, true}) for (u32 size : {0u, 3u}) {
            Form form{16, mode, shape == 0, shape == 2, indexed};
            Program program(Base(form) | (size << 22) | (15u << 16) | (31u << 5));
            Machine machine; machine.Context().fpsr = kQC | 0x91;
            const auto before = machine.Context();
            program.Run(machine, swift::arm64::InterruptReason::ILL_CODE);
            REQUIRE(std::memcmp(&machine.Context().v, &before.v, sizeof(before.v)) == 0);
            REQUIRE(machine.Context().fpsr == before.fpsr);
            REQUIRE(machine.Context().fpcr == before.fpcr);
            REQUIRE(machine.state->current_loc.Value() == 0x1000);
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
}
