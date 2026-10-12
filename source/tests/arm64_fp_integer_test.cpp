#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "arm64_fp_integer_reference.h"
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
struct Shape { u32 bits; bool q,scalar; };
const std::array<Shape,8> Shapes{{{16,false,false},{16,true,false},{32,false,false},{32,true,false},
                                {64,true,false},{16,true,true},{32,true,true},{64,true,true}}};
u64 Element(HighVectorBits vector, u32 bits, u32 lane) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits)-1;
    return (vector[lane*bits/64] >> (lane*bits%64)) & mask;
}
void PutElement(HighVectorBits& vector, u32 bits, u32 lane, u64 value) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits)-1;
    const u32 half = lane*bits/64, shift = lane*bits%64;
    vector[half] = (vector[half] & ~(mask << shift)) | ((value & mask) << shift);
}
u32 Encode(Shape shape, u32 operation, Registers regs) {
    const std::array<u32,12> bases{NEON_SCVTF,NEON_UCVTF,NEON_FCVTNS,NEON_FCVTNU,NEON_FCVTMS,NEON_FCVTMU,
        NEON_FCVTPS,NEON_FCVTPU,NEON_FCVTZS,NEON_FCVTZU,NEON_FCVTAS,NEON_FCVTAU};
    const std::array<u32,12> half{NEON_SCVTF_H,NEON_UCVTF_H,NEON_FCVTNS_H,NEON_FCVTNU_H,NEON_FCVTMS_H,NEON_FCVTMU_H,
        NEON_FCVTPS_H,NEON_FCVTPU_H,NEON_FCVTZS_H,NEON_FCVTZU_H,NEON_FCVTAS_H,NEON_FCVTAU_H};
    return (shape.bits == 16 ? half : bases)[operation] | (shape.q ? NEON_Q : 0) | (shape.scalar ? NEONScalar : 0) |
           (shape.bits == 64 ? 1u<<22 : 0) | (regs[1]<<5) | regs[0];
}
void CheckCase(Program& program, Shape shape, u32 operation, Registers regs, HighVectorBits source, u32 fpcr, u32 sticky = 0x08000002) {
    Machine machine; machine.SetVector(regs[1],source); machine.Context().fpcr = fpcr; machine.Context().fpsr = sticky;
    const auto before = machine.Context();
    HighVectorBits expected{}; u32 flags = before.fpsr;
    for (u32 lane=0; lane < (shape.scalar ? 1 : (shape.q ? 128u : 64u)/shape.bits); ++lane) {
        const auto result = fp_integer_reference::Evaluate(Element(source,shape.bits,lane),shape.bits,operation,fpcr);
        PutElement(expected,shape.bits,lane,result.value); flags |= result.exceptions;
    }
    program.Run(machine);
    REQUIRE(machine.GetVector(regs[0]) == expected); REQUIRE(machine.Context().fpsr == flags);
    REQUIRE(machine.Context().fpcr == before.fpcr); REQUIRE(machine.Context().r == before.r);
    REQUIRE(machine.Context().fp == before.fp); REQUIRE(machine.Context().lr == before.lr); REQUIRE(machine.Context().sp == before.sp);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000); REQUIRE(machine.state->current_loc.Value() == 0x1004);
    for (u32 reg=0; reg<32; ++reg) if (reg != regs[0])
        REQUIRE(std::memcmp(&machine.Context().v[reg],&before.v[reg],sizeof(before.v[reg])) == 0);
}
std::vector<u64> Edges(u32 bits, u32 operation) {
    const u32 fraction = bits == 16 ? 10 : bits == 32 ? 23 : 52, bias = bits == 16 ? 15 : bits == 32 ? 127 : 1023;
    const u64 sign = u64{1}<<(bits-1), mask = bits == 64 ? ~u64{0} : (u64{1}<<bits)-1;
    if (operation < 2) {
        const u64 pivot = u64{1}<<(fraction+1);
        return {0,1,2,mask,sign,sign-1,sign+1,pivot-1,pivot,pivot+1,mask-1,
                (0-pivot-1)&mask,(0-pivot+1)&mask,2049,(0-u64{2049})&mask,65519&mask,65520&mask,65535&mask};
    }
    const u64 infinity = u64(2*bias+1)<<fraction, quiet = u64{1}<<(fraction-1), one = u64(bias)<<fraction, half = u64(bias-1)<<fraction;
    const u64 bound = u64(bias+bits-1)<<fraction, ubound = u64(bias+bits)<<fraction;
    return {0,sign,half,sign|half,one|quiet,sign|one|quiet,one+(u64{1}<<(fraction-2)),sign|(one+(u64{1}<<(fraction-2))),
        (u64(bias+1)<<fraction)|(u64{1}<<(fraction-2)),sign|((u64(bias+1)<<fraction)|(u64{1}<<(fraction-2))),
        1,sign|1,(u64{1}<<fraction)-1,sign|((u64{1}<<fraction)-1),u64{1}<<fraction,sign|(u64{1}<<fraction),
        infinity,sign|infinity,infinity|quiet|3,sign|infinity|quiet|5,infinity|1,sign|infinity|3,
        bound-1,bound,bound+1,sign|bound,sign|(bound+1),ubound-1,ubound,infinity-1,sign|(infinity-1)};
}
u64 Random(u64& state) { state ^= state>>12; state ^= state<<25; state ^= state>>27; return state*0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 nonfixed SIMD integer conversions follow exact rational rounding width clamps and sticky FPSR") {
    for (auto shape : Shapes) for (u32 operation=0; operation<12; ++operation) {
        const auto edges = Edges(shape.bits,operation);
        for (auto regs : Aliases) {
            Program program(Encode(shape,operation,regs));
            for (auto fpcr : Controls) for (u32 profile=0; profile<edges.size(); ++profile) {
                HighVectorBits source{};
                for (u32 lane=0; lane<128/shape.bits; ++lane) PutElement(source,shape.bits,lane,edges[(profile+lane)%edges.size()]);
                CheckCase(program,shape,operation,regs,source,fpcr);
            }
        }
    }
}

