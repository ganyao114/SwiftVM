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
Big Magnitude(u16 value) {
    const u32 exponent = (value >> 10) & 31, fraction = value & 1023;
    return exponent ? Big{fraction | 1024} << (exponent - 1) : Big{fraction};
}
// Finite inputs use exact integer rational arithmetic. Quantization compares
// the exact remainder to a half-ULP and checks tininess before rounding.
Ref Reference(u16 a, u16 b, u32 op, u32 fpcr) {
    const bool absolute = op == 8;
    if (absolute) op = 1;
    auto finish = [&](u32 value, u32 flags) { return Ref{u16(absolute ? value & 0x7FFF : value), flags}; };
    auto nan = [](u16 value) { return (value & 0x7C00) == 0x7C00 && (value & 1023); };
    auto signaling = [&](u16 value) { return nan(value) && !(value & 512); };
    if (fpcr & (1u << 19)) {
        if (!(a & 0x7C00) && (a & 1023)) a &= 0x8000;
        if (!(b & 0x7C00) && (b & 1023)) b &= 0x8000;
    }
    const bool numeric = op >= 6, minimum = op & 1, snan = signaling(a) || signaling(b);
    if (snan || ((nan(a) || nan(b)) && (op < 4 || !numeric || (nan(a) && nan(b))))) {
        const u32 chosen = signaling(a) ? a : signaling(b) ? b : nan(a) ? a : b;
        return finish(fpcr & (1u << 25) ? 0x7E00 : chosen | 512, snan ? 1 : 0);
    }
    if (nan(a)) return finish(b, 0);
    if (nan(b)) return finish(a, 0);
    if (op >= 4) {
        if (!(a & 0x7FFF) && !(b & 0x7FFF)) return finish(minimum ? a | b : a & b, 0);
        const bool less = bool(a & 0x8000) != bool(b & 0x8000) ? bool(a & 0x8000)
                         : (a & 0x8000) ? a > b : a < b;
        return finish(minimum == less ? a : b, 0);
    }
    const bool ainf = (a & 0x7FFF) == 0x7C00, binf = (b & 0x7FFF) == 0x7C00;
    const bool azero = !(a & 0x7FFF), bzero = !(b & 0x7FFF);
    const bool asign = a & 0x8000;
    bool bsign = b & 0x8000, negative;
    const u32 mode = (fpcr >> 22) & 3;
    Big numerator, denominator;
    if (op < 2) {
        if (op == 1) bsign = !bsign;
        if (ainf || binf) {
            if (ainf && binf && asign != bsign) return finish(0x7E00, 1);
            return finish(((ainf ? asign : bsign) ? 0x8000 : 0) | 0x7C00, 0);
        }
        Big left = Magnitude(a), right = Magnitude(b);
        if (asign) left = -left;
        if (bsign) right = -right;
        numerator = left + right;
        denominator = Big{1} << 24;
        if (numerator == 0)
            return finish((asign == bsign ? asign : mode == 2) ? 0x8000 : 0, 0);
        negative = numerator < 0;
        if (negative) numerator = -numerator;
    } else {
        negative = asign != bsign;
        if (op == 2) {
            if ((ainf && bzero) || (binf && azero)) return finish(0x7E00, 1);
            if (ainf || binf) return finish((negative ? 0x8000 : 0) | 0x7C00, 0);
            numerator = Magnitude(a) * Magnitude(b);
            denominator = Big{1} << 48;
        } else {
            if ((azero && bzero) || (ainf && binf)) return finish(0x7E00, 1);
            if (ainf || bzero)
                return finish((negative ? 0x8000 : 0) | 0x7C00, bzero && !ainf ? 2 : 0);
            if (binf) return finish(negative ? 0x8000 : 0, 0);
            numerator = Magnitude(a);
            denominator = Magnitude(b);
        }
    }
    const u32 sign = negative ? 0x8000 : 0;
    if (numerator == 0) return finish(sign, 0);
    numerator <<= 24;
    const bool tiny = numerator < denominator * 1024;
    if (tiny && (fpcr & (1u << 19))) return finish(sign, 8);
    s32 top = s32(boost::multiprecision::msb(numerator)) - s32(boost::multiprecision::msb(denominator));
    if (top >= 0 ? numerator < (denominator << top) : (numerator << -top) < denominator) --top;
    u32 shift = top > 10 ? u32(top - 10) : 0;
    const Big divisor = denominator << shift;
    Big rounded = numerator / divisor, remainder = numerator % divisor;
    u32 flags = 0;
    if (remainder != 0) {
        flags |= 16 | (tiny ? 8 : 0);
        bool increment = (mode == 1 && !negative) || (mode == 2 && negative);
        if (mode == 0) increment = 2 * remainder > divisor || (2 * remainder == divisor && bool(rounded & 1));
        if (increment) ++rounded;
    }
    if (rounded >= 2048) { rounded >>= 1; ++shift; }
    const u32 exponent = rounded < 1024 ? 0 : shift + 1;
    if (exponent >= 31) {
        const bool infinity = mode == 0 || (mode == 1 && !negative) || (mode == 2 && negative);
        return finish(sign | (infinity ? 0x7C00 : 0x7BFF), flags | 20);
    }
    return finish(sign | (exponent << 10) | (rounded.convert_to<u32>() & 1023), flags);
}
using Lanes = std::array<u16, 8>;
using Registers = std::array<u32, 3>;
const std::array<Registers, 5> Aliases{{{0,31,30}, {31,31,30}, {30,31,30}, {0,30,30}, {30,30,30}}};
const std::array<u32, 8> Controls{0, 1u << 22, 2u << 22, 3u << 22,
    1u << 24, 1u << 25, (1u << 19) | (1u << 25), 1u << 19};
