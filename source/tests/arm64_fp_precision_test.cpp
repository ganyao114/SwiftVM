#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "arm64_fp_precision_reference.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;
using namespace vixl::aarch64;
namespace {
using HighVectorBits = std::array<u64, 2>;
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

using Registers = std::array<u32,2>;
const std::array<Registers,3> Aliases{{{0,31},{31,31},{30,31}}};
const std::array<u32,8> Controls{0,1u<<22,2u<<22,3u<<22,1u<<19,1u<<24,1u<<25,(1u<<19)|(1u<<24)|(1u<<25)};
struct Shape { u32 small; bool upper; u32 operation; };
const std::array<Shape,10> Shapes{{{16,false,0},{16,true,0},{32,false,0},{32,true,0},
                                 {16,false,1},{16,true,1},{32,false,1},{32,true,1},{32,false,2},{32,true,2}}};
u64 Element(HighVectorBits vector, u32 bits, u32 lane) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits)-1;
    return (vector[lane*bits/64] >> (lane*bits%64)) & mask;
}
void PutElement(HighVectorBits& vector, u32 bits, u32 lane, u64 value) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits)-1;
    const u32 half = lane*bits/64, shift = lane*bits%64;
    vector[half] = (vector[half] & ~(mask << shift)) | ((value & mask) << shift);
}
u32 Encode(Shape shape, Registers regs) {
    const std::array<u32,3> bases{NEON_FCVTL,NEON_FCVTN,NEON_FCVTXN};
    return bases[shape.operation] | (shape.upper ? NEON_Q : 0) | (shape.small == 32 ? 1u<<22 : 0) | (regs[1]<<5) | regs[0];
}
void CheckCase(Program& program, Shape shape, Registers regs, HighVectorBits source, u32 fpcr) {
    Machine machine; machine.SetVector(regs[1],source); machine.Context().fpcr = fpcr; machine.Context().fpsr = 0x08000002;
    const auto before = machine.Context();
    const bool widen = shape.operation == 0;
    const u32 source_bits = widen ? shape.small : shape.small*2, destination_bits = widen ? shape.small*2 : shape.small;
    const u32 source_offset = widen && shape.upper ? 64/source_bits : 0, destination_offset = !widen && shape.upper ? 64/destination_bits : 0;
    HighVectorBits expected = !widen && shape.upper ? HighVectorBits{machine.GetVector(regs[0])[0],0} : HighVectorBits{};
    u32 flags = before.fpsr;
    for (u32 lane=0; lane<64/shape.small; ++lane) {
        const auto result = fp_precision_reference::Evaluate(Element(source,source_bits,source_offset+lane),source_bits,destination_bits,shape.operation == 2,fpcr);
        PutElement(expected,destination_bits,destination_offset+lane,result.value); flags |= result.exceptions;
    }
    program.Run(machine);
    REQUIRE(machine.GetVector(regs[0]) == expected); REQUIRE(machine.Context().fpsr == flags);
    REQUIRE(machine.Context().fpcr == before.fpcr); REQUIRE(machine.Context().r == before.r);
    REQUIRE(machine.Context().fp == before.fp); REQUIRE(machine.Context().lr == before.lr); REQUIRE(machine.Context().sp == before.sp);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000); REQUIRE(machine.state->current_loc.Value() == 0x1004);
    for (u32 reg=0; reg<32; ++reg) if (reg != regs[0])
        REQUIRE(std::memcmp(&machine.Context().v[reg],&before.v[reg],sizeof(before.v[reg])) == 0);
}
std::vector<u64> Edges(u32 source, u32 destination) {
    const u32 sf = fp_precision_reference::Fraction(source), sbias = fp_precision_reference::Bias(source);
    const u32 df = fp_precision_reference::Fraction(destination), dbias = fp_precision_reference::Bias(destination);
    const u64 sign = u64{1}<<(source-1), infinity = u64(2*sbias+1)<<sf, quiet = u64{1}<<(sf-1), one = u64(sbias)<<sf;
    std::vector<u64> values{0,sign,1,sign|1,(u64{1}<<sf)-1,sign|((u64{1}<<sf)-1),u64{1}<<sf,sign|(u64{1}<<sf),
        one,sign|one,one-1,one+1,infinity-1,sign|(infinity-1),infinity,sign|infinity,
        infinity|quiet|3,sign|infinity|quiet|5,infinity|1,sign|infinity|3,infinity|quiet|((u64{1}<<sf)-1)};
    if (source > destination) {
        auto power = [&](s32 exponent) { return u64(exponent+sbias)<<sf; };
        const u64 minimum = power(1-s32(dbias)), subnormal = power(1-s32(dbias)-s32(df)), halfsub = power(-s32(dbias)-s32(df));
        const u64 overflow = power(dbias+1), midpoint = one|(u64{1}<<(sf-df-1));
        const std::array<u64,17> narrow{minimum-1,minimum,minimum+1,subnormal,subnormal+1,halfsub-1,halfsub,halfsub+1,
            midpoint-1,midpoint,midpoint+1,sign|midpoint,overflow-1,overflow,overflow+1,sign|(overflow-1),sign|overflow};
        values.insert(values.end(),narrow.begin(),narrow.end());
    }
    return values;
}
u64 Random(u64& state) { state ^= state>>12; state ^= state<<25; state ^= state>>27; return state*0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 SIMD precision conversions match exact dyadic rounding NaN payloads underflow overflow and odd rounding") {
    for (auto shape : Shapes) {
        const u32 source_bits = shape.operation == 0 ? shape.small : shape.small*2;
        const u32 destination_bits = shape.operation == 0 ? shape.small*2 : shape.small;
        const auto edges = Edges(source_bits,destination_bits);
        for (auto regs : Aliases) {
            Program program(Encode(shape,regs));
            for (auto fpcr : Controls) for (u32 profile=0; profile<edges.size(); ++profile) {
                HighVectorBits source{};
                for (u32 lane=0; lane<128/source_bits; ++lane) PutElement(source,source_bits,lane,edges[(profile+lane)%edges.size()]);
                CheckCase(program,shape,regs,source,fpcr);
            }
        }
    }
}

