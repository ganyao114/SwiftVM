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
struct Ref { u16 value; u32 flags; };
Big IntegerRoot(const Big& value) {
    Big root = Big{1} << ((boost::multiprecision::msb(value) + 2) / 2);
    for (;;) {
        Big next = (root + value / root) / 2;
        if (next >= root) return root;
        root = next;
    }
}
// Half values are exact integers in units of 2^-24. Square roots compare
// squared integer midpoints, with no host floating point or SoftFloat calls.
Ref Reference(u16 value, u32 op, u32 fpcr) {
    const u32 sign = value & 0x8000, exponent = (value >> 10) & 31;
    u32 fraction = value & 1023;
    if (exponent == 31 && fraction)
        return {u16(fpcr & (1u << 25) ? 0x7E00 : value | 512), fraction & 512 ? 0u : 1u};
    if (!exponent && fraction && (fpcr & (1u << 19))) { value = sign; fraction = 0; }
    if (!(value & 0x7FFF)) return {value, 0};
    if (exponent == 31)
        return sign && op == 0 ? Ref{0x7E00, 1} : Ref{value, 0};
    const Big magnitude = exponent ? Big{fraction | 1024} << (exponent - 1) : Big{fraction};
    u32 mode = (fpcr >> 22) & 3;
    if (op == 0) {
        if (sign) return {0x7E00, 1};
        const Big target = magnitude << 24;
        const Big root = IntegerRoot(target);
        const u32 top = boost::multiprecision::msb(root);
        u32 shift = top > 10 ? top - 10 : 0;
        Big rounded = root >> shift;
        const Big encoded = rounded << shift;
        const bool exact = encoded * encoded == target;
        bool increment = mode == 1 && !exact;
        if (mode == 0) {
            const Big midpoint = (2 * rounded + 1) << shift;
            increment = 4 * target > midpoint * midpoint ||
                        (4 * target == midpoint * midpoint && bool(rounded & 1));
        }
        if (increment) ++rounded;
        if (rounded >= 2048) { rounded >>= 1; ++shift; }
        return {u16(((shift + 1) << 10) | (rounded.convert_to<u32>() & 1023)), exact ? 0u : 16u};
    }
    switch (op) {
        case 1: mode = 0; break;
        case 2: mode = 2; break;
        case 3: mode = 1; break;
        case 4: mode = 3; break;
        case 5: mode = 4; break;
        default: break;
    }
    Big rounded = magnitude >> 24, remainder = magnitude - (rounded << 24);
    if (remainder != 0) {
        bool increment = (mode == 1 && !sign) || (mode == 2 && sign);
        if (mode == 0 || mode == 4)
            increment = remainder > (Big{1} << 23) ||
                        (remainder == (Big{1} << 23) && (mode == 4 || bool(rounded & 1)));
        if (increment) ++rounded;
    }
    const u32 flags = remainder != 0 && op == 6 ? 16 : 0;
    if (rounded == 0) return {u16(sign), flags};
    const u32 top = boost::multiprecision::msb(rounded);
    const u32 significand = ((rounded << 10) >> top).convert_to<u32>();
    return {u16(sign | ((top + 15) << 10) | (significand & 1023)), flags};
}
u32 Encode(u32 op, bool q, u32 rd) {
    const std::array<u32, 8> bases{NEON_FSQRT_H, NEON_FRINTN_H, NEON_FRINTM_H, NEON_FRINTP_H,
        NEON_FRINTZ_H, NEON_FRINTA_H, NEON_FRINTX_H, NEON_FRINTI_H};
    return bases[op] | (q ? NEON_Q : 0) | (31u << 5) | rd;
}
using Lanes = std::array<u16, 8>;
const std::array<u32, 8> Controls{0, 1u << 22, 2u << 22, 3u << 22,
    1u << 24, 1u << 25, (1u << 19) | (1u << 25), 1u << 19};