u32 Encode(u32 op, u32 shape, Registers regs) {
    const std::array<u32, 9> bases{NEON_FADD_H, NEON_FSUB_H, NEON_FMUL_H, NEON_FDIV_H,
        NEON_FMAX_H, NEON_FMIN_H, NEON_FMAXNM_H, NEON_FMINNM_H, NEON_FABD_H};
    return bases[op] | (shape == 2 ? NEON_Q | NEONScalar : shape == 1 ? NEON_Q : 0) |
           (regs[2] << 16) | (regs[1] << 5) | regs[0];
}
void CheckCase(Program& program, u32 op, u32 shape, Registers regs, Lanes lhs, Lanes rhs, u32 fpcr) {
    Machine machine;
    std::memcpy(&machine.Context().v[regs[1]], lhs.data(), sizeof(lhs));
    std::memcpy(&machine.Context().v[regs[2]], rhs.data(), sizeof(rhs));
    machine.Context().fpcr = fpcr;
    machine.Context().fpsr = 0x08000002;
    const auto before = machine.Context();
    std::memcpy(lhs.data(), &before.v[regs[1]], sizeof(lhs));
    std::memcpy(rhs.data(), &before.v[regs[2]], sizeof(rhs));
    Lanes expected{};
    u32 flags = before.fpsr;
    for (u32 lane = 0; lane < (shape == 2 ? 1u : shape == 1 ? 8u : 4u); ++lane) {
        const auto result = Reference(lhs[lane], rhs[lane], op, fpcr);
        expected[lane] = result.value;
        flags |= result.flags;
    }
    program.Run(machine);
    REQUIRE(std::memcmp(&machine.Context().v[regs[0]], expected.data(), sizeof(expected)) == 0);
    REQUIRE(machine.Context().fpsr == flags);
    REQUIRE(machine.Context().fpcr == before.fpcr);
    REQUIRE(machine.Context().r == before.r);
    REQUIRE(machine.Context().fp == before.fp);
    REQUIRE(machine.Context().lr == before.lr);
    REQUIRE(machine.Context().sp == before.sp);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    REQUIRE(machine.state->current_loc.Value() == 0x1004);
    for (u32 reg = 0; reg < 32; ++reg) if (reg != regs[0])
        REQUIRE(std::memcmp(&machine.Context().v[reg], &before.v[reg], sizeof(before.v[reg])) == 0);
}
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 FP16 binary operations and scalar FABD match independent rational rounding at exceptional boundaries") {
    const std::array<u16, 22> edges{0,0x8000,1,0x8001,0x03FF,0x83FF,0x0400,0x8400,0x0401,
        0x3C00,0xBC00,0x3C01,0x3555,0x1000,0x7BFF,0xFBFF,0x7C00,0xFC00,0x7E03,0xFE05,0x7C01,0xFC03};
    for (u32 op = 0; op < 9; ++op) for (u32 shape = 0; shape < (op == 8 ? 3u : 2u); ++shape) {
        Program program(Encode(op, shape, Aliases[0]));
        for (u32 control : Controls) for (u32 start = 0; start < edges.size() * edges.size(); start += 8) {
            Lanes lhs{}, rhs{};
            for (u32 lane = 0; lane < 8; ++lane) {
                const u32 pair = (start + lane) % (edges.size() * edges.size());
                lhs[lane] = edges[pair / edges.size()]; rhs[lane] = edges[pair % edges.size()];
            }
            CheckCase(program, op, shape, Aliases[0], lhs, rhs, control);
            if (shape == 2) for (u32 lane = 1; lane < 8; ++lane) {
                auto scalar_lhs = lhs, scalar_rhs = rhs;
                scalar_lhs[0] = lhs[lane]; scalar_rhs[0] = rhs[lane];
                CheckCase(program, op, shape, Aliases[0], scalar_lhs, scalar_rhs, control);
            }
        }
    }
}