TEST_CASE("ARM64 SIMD integer conversion active lanes alias snapshots and per lane invalid inexact accumulation") {
    u64 seed = 0xE028442668ABCDEF;
    for (auto shape : Shapes) for (u32 operation=0; operation<12; ++operation) for (auto regs : Aliases) {
        Program program(Encode(shape,operation,regs));
        for (u32 trial=0; trial<16; ++trial) {
            HighVectorBits source{Random(seed),Random(seed)};
            const auto edges = Edges(shape.bits,operation);
            if (trial < 2) {
                const u32 active = shape.scalar ? 1 : (shape.q ? 128 : 64)/shape.bits;
                for (u32 lane=0; lane<128/shape.bits; ++lane) {
                    const u64 value = operation < 2 ? edges[trial ? 0 : 9] :
                        trial ? edges[lane < active ? 0 : 20] : edges[lane%2 ? 4 : 18];
                    PutElement(source,shape.bits,lane,value);
                }
            }
            CheckCase(program,shape,operation,regs,source,Controls[trial%Controls.size()],trial&1 ? 0x08000092 : 0x08000002);
        }
    }
}

TEST_CASE("ARM64 SIMD integer conversions unsupported FPCR exit before changing source destination or exceptions") {
    const Registers regs{31,31};
    for (auto shape : Shapes) for (u32 operation=0; operation<12; ++operation) {
        Program program(Encode(shape,operation,regs));
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

TEST_CASE("ARM64 nonfixed SIMD integer conversions reject unallocated D Q0 shapes precisely") {
    for (u32 operation=0; operation<12; ++operation) {
        Program program(Encode({64,false,false},operation,{0,31}));
        Machine machine; machine.Context().pc = 0x8877665544332211; const auto before = machine.Context();
        program.Run(machine,swift::arm64::InterruptReason::ILL_CODE);
        auto expected = before; expected.padding = u32(swift::arm64::InterruptReason::ILL_CODE);
        REQUIRE(std::memcmp(&machine.Context(),&expected,sizeof(expected)) == 0);
        REQUIRE(machine.state->current_loc.Value() == 0x1000); REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    }
}
