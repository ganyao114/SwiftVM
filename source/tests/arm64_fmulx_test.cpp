#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "arm64_fmulx_reference.h"
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

using Registers = std::array<u32, 3>;
const std::array<Registers, 5> Aliases{{{0,31,30},{31,31,30},{30,31,30},{0,30,30},{30,30,30}}};
const std::array<u32, 8> Controls{0,1u<<22,2u<<22,3u<<22,1u<<19,1u<<24,1u<<25,(1u<<19)|(1u<<24)|(1u<<25)};
struct Shape { u32 bits; bool q, scalar; };
const std::array<Shape, 8> Shapes{{{16,false,false},{16,true,false},{32,false,false},{32,true,false},
                                {64,true,false},{16,true,true},{32,true,true},{64,true,true}}};
u64 Element(HighVectorBits vector, u32 bits, u32 lane) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    return (vector[lane * bits / 64] >> (lane * bits % 64)) & mask;
}
void PutElement(HighVectorBits& vector, u32 bits, u32 lane, u64 value) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const u32 half = lane * bits / 64, shift = lane * bits % 64;
    vector[half] = (vector[half] & ~(mask << shift)) | ((value & mask) << shift);
}
u32 Encode(Shape shape, Registers regs) {
    return (shape.bits == 16 ? u32(NEON_FMULX_H) : u32(NEON_FMULX)) |
        (shape.q ? NEON_Q : 0) | (shape.scalar ? NEONScalar : 0) |
        (shape.bits == 64 ? 1u << 22 : 0) | (regs[2] << 16) | (regs[1] << 5) | regs[0];
}
u32 Active(Shape shape) { return shape.scalar ? 1 : (shape.q ? 128 : 64) / shape.bits; }
void CheckCase(Program& program, Shape shape, Registers regs, HighVectorBits lhs, HighVectorBits rhs, u32 fpcr) {
    Machine machine;
    machine.SetVector(regs[1], lhs); machine.SetVector(regs[2], rhs);
    machine.Context().fpcr = fpcr; machine.Context().fpsr = 0x08000002;
    const auto before = machine.Context();
    lhs = machine.GetVector(regs[1]); rhs = machine.GetVector(regs[2]);
    HighVectorBits expected{};
    u32 flags = before.fpsr;
    for (u32 lane = 0; lane < Active(shape); ++lane) {
        const auto result = fmulx_reference::Evaluate(Element(lhs,shape.bits,lane),Element(rhs,shape.bits,lane),shape.bits,fpcr);
        PutElement(expected,shape.bits,lane,result.value); flags |= result.exceptions;
    }
    program.Run(machine);
    REQUIRE(machine.GetVector(regs[0]) == expected);
    REQUIRE(machine.Context().fpsr == flags); REQUIRE(machine.Context().fpcr == before.fpcr);
    REQUIRE(machine.Context().r == before.r); REQUIRE(machine.Context().fp == before.fp);
    REQUIRE(machine.Context().lr == before.lr); REQUIRE(machine.Context().sp == before.sp);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000); REQUIRE(machine.state->current_loc.Value() == 0x1004);
    for (u32 reg = 0; reg < 32; ++reg) if (reg != regs[0])
        REQUIRE(std::memcmp(&machine.Context().v[reg], &before.v[reg], sizeof(before.v[reg])) == 0);
}
std::vector<u64> Edges(u32 bits) {
    const fmulx_reference::Format f(bits);
    const u64 one = u64(f.bias) << f.fraction, two = u64(f.bias+1) << f.fraction, minimum = u64{1} << f.fraction;
    return {0,f.sign,1,f.sign|1,minimum-1,f.sign|(minimum-1),minimum,f.sign|minimum,minimum+1,
        one-1,one,one+1,f.sign|one,two,one|(u64{1}<<(f.fraction-1)),f.exponent_mask-1,f.sign|(f.exponent_mask-1),
        f.exponent_mask,f.sign|f.exponent_mask,f.exponent_mask|f.quiet|3,f.sign|f.exponent_mask|f.quiet|5,
        f.exponent_mask|1,f.sign|f.exponent_mask|7};
}
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 scalar and vector FMULX H S D signed zero infinity NaNs and rational rounding match exact integers") {
    for (auto shape : Shapes) {
        const auto edges = Edges(shape.bits);
        Program program(Encode(shape,Aliases[0]));
        for (u32 control : Controls) for (u32 start = 0; start < edges.size()*edges.size(); start += Active(shape)) {
            HighVectorBits lhs{},rhs{};
            for (u32 lane = 0; lane < 128/shape.bits; ++lane) {
                const u32 pair = (start+lane) % (edges.size()*edges.size());
                PutElement(lhs,shape.bits,lane,edges[pair/edges.size()]); PutElement(rhs,shape.bits,lane,edges[pair%edges.size()]);
            }
            CheckCase(program,shape,Aliases[0],lhs,rhs,control);
        }
    }
}

