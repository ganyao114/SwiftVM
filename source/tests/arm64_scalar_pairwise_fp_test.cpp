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
struct FloatFormat {
    u32 bits, fraction, bias, max_exponent;
    u64 sign, fraction_mask, exponent_mask, quiet;
    explicit FloatFormat(u32 width) : bits(width), fraction(width == 16 ? 10 : width == 32 ? 23 : 52),
        bias(width == 16 ? 15 : width == 32 ? 127 : 1023), max_exponent(2 * bias + 1),
        sign(u64{1} << (width - 1)), fraction_mask((u64{1} << fraction) - 1),
        exponent_mask(u64(max_exponent) << fraction), quiet(u64{1} << (fraction - 1)) {}
    bool NaN(u64 value) const { return (value & exponent_mask) == exponent_mask && (value & fraction_mask); }
    bool Signaling(u64 value) const { return NaN(value) && !(value & quiet); }
    bool Subnormal(u64 value) const { return !(value & exponent_mask) && (value & fraction_mask); }
    bool Flush(u32 fpcr) const { return fpcr & (1u << (bits == 16 ? 19 : 24)); }
    Big ExactMagnitude(u64 value) const {
        const u32 exponent = (value & exponent_mask) >> fraction;
        Big magnitude = value & fraction_mask;
        if (exponent) magnitude = (magnitude + (Big{1} << fraction)) << (exponent - 1);
        return magnitude;
    }
};
struct Ref { u64 value; u32 flags; };
// Every finite float is an integer multiple of the minimum subnormal. This
// reference adds those unbounded integers and rounds exactly once to H/S/D.
Ref Reference(u64 a, u64 b, u32 bits, u32 operation, u32 fpcr) {
    const FloatFormat f(bits);
    u32 flags = 0;
    for (u64* value : {&a, &b}) if (f.Flush(fpcr) && f.Subnormal(*value)) {
        *value &= f.sign;
        if (bits != 16) flags |= 128;
    }
    const bool numeric = operation >= 6, minimum = operation & 1;
    const bool signaling = f.Signaling(a) || f.Signaling(b);
    if (signaling || ((f.NaN(a) || f.NaN(b)) &&
                     (operation == 2 || !numeric || (f.NaN(a) && f.NaN(b))))) {
        if (signaling) flags |= 1;
        u64 selected = f.Signaling(a) ? a : f.Signaling(b) ? b : f.NaN(a) ? a : b;
        return {fpcr & (1u << 25) ? f.exponent_mask | f.quiet : selected | f.quiet, flags};
    }
    if (f.NaN(a)) return {b, flags};
    if (f.NaN(b)) return {a, flags};
    if (operation != 2) {
        if (!(a & ~f.sign) && !(b & ~f.sign)) return {minimum ? a | b : a & b, flags};
        const bool less = bool(a & f.sign) != bool(b & f.sign) ? bool(a & f.sign)
                         : (a & f.sign) ? a > b : a < b;
        return {minimum == less ? a : b, flags};
    }
    const bool ainf = (a & ~f.sign) == f.exponent_mask, binf = (b & ~f.sign) == f.exponent_mask;
    if (ainf || binf) {
        if (ainf && binf && bool(a & f.sign) != bool(b & f.sign))
            return {f.exponent_mask | f.quiet, flags | 1};
        return {ainf ? a : b, flags};
    }
    Big sum = (a & f.sign ? -f.ExactMagnitude(a) : f.ExactMagnitude(a)) +
              (b & f.sign ? -f.ExactMagnitude(b) : f.ExactMagnitude(b));
    const u32 rounding = (fpcr >> 22) & 3;
    if (sum == 0) {
        const u64 sign = bool(a & f.sign) == bool(b & f.sign) ? a & f.sign
                       : rounding == 2 ? f.sign : 0;
        return {sign, flags};
    }
    const bool negative = sum < 0;
    if (negative) sum = -sum;
    const u64 sign = negative ? f.sign : 0;
    if (f.Flush(fpcr) && sum < (Big{1} << f.fraction)) return {sign, flags | 8};
    const u32 top = boost::multiprecision::msb(sum);
    u32 shift = top > f.fraction ? top - f.fraction : 0;
    Big rounded = sum >> shift, remainder = sum - (rounded << shift);
    if (remainder != 0) {
        flags |= 16;
        bool increment = rounding == 1 ? !negative : rounding == 2 ? negative : false;
        if (rounding == 0) {
            const Big half = Big{1} << (shift - 1);
            increment = remainder > half || (remainder == half && bool(rounded & 1));
        }
        if (increment) ++rounded;
    }
    if (rounded >= (Big{1} << (f.fraction + 1))) { rounded >>= 1; ++shift; }
    const u32 exponent = rounded < (Big{1} << f.fraction) ? 0 : shift + 1;
    if (exponent >= f.max_exponent) {
        const bool infinity = rounding == 0 || (rounding == 1 && !negative) || (rounding == 2 && negative);
        return {sign | (infinity ? f.exponent_mask : f.exponent_mask - 1), flags | 20};
    }
    return {sign | (u64(exponent) << f.fraction) | (rounded.convert_to<u64>() & f.fraction_mask), flags};
}
u32 Encode(u32 bits, u32 operation, u32 rd, u32 rn) {
    u32 base;
    if (bits == 16) {
        switch (operation) {
            case 2: base = NEON_FADDP_h_scalar; break;
            case 4: base = NEON_FMAXP_h_scalar; break;
            case 5: base = NEON_FMINP_h_scalar; break;
            case 6: base = NEON_FMAXNMP_h_scalar; break;
            default: base = NEON_FMINNMP_h_scalar; break;
        }
    } else {
        switch (operation) {
            case 2: base = NEON_FADDP_scalar; break;
            case 4: base = NEON_FMAXP_scalar; break;
            case 5: base = NEON_FMINP_scalar; break;
            case 6: base = NEON_FMAXNMP_scalar; break;
            default: base = NEON_FMINNMP_scalar; break;
        }
        if (bits == 64) base |= 1u << 22;
    }
    return base | (rn << 5) | rd;
}
const std::array<u32, 8> Controls{0, 1u << 22, 2u << 22, 3u << 22,
    1u << 24, 1u << 25, (1u << 24) | (1u << 25), 1u << 19};
