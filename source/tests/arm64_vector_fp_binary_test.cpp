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
struct Ref { u64 value; u32 flags; };
Big Magnitude(u64 value, u32 fraction_bits, u32 maximum_exponent) {
    const u32 exponent = (value >> fraction_bits) & maximum_exponent;
    const u64 fraction = value & ((u64{1} << fraction_bits) - 1);
    if (!exponent) return Big{fraction};
    return (Big{fraction} + (Big{1} << fraction_bits)) << (exponent - 1);
}
// Finite inputs use exact integer rational arithmetic. Quantization compares
// the exact remainder to a half-ULP and checks tininess before rounding.
Ref Reference(u64 a, u64 b, u32 bits, u32 op, u32 fpcr) {
    const u32 fraction_bits = bits == 32 ? 23 : 52, bias = bits == 32 ? 127 : 1023;
    const u32 maximum_exponent = 2 * bias + 1, scale = fraction_bits + bias - 1;
    const u64 fraction_mask = (u64{1} << fraction_bits) - 1, quiet = u64{1} << (fraction_bits - 1);
    const u64 exponent_mask = u64(maximum_exponent) << fraction_bits;
    const u64 sign_mask = u64{1} << (bits - 1), magnitude_mask = sign_mask - 1;
    u32 input_flags = 0;
    const bool absolute = op == 8;
    if (absolute) op = 1;
    auto finish = [&](u64 value, u32 flags) { return Ref{u64(absolute ? value & magnitude_mask : value), flags | input_flags}; };
    auto nan = [&](u64 value) { return (value & exponent_mask) == exponent_mask && (value & fraction_mask); };
    auto signaling = [&](u64 value) { return nan(value) && !(value & quiet); };
    if (fpcr & (1u << 24)) {
        if (!(a & exponent_mask) && (a & fraction_mask)) { a &= sign_mask; input_flags |= 128; }
        if (!(b & exponent_mask) && (b & fraction_mask)) { b &= sign_mask; input_flags |= 128; }
    }
    const bool numeric = op >= 6, minimum = op & 1, snan = signaling(a) || signaling(b);
    if (snan || ((nan(a) || nan(b)) && (op < 4 || !numeric || (nan(a) && nan(b))))) {
        const u64 chosen = signaling(a) ? a : signaling(b) ? b : nan(a) ? a : b;
        return finish(fpcr & (1u << 25) ? (exponent_mask | quiet) : chosen | quiet, snan ? 1 : 0);
    }
    if (nan(a)) return finish(b, 0);
    if (nan(b)) return finish(a, 0);
    if (op >= 4) {
        if (!(a & magnitude_mask) && !(b & magnitude_mask)) return finish(minimum ? a | b : a & b, 0);
        const bool less = bool(a & sign_mask) != bool(b & sign_mask) ? bool(a & sign_mask)
                         : (a & sign_mask) ? a > b : a < b;
        return finish(minimum == less ? a : b, 0);
    }
    const bool ainf = (a & magnitude_mask) == exponent_mask, binf = (b & magnitude_mask) == exponent_mask;
    const bool azero = !(a & magnitude_mask), bzero = !(b & magnitude_mask);
    const bool asign = a & sign_mask;
    bool bsign = b & sign_mask, negative;
    const u32 mode = (fpcr >> 22) & 3;
    Big numerator, denominator;
    if (op < 2) {
        if (op == 1) bsign = !bsign;
        if (ainf || binf) {
            if (ainf && binf && asign != bsign) return finish((exponent_mask | quiet), 1);
            return finish(((ainf ? asign : bsign) ? sign_mask : 0) | exponent_mask, 0);
        }
        Big left = Magnitude(a, fraction_bits, maximum_exponent), right = Magnitude(b, fraction_bits, maximum_exponent);
        if (asign) left = -left;
        if (bsign) right = -right;
        numerator = left + right;
        denominator = Big{1} << scale;
        if (numerator == 0)
            return finish((asign == bsign ? asign : mode == 2) ? sign_mask : 0, 0);
        negative = numerator < 0;
        if (negative) numerator = -numerator;
    } else {
        negative = asign != bsign;
        if (op == 2) {
            if ((ainf && bzero) || (binf && azero)) return finish((exponent_mask | quiet), 1);
            if (ainf || binf) return finish((negative ? sign_mask : 0) | exponent_mask, 0);
            numerator = Magnitude(a, fraction_bits, maximum_exponent) * Magnitude(b, fraction_bits, maximum_exponent);
            denominator = Big{1} << (2 * scale);
        } else {
            if ((azero && bzero) || (ainf && binf)) return finish((exponent_mask | quiet), 1);
            if (ainf || bzero)
                return finish((negative ? sign_mask : 0) | exponent_mask, bzero && !ainf ? 2 : 0);
            if (binf) return finish(negative ? sign_mask : 0, 0);
            numerator = Magnitude(a, fraction_bits, maximum_exponent);
            denominator = Magnitude(b, fraction_bits, maximum_exponent);
        }
    }
    const u64 sign = negative ? sign_mask : 0;
    if (numerator == 0) return finish(sign, 0);
    numerator <<= scale;
    const bool tiny = numerator < denominator * (Big{1} << fraction_bits);
    if (tiny && (fpcr & (1u << 24))) return finish(sign, 8);
    s32 top = s32(boost::multiprecision::msb(numerator)) - s32(boost::multiprecision::msb(denominator));
    if (top >= 0 ? numerator < (denominator << top) : (numerator << -top) < denominator) --top;
    u32 shift = top > s32(fraction_bits) ? u32(top - fraction_bits) : 0;
    const Big divisor = denominator << shift;
    Big rounded = numerator / divisor, remainder = numerator % divisor;
    u32 flags = 0;
    if (remainder != 0) {
        flags |= 16 | (tiny ? 8 : 0);
        bool increment = (mode == 1 && !negative) || (mode == 2 && negative);
        if (mode == 0) increment = 2 * remainder > divisor || (2 * remainder == divisor && bool(rounded & 1));
        if (increment) ++rounded;
    }
    if (rounded >= (Big{1} << (fraction_bits + 1))) { rounded >>= 1; ++shift; }
    const u32 exponent = rounded < (Big{1} << fraction_bits) ? 0 : shift + 1;
    if (exponent >= maximum_exponent) {
        const bool infinity = mode == 0 || (mode == 1 && !negative) || (mode == 2 && negative);
        return finish(sign | (infinity ? exponent_mask : (exponent_mask - 1)), flags | 20);
    }
    return finish(sign | (u64(exponent) << fraction_bits) | (rounded.convert_to<u64>() & fraction_mask), flags);
}
using Registers = std::array<u32, 3>;
const std::array<Registers, 5> Aliases{{{0,31,30}, {31,31,30}, {30,31,30}, {0,30,30}, {30,30,30}}};
const std::array<u32, 8> Controls{0, 1u << 22, 2u << 22, 3u << 22,
    1u << 19, 1u << 25, (1u << 24) | (1u << 25), 1u << 24};