void CheckCase(Program& program, u32 op, bool q, u32 rd, Lanes source, u32 fpcr) {
    Machine machine;
    std::memcpy(&machine.Context().v[31], source.data(), sizeof(source));
    machine.Context().fpcr = fpcr;
    machine.Context().fpsr = 0x08000002;
    const auto before = machine.Context();
    Lanes expected{};
    u32 flags = before.fpsr;
    for (u32 lane = 0; lane < (q ? 8u : 4u); ++lane) {
        const auto result = Reference(source[lane], op, fpcr);
        expected[lane] = result.value;
        flags |= result.flags;
    }
    program.Run(machine);
    REQUIRE(std::memcmp(&machine.Context().v[rd], expected.data(), sizeof(expected)) == 0);
    REQUIRE(machine.Context().fpsr == flags);
    REQUIRE(machine.Context().fpcr == before.fpcr);
    REQUIRE(machine.Context().r == before.r);
    REQUIRE(machine.Context().fp == before.fp);
    REQUIRE(machine.Context().lr == before.lr);
    REQUIRE(machine.Context().sp == before.sp);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    REQUIRE(machine.state->current_loc.Value() == 0x1004);
    for (u32 reg = 0; reg < 32; ++reg) if (reg != rd)
        REQUIRE(std::memcmp(&machine.Context().v[reg], &before.v[reg], sizeof(before.v[reg])) == 0);
}
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 vector FP16 sqrt and rounding cover every half bit pattern against integer arithmetic") {
    for (u32 op = 0; op < 8; ++op) {
        Program program(Encode(op, true, 31));
        for (u32 start = 0; start < 65536; start += 8) {
            Lanes source{};
            for (u32 lane = 0; lane < 8; ++lane) source[lane] = u16(start + lane);
            CheckCase(program, op, true, 31, source, 0);
        }
    }
}

TEST_CASE("ARM64 vector FP16 unary all rounding controls aliases and NaN subnormal edge states") {
    const std::array<Lanes, 7> directed{{
        {0x0000,0x8000,0x3C00,0x4000,0x4200,0x4400,0x7C00,0xFC00},
        {0x3800,0xB800,0x3E00,0xBE00,0x4100,0xC100,0x4300,0xC300},
        {0x37FF,0x3801,0xB7FF,0xB801,0x3DFF,0x3E01,0xBDFF,0xBE01},
        {0x0001,0x8001,0x03FF,0x83FF,0x0400,0x8400,0x0401,0x8401},
        {0x7E03,0xFE05,0x7C01,0xFC03,0x7FFF,0xFFFF,0x7DFF,0xFDFF},
        {0x3C00,0x4000,0x4400,0x4800,0x7C01,0xBC00,0x7DFF,0xFC00},
        {0x3555,0x3955,0x3C01,0x3BFF,0x7BFF,0x7BFE,0x5801,0xD801}}};
    u64 random = 0xA54FF53A5F1D36F1;
    for (u32 op = 0; op < 8; ++op) for (bool q : {false, true}) for (u32 rd : {0u, 30u, 31u}) {
        Program program(Encode(op, q, rd));
        for (u32 control : Controls) {
            for (const auto& source : directed) CheckCase(program, op, q, rd, source, control);
            for (u32 sample = 0; sample < 64; ++sample) {
                Lanes source{};
                for (auto& value : source) value = u16(Random(random));
                CheckCase(program, op, q, rd, source, control);
            }
        }
    }
}

TEST_CASE("ARM64 vector FP16 unary inactive lanes cannot raise floating point exceptions") {
    const Lanes source{0, 0x8000, 0x3C00, 0x4400, 0x7C01, 0xBC00, 0xFDFF, 0xFC00};
    for (u32 op = 0; op < 8; ++op) for (u32 rd : {0u, 31u}) {
        Program program(Encode(op, false, rd));
        for (u32 control : Controls) CheckCase(program, op, false, rd, source, control);
    }
}

TEST_CASE("ARM64 vector FP16 unary unsupported controls exit before modifying guest arithmetic state") {
    for (u32 op = 0; op < 8; ++op) for (bool q : {false, true}) {
        Program program(Encode(op, q, 31));
        for (u32 control : {1u, 2u, 1u << 8, 1u << 9, 1u << 10, 1u << 11, 1u << 12,
                            1u << 15, 1u << 26, 1u << 31}) {
            Machine machine;
            machine.Context().fpcr = control;
            machine.Context().fpsr = 0x08000091;
            const auto before = machine.Context();
            program.Run(machine, swift::arm64::InterruptReason::FALLBACK);
            REQUIRE(std::memcmp(&machine.Context().v, &before.v, sizeof(before.v)) == 0);
            REQUIRE(machine.Context().r == before.r);
            REQUIRE(machine.Context().fpsr == before.fpsr);
            REQUIRE(machine.Context().fpcr == before.fpcr);
            REQUIRE(machine.state->current_loc.Value() == 0x1000);
            REQUIRE(machine.Context().pc == 0x1000);
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
    }
}