TEST_CASE("ARM64 FMULX aliases source snapshots sticky status and complete state match deterministic exact multiplication") {
    u64 random = 0x629A292A367CD507;
    for (auto shape : Shapes) for (auto regs : Aliases) {
        Program program(Encode(shape,regs));
        for (u32 control : Controls) for (u32 sample = 0; sample < 64; ++sample) {
            const HighVectorBits lhs{Random(random),Random(random)},rhs{Random(random),Random(random)};
            CheckCase(program,shape,regs,lhs,rhs,control);
        }
    }
}

TEST_CASE("ARM64 FMULX inactive vector scalar and high scalar bits cannot raise exceptions and fallback is precise") {
    for (auto shape : Shapes) for (auto regs : Aliases) {
        const fmulx_reference::Format f(shape.bits);
        HighVectorBits lhs{},rhs{};
        for (u32 lane = 0; lane < 128/shape.bits; ++lane) {
            PutElement(lhs,shape.bits,lane,lane < Active(shape) ? u64(f.bias)<<f.fraction : f.exponent_mask|1);
            PutElement(rhs,shape.bits,lane,lane < Active(shape) ? u64(f.bias)<<f.fraction : f.exponent_mask|3);
        }
        Program program(Encode(shape,regs));
        for (u32 control : Controls) CheckCase(program,shape,regs,lhs,rhs,control);
        for (u32 control : {1u,2u,1u<<8,1u<<9,1u<<10,1u<<11,1u<<12,1u<<15,1u<<26,1u<<31}) {
            Machine machine; machine.Context().fpcr = control; machine.Context().fpsr = 0x08000091;
            machine.Context().pc = 0xCAFE0123456789AB;
            const auto before = machine.Context();
            program.Run(machine,swift::arm64::InterruptReason::FALLBACK);
            REQUIRE(std::memcmp(&machine.Context().v,&before.v,sizeof(before.v)) == 0);
            REQUIRE(machine.Context().r == before.r); REQUIRE(machine.Context().fpcr == before.fpcr); REQUIRE(machine.Context().fpsr == before.fpsr);
            REQUIRE(machine.Context().pc == 0x1000); REQUIRE(machine.state->current_loc.Value() == 0x1000);
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
    }
    Program reserved(Encode({64,false,false},Aliases[1]));
    Machine machine; machine.Context().pc = 0xCAFE0123456789AB;
    const auto before = machine.Context();
    reserved.Run(machine,swift::arm64::InterruptReason::ILL_CODE);
    REQUIRE(std::memcmp(&machine.Context().v,&before.v,sizeof(before.v)) == 0);
    REQUIRE(machine.Context().pc == before.pc); REQUIRE(machine.state->current_loc.Value() == 0x1000);
}

TEST_CASE("ARM64 Advanced SIMD scalar FMULX decode remains distinct from scalar FP FMUL zero infinity") {
    for (u32 bits : {16u,32u,64u}) {
        const fmulx_reference::Format f(bits);
        Program extended(Encode({bits,true,true},Aliases[0]));
        const u32 regular_base = bits == 16 ? u32(FMUL_h) : bits == 64 ? u32(FMUL_d) : u32(FMUL_s);
        Program regular(regular_base | (30u<<16) | (31u<<5));
        for (u32 control : Controls) for (u32 signs = 0; signs < 4; ++signs) for (bool reverse : {false,true}) {
            const u64 zero = signs & 1 ? f.sign : 0, infinity = (signs & 2 ? f.sign : 0) | f.exponent_mask;
            const HighVectorBits lhs{reverse ? infinity : zero,0xABCD1032547698EF};
            const HighVectorBits rhs{reverse ? zero : infinity,0xCAFE0123456789AB};
            CheckCase(extended,{bits,true,true},Aliases[0],lhs,rhs,control);
            Machine machine; machine.SetVector(31,lhs); machine.SetVector(30,rhs);
            machine.Context().fpcr = control; machine.Context().fpsr = 0x08000002;
            regular.Run(machine);
            REQUIRE(machine.GetVector(0) == HighVectorBits{f.exponent_mask | f.quiet,0});
            REQUIRE(machine.Context().fpsr == 0x08000003);
        }
    }
}