u32 Encode(u32 op, u32 bits, bool q, Registers regs) {
    const std::array<u32, 9> bases{NEON_FADD, NEON_FSUB, NEON_FMUL, NEON_FDIV,
        NEON_FMAX, NEON_FMIN, NEON_FMAXNM, NEON_FMINNM, NEON_FABD};
    return bases[op] | (q ? NEON_Q : 0) | (bits == 64 ? 1u << 22 : 0) |
           (regs[2] << 16) | (regs[1] << 5) | regs[0];
}
u64 Element(HighVectorBits value, u32 bits, u32 lane) {
    return bits == 64 ? value[lane] : (value[lane / 2] >> ((lane % 2) * 32)) & 0xFFFFFFFF;
}
void PutElement(HighVectorBits& value, u32 bits, u32 lane, u64 element) {
    if (bits == 64) value[lane] = element;
    else {
        const u32 shift = (lane % 2) * 32;
        value[lane / 2] = (value[lane / 2] & ~(u64{0xFFFFFFFF} << shift)) | (element << shift);
    }
}
void CheckCase(Program& program, u32 op, u32 bits, bool q, Registers regs,
               HighVectorBits lhs, HighVectorBits rhs, u32 fpcr) {
    Machine machine;
    machine.SetVector(regs[1], lhs); machine.SetVector(regs[2], rhs);
    machine.Context().fpcr = fpcr;
    machine.Context().fpsr = 0x08000002;
    const auto before = machine.Context();
    lhs = machine.GetVector(regs[1]); rhs = machine.GetVector(regs[2]);
    HighVectorBits expected{};
    u32 flags = before.fpsr;
    for (u32 lane = 0; lane < (q ? 128u : 64u) / bits; ++lane) {
        const auto result = Reference(Element(lhs,bits,lane), Element(rhs,bits,lane), bits, op, fpcr);
        PutElement(expected, bits, lane, result.value); flags |= result.flags;
    }
    program.Run(machine);
    REQUIRE(machine.GetVector(regs[0]) == expected);
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
std::vector<u64> Edges(u32 bits) {
    const u32 fraction = bits == 32 ? 23 : 52, bias = bits == 32 ? 127 : 1023;
    const u64 sign = u64{1} << (bits-1), infinity = u64(2*bias+1) << fraction;
    const u64 quiet = u64{1} << (fraction-1), one = u64(bias) << fraction, minnormal = u64{1} << fraction;
    return {0,sign,1,sign|1,minnormal-1,sign|(minnormal-1),minnormal,sign|minnormal,minnormal+1,
        one,sign|one,one+1,u64(bias+1)<<fraction,u64(bias-fraction-1)<<fraction,
        infinity-1,sign|(infinity-1),infinity,sign|infinity,infinity|quiet|3,sign|infinity|quiet|5,
        infinity|1,sign|infinity|3};
}
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 S D vector FP binary matches independent rational rounding and NaN bit model at edges") {
    for (u32 op=0; op<9; ++op) for (u32 shape=0; shape<3; ++shape) {
        const u32 bits = shape == 2 ? 64 : 32; const bool q = shape != 0;
        const auto edges = Edges(bits);
        Program program(Encode(op,bits,q,Aliases[0]));
        const u32 active = (q ? 128 : 64) / bits;
        for (u32 control : Controls) for (u32 start=0; start<edges.size()*edges.size(); start+=active) {
            HighVectorBits lhs{},rhs{};
            for (u32 lane=0; lane<128/bits; ++lane) {
                const u32 pair=(start+lane)%(edges.size()*edges.size());
                PutElement(lhs,bits,lane,edges[pair/edges.size()]); PutElement(rhs,bits,lane,edges[pair%edges.size()]);
            }
            CheckCase(program,op,bits,q,Aliases[0],lhs,rhs,control);
        }
    }
}

