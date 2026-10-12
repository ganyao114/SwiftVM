#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "arm64_indexed_fp_reference.h"
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
struct Shape { u32 bits; bool q, scalar; };
const std::array<Shape, 8> Shapes{{{16,false,false},{16,true,false},{32,false,false},{32,true,false},
                                {64,true,false},{16,true,true},{32,true,true},{64,true,true}}};
const std::array<u32, 8> Controls{0,1u<<22,2u<<22,3u<<22,1u<<19,1u<<24,1u<<25,(1u<<19)|(1u<<24)|(1u<<25)};
std::array<Registers, 5> Aliases(u32 bits) {
    const u32 rn = bits == 16 ? 15 : 31, rm = bits == 16 ? 14 : 30;
    return {{{0,rn,rm},{rn,rn,rm},{rm,rn,rm},{0,rm,rm},{rm,rm,rm}}};
}
u64 Element(HighVectorBits vector, u32 bits, u32 lane) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    return (vector[lane * bits / 64] >> (lane * bits % 64)) & mask;
}
void PutElement(HighVectorBits& vector, u32 bits, u32 lane, u64 value) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const u32 half = lane * bits / 64, shift = lane * bits % 64;
    vector[half] = (vector[half] & ~(mask << shift)) | ((value & mask) << shift);
}
u32 Encode(Shape shape, u32 operation, u32 index, Registers regs) {
    const std::array<u32, 4> bases{NEON_FMUL_H_byelement,NEON_FMULX_H_byelement,NEON_FMLA_H_byelement,NEON_FMLS_H_byelement};
    u32 code = bases[operation] | ((shape.bits == 16 ? 0 : shape.bits == 32 ? 2u : 3u) << 22) |
        (shape.q ? NEON_Q : 0) | (shape.scalar ? NEONScalar : 0) |
        (regs[2] << 16) | (regs[1] << 5) | regs[0];
    if (shape.bits == 16) code |= ((index >> 2) << 11) | (((index >> 1) & 1) << 21) | ((index & 1) << 20);
    else if (shape.bits == 32) code |= ((index >> 1) << 11) | ((index & 1) << 21);
    else code |= index << 11;
    return code;
}
u32 Active(Shape shape) { return shape.scalar ? 1 : (shape.q ? 128 : 64) / shape.bits; }
void CheckCase(Program& program, Shape shape, u32 operation, u32 index, Registers regs,
               HighVectorBits lhs, HighVectorBits indexed, HighVectorBits accumulator, u32 fpcr, u32 sticky = 0x08000002) {
    Machine machine;
    machine.SetVector(regs[0],accumulator); machine.SetVector(regs[1],lhs); machine.SetVector(regs[2],indexed);
    machine.Context().fpcr = fpcr; machine.Context().fpsr = sticky;
    const auto before = machine.Context();
    lhs = machine.GetVector(regs[1]); indexed = machine.GetVector(regs[2]); accumulator = machine.GetVector(regs[0]);
    HighVectorBits expected{};
    u32 flags = before.fpsr;
    const u64 rhs = Element(indexed,shape.bits,index);
    for (u32 lane = 0; lane < Active(shape); ++lane) {
        const auto result = indexed_fp_reference::Evaluate(Element(lhs,shape.bits,lane),rhs,Element(accumulator,shape.bits,lane),
            shape.bits,operation,fpcr);
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
using Triple = std::array<u64, 3>;
std::vector<Triple> Edges(u32 bits, u32 operation) {
    const fmulx_reference::Format f(bits);
    const u64 one = u64(f.bias) << f.fraction, two = u64(f.bias+1) << f.fraction;
    const u64 cancellation = (operation == 2 ? f.sign : 0) | one;
    return {{one+1,one-2,cancellation},{one,one,cancellation},
        {0,f.exponent_mask,0},{f.sign,f.exponent_mask,f.exponent_mask|f.quiet|3},
        {f.exponent_mask,0,f.exponent_mask|1},{1,f.exponent_mask,f.sign|one},
        {f.sign|1,f.exponent_mask,f.exponent_mask|f.quiet|3},
        {f.exponent_mask|f.quiet|1,f.sign|f.exponent_mask|2,f.exponent_mask|f.quiet|3},
        {f.exponent_mask|1,f.exponent_mask|f.quiet|2,f.sign|f.exponent_mask|3},
        {f.sign|f.exponent_mask|f.quiet|1,f.exponent_mask|f.quiet|2,f.exponent_mask|f.quiet|3},
        {one+1,one|(u64{1}<<(f.fraction-1)),f.sign|1},
        {one+3,one|(u64{1}<<(f.fraction-1)),1},{f.exponent_mask-1,two,f.exponent_mask-1},
        {(u64{1}<<f.fraction)+1,one-1,(operation == 2 ? f.sign : 0)|(u64{1}<<f.fraction)}};
}
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 indexed scalar vector H S D FMUL FMULX FMLA FMLS all legal elements match rational boundaries") {
    for (auto shape : Shapes) for (u32 operation = 0; operation < 4; ++operation)
        for (u32 index = 0; index < 128/shape.bits; ++index) for (auto regs : Aliases(shape.bits)) {
            Program program(Encode(shape,operation,index,regs));
            const auto edges = Edges(shape.bits,operation);
            for (u32 control : Controls) for (auto values : edges) {
                HighVectorBits lhs{},rhs{},acc{};
                for (u32 lane = 0; lane < 128/shape.bits; ++lane) {
                    PutElement(lhs,shape.bits,lane,values[0]); PutElement(rhs,shape.bits,lane,values[1]);
                    PutElement(acc,shape.bits,lane,values[2]);
                }
                CheckCase(program,shape,operation,index,regs,lhs,rhs,acc,control);
            }
        }
}

TEST_CASE("ARM64 indexed FP random source accumulator aliases snapshot every selected lane and preserve sticky status") {
    u64 random = 0xB58A503756A655D9;
    for (auto shape : Shapes) for (u32 operation = 0; operation < 4; ++operation)
        for (u32 index = 0; index < 128/shape.bits; ++index) for (auto regs : Aliases(shape.bits)) {
            Program program(Encode(shape,operation,index,regs));
            for (u32 control : Controls) for (u32 sample = 0; sample < 16; ++sample) {
                const HighVectorBits lhs{Random(random),Random(random)},rhs{Random(random),Random(random)},acc{Random(random),Random(random)};
                CheckCase(program,shape,operation,index,regs,lhs,rhs,acc,control,sample & 1 ? 0x0800009F : 0x08000002);
            }
        }
}

TEST_CASE("ARM64 indexed FP evaluates only the chosen element and active lanes and respects split M register encoding") {
    for (auto shape : Shapes) for (u32 operation = 0; operation < 4; ++operation) {
        const fmulx_reference::Format f(shape.bits);
        const u64 one = u64(f.bias) << f.fraction;
        for (u32 index = 0; index < 128/shape.bits; ++index) {
            for (u32 indexed_register : shape.bits == 16 ? std::vector<u32>{0,1,14,15} : std::vector<u32>{0,1,15,16,30,31}) {
                const Registers regs{2,3,indexed_register};
                Program program(Encode(shape,operation,index,regs));
                HighVectorBits lhs{},rhs{},acc{};
                for (u32 lane = 0; lane < 128/shape.bits; ++lane) {
                    PutElement(lhs,shape.bits,lane,lane < Active(shape) ? one : f.exponent_mask|1);
                    PutElement(acc,shape.bits,lane,lane < Active(shape) ? one : f.exponent_mask|3);
                    PutElement(rhs,shape.bits,lane,lane == index ? one : f.exponent_mask|5);
                }
                for (u32 control : Controls) CheckCase(program,shape,operation,index,regs,lhs,rhs,acc,control);
                for (u32 lane = 0; lane < 128/shape.bits; ++lane) {
                    if (lane >= Active(shape)) { PutElement(lhs,shape.bits,lane,1); PutElement(acc,shape.bits,lane,1); }
                    if (lane != index) PutElement(rhs,shape.bits,lane,1);
                }
                CheckCase(program,shape,operation,index,regs,lhs,rhs,acc,(1u<<19)|(1u<<24));
            }
        }
    }
}

TEST_CASE("ARM64 indexed FP unsupported controls reserved size D Q0 and D L1 leave arithmetic state untouched") {
    for (auto shape : Shapes) for (u32 operation = 0; operation < 4; ++operation) {
        Program program(Encode(shape,operation,128/shape.bits-1,Aliases(shape.bits)[1]));
        for (u32 control : {1u,2u,1u<<8,1u<<9,1u<<10,1u<<11,1u<<12,1u<<15,1u<<26,1u<<31}) {
            Machine machine; machine.Context().fpcr = control; machine.Context().fpsr = 0x08000091;
            machine.Context().pc = 0xCAFE0123456789AB;
            const auto before = machine.Context();
            program.Run(machine,swift::arm64::InterruptReason::FALLBACK);
            REQUIRE(std::memcmp(&machine.Context().v,&before.v,sizeof(before.v)) == 0);
            REQUIRE(machine.Context().r == before.r); REQUIRE(machine.Context().fpcr == before.fpcr); REQUIRE(machine.Context().fpsr == before.fpsr);
            REQUIRE(machine.Context().pc == 0x1000); REQUIRE(machine.state->current_loc.Value() == 0x1000);
        }
    }
    for (u32 operation = 0; operation < 4; ++operation) for (u32 reserved : {
        Encode({64,false,false},operation,1,{0,31,30}),
        Encode({64,true,false},operation,1,{0,31,30}) | (1u<<21),
        Encode({64,true,true},operation,1,{0,31,30}) | (1u<<21),
        Encode({16,true,false},operation,0,{0,15,14}) | (1u<<22),
        Encode({16,true,true},operation,0,{0,15,14}) | (1u<<22)}) {
        Program program(reserved);
        Machine machine; machine.Context().pc = 0xCAFE0123456789AB;
        const auto before = machine.Context();
        program.Run(machine,swift::arm64::InterruptReason::ILL_CODE);
        REQUIRE(std::memcmp(&machine.Context().v,&before.v,sizeof(before.v)) == 0);
        REQUIRE(machine.Context().fpsr == before.fpsr); REQUIRE(machine.Context().fpcr == before.fpcr);
        REQUIRE(machine.Context().pc == before.pc); REQUIRE(machine.state->current_loc.Value() == 0x1000);
    }
}