TEST_CASE("ARM64 FP16 binary operations preserve aliased source snapshots complete state and sticky flags") {
    u64 random = 0x510E527FADE682D1;
    for (u32 op = 0; op < 9; ++op) for (u32 shape = 0; shape < (op == 8 ? 3u : 2u); ++shape)
        for (auto regs : Aliases) {
            Program program(Encode(op, shape, regs));
            for (u32 control : Controls) for (u32 sample = 0; sample < 64; ++sample) {
                Lanes lhs{}, rhs{};
                for (u32 lane = 0; lane < 8; ++lane) { lhs[lane] = u16(Random(random)); rhs[lane] = u16(Random(random)); }
                CheckCase(program, op, shape, regs, lhs, rhs, control);
            }
        }
}

TEST_CASE("ARM64 FP16 binary inactive scalar and Q0 lanes cannot raise floating point exceptions") {
    for (u32 op = 0; op < 9; ++op) for (u32 shape : {0u, 2u}) {
        if (shape == 2 && op != 8) continue;
        Lanes lhs{}, rhs{};
        for (u32 lane = 0; lane < 8; ++lane) {
            lhs[lane] = lane < (shape == 2 ? 1u : 4u) ? 0 : 0x7C01;
            rhs[lane] = lane < (shape == 2 ? 1u : 4u) ? 0x3C00 : 0xFC03;
        }
        for (auto regs : Aliases) {
            Program program(Encode(op, shape, regs));
            for (u32 control : Controls) CheckCase(program, op, shape, regs, lhs, rhs, control);
        }
    }
}

TEST_CASE("ARM64 FP16 binary unsupported controls and remaining allocated operations precisely fall back") {
    for (u32 op = 0; op < 9; ++op) for (u32 shape = 0; shape < (op == 8 ? 3u : 2u); ++shape) {
        Program program(Encode(op, shape, Aliases[1]));
        for (u32 control : {1u,2u,1u << 8,1u << 9,1u << 10,1u << 11,1u << 12,1u << 15,1u << 26,1u << 31}) {
            Machine machine; machine.Context().fpcr = control; machine.Context().fpsr = 0x08000091;
            const auto before = machine.Context();
            program.Run(machine, swift::arm64::InterruptReason::FALLBACK);
            REQUIRE(std::memcmp(&machine.Context().v, &before.v, sizeof(before.v)) == 0);
            REQUIRE(machine.Context().r == before.r);
            REQUIRE(machine.Context().fpsr == before.fpsr);
            REQUIRE(machine.Context().fpcr == before.fpcr);
            REQUIRE(machine.Context().pc == 0x1000);
            REQUIRE(machine.state->current_loc.Value() == 0x1000);
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
    }
}