void CheckCase(Program& program, u32 bits, u32 operation, u32 rd, u64 lhs, u64 rhs, u32 fpcr) {
    Machine machine;
    auto source = machine.GetVector(31);
    if (bits == 64) source = {lhs, rhs};
    else {
        const u64 mask = (u64{1} << (bits * 2 % 64)) - 1;
        source[0] = (bits == 32 ? 0 : source[0] & ~mask) | lhs | (rhs << bits);
    }
    machine.SetVector(31, source);
    machine.Context().fpcr = fpcr;
    machine.Context().fpsr = 0x08000002;
    const auto before = machine.Context();
    const auto expected = Reference(lhs, rhs, bits, operation, fpcr);
    program.Run(machine);
    REQUIRE(machine.GetVector(rd) == HighVectorBits{expected.value, 0});
    REQUIRE(machine.Context().fpsr == (before.fpsr | expected.flags));
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
std::vector<u64> Edges(u32 bits) {
    const FloatFormat f(bits);
    const u64 one = u64(f.bias) << f.fraction;
    return {0, f.sign, 1, f.sign | 1, f.fraction_mask, u64{1} << f.fraction,
            (u64{1} << f.fraction) + 1, one, one | f.sign, one + 1,
            f.exponent_mask - 1, f.exponent_mask, f.exponent_mask | f.sign,
            f.exponent_mask | 3, f.exponent_mask | f.quiet | 7,
            f.sign | f.exponent_mask | 5, f.sign | f.exponent_mask | f.quiet | 11};
}
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 scalar FP pairwise H S D matches independent exact addition and NaN min max bit model") {
    for (u32 bits : {16u, 32u, 64u}) for (u32 op : {2u, 4u, 5u, 6u, 7u}) for (u32 rd : {0u, 31u, 30u}) {
        Program program(Encode(bits, op, rd, 31));
        for (u32 control : Controls) for (u64 lhs : Edges(bits)) for (u64 rhs : Edges(bits))
            CheckCase(program, bits, op, rd, lhs, rhs, control);
        const FloatFormat f(bits);
        const u64 one = u64(f.bias) << f.fraction, half_ulp = u64(f.bias - f.fraction - 1) << f.fraction;
        for (u32 control : Controls) {
            CheckCase(program, bits, op, rd, one, half_ulp, control);
            CheckCase(program, bits, op, rd, one + 1, half_ulp, control);
        }
    }
}

TEST_CASE("ARM64 scalar FP pairwise random inputs preserve complete guest state and sticky exceptions") {
    u64 random = 0x9E3779B97F4A7C15;
    for (u32 bits : {16u, 32u, 64u}) for (u32 op : {2u, 4u, 5u, 6u, 7u}) for (u32 rd : {0u, 31u, 30u}) {
        Program program(Encode(bits, op, rd, 31));
        const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
        for (u32 control : Controls) for (u32 sample = 0; sample < 48; ++sample) {
            const u64 lhs = Random(random) & mask, rhs = Random(random) & mask;
            CheckCase(program, bits, op, rd, lhs, rhs, control);
        }
    }
}

TEST_CASE("ARM64 scalar FP pairwise unsupported FPCR controls exit before any guest arithmetic update") {
    for (u32 bits : {16u, 32u, 64u}) for (u32 op : {2u, 4u, 5u, 6u, 7u}) {
        Program program(Encode(bits, op, 31, 31));
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