TEST_CASE("ARM64 SIMD precision conversions snapshot alias sources preserve narrow2 lower half and ignore inactive poison") {
    u64 seed = 0xE038442668ABCDEF;
    for (auto shape : Shapes) for (auto regs : Aliases) {
        Program program(Encode(shape,regs));
        for (u32 trial=0; trial<32; ++trial) {
            HighVectorBits source{Random(seed),Random(seed)};
            if (trial==0 && shape.operation==0) {
                const auto edges = Edges(shape.small,shape.small*2);
                for (u32 lane=0; lane<128/shape.small; ++lane)
                    PutElement(source,shape.small,lane,edges[(lane >= 64/shape.small) != shape.upper ? 18 : 8]);
            }
            CheckCase(program,shape,regs,source,Controls[trial%Controls.size()]);
        }
    }
}

TEST_CASE("ARM64 SIMD precision conversions reject unsupported FPCR precisely before modifying state") {
    for (auto shape : Shapes) {
        Program program(Encode(shape,{31,31}));
        for (u32 control : {1u<<0,1u<<1,1u<<2,1u<<8,1u<<9,1u<<10,1u<<11,1u<<12,1u<<15,1u<<26,1u<<31}) {
            Machine machine; machine.Context().fpcr = control; machine.Context().fpsr = 0x08000092;
            machine.Context().pc = 0x8877665544332211; const auto before = machine.Context();
            program.Run(machine,swift::arm64::InterruptReason::FALLBACK);
            auto expected = before; expected.pc = 0x1000; expected.padding = u32(swift::arm64::InterruptReason::FALLBACK);
            REQUIRE(std::memcmp(&machine.Context(),&expected,sizeof(expected)) == 0);
            REQUIRE(machine.state->current_loc.Value() == 0x1000); REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
    }
}

TEST_CASE("ARM64 SIMD FCVTXN rejects unallocated half precision destination") {
    for (bool upper : {false,true}) {
        Program program(Encode({16,upper,2},{0,31}));
        Machine machine; machine.Context().pc = 0x8877665544332211; const auto before = machine.Context();
        program.Run(machine,swift::arm64::InterruptReason::ILL_CODE);
        auto expected = before; expected.padding = u32(swift::arm64::InterruptReason::ILL_CODE);
        REQUIRE(std::memcmp(&machine.Context(),&expected,sizeof(expected)) == 0);
        REQUIRE(machine.state->current_loc.Value() == 0x1000); REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    }
}