TEST_CASE("ARM64 S D vector FP binary preserves aliased snapshots all state and sticky exceptions") {
    u64 random=0x1F83D9ABFB41BD6B;
    for (u32 op=0; op<9; ++op) for (u32 shape=0; shape<3; ++shape) for (auto regs:Aliases) {
        const u32 bits=shape==2 ? 64 : 32; const bool q=shape!=0;
        Program program(Encode(op,bits,q,regs));
        for (u32 control:Controls) for (u32 sample=0; sample<48; ++sample) {
            const HighVectorBits lhs{Random(random),Random(random)},rhs{Random(random),Random(random)};
            CheckCase(program,op,bits,q,regs,lhs,rhs,control);
        }
    }
}

TEST_CASE("ARM64 S vector Q0 FP binary ignores exception poisons in inactive lanes") {
    const HighVectorBits lhs{0,0xFF8000017F800003},rhs{0x3F8000003F800000,0x7F800003FF800001};
    for (u32 op=0; op<9; ++op) for (auto regs:Aliases) {
        Program program(Encode(op,32,false,regs));
        for (u32 control:Controls) CheckCase(program,op,32,false,regs,lhs,rhs,control);
    }
}

TEST_CASE("ARM64 vector FP binary unsupported FPCR and reserved D Q0 exit without arithmetic updates") {
    for (u32 op=0; op<9; ++op) for (u32 shape=0; shape<4; ++shape) {
        const u32 bits=shape>=2 ? 64 : 32; const bool q=shape==1 || shape==2;
        Program program(Encode(op,bits,q,Aliases[1]));
        for (u32 control : {0u,1u,2u,1u<<8,1u<<9,1u<<10,1u<<11,1u<<12,1u<<15,1u<<26,1u<<31}) {
            if (shape!=3 && !control) continue;
            Machine machine; machine.Context().fpcr=control; machine.Context().fpsr=0x08000091;
            machine.Context().pc=0xCAFE0123456789AB;
            const auto before=machine.Context();
            program.Run(machine,shape==3 ? swift::arm64::InterruptReason::ILL_CODE : swift::arm64::InterruptReason::FALLBACK);
            REQUIRE(std::memcmp(&machine.Context().v,&before.v,sizeof(before.v))==0);
            REQUIRE(machine.Context().r==before.r);
            REQUIRE(machine.Context().fpsr==before.fpsr);
            REQUIRE(machine.Context().fpcr==before.fpcr);
            // Decode-time Interrupt publishes the fault PC through State's
            // current_loc; runtime FP helpers additionally synchronize ctx.pc.
            REQUIRE(machine.Context().pc==(shape==3 ? before.pc : 0x1000));
            REQUIRE(machine.state->current_loc.Value()==0x1000);
            REQUIRE(machine.state->host_cpu_flags==0xB0000000);
        }
    }
}
