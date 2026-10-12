#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "arm64_fp_reduction_reference.h"
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

using Registers = std::array<u32,3>;
const std::array<Registers,5> Aliases{{{0,31,30},{31,31,30},{30,31,30},{31,31,31},{3,31,30}}};
const std::array<u32,8> Controls{0,1u<<22,2u<<22,3u<<22,1u<<19,1u<<24,1u<<25,(1u<<19)|(1u<<24)|(1u<<25)};
struct Shape { u32 bits; bool q; };
const std::array<Shape,5> Shapes{{{16,false},{16,true},{32,false},{32,true},{64,true}}};
const std::array<u32,5> Operations{2,4,5,6,7};
u64 Element(HighVectorBits vector,u32 bits,u32 lane) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1}<<bits)-1;
    return (vector[lane*bits/64]>>(lane*bits%64))&mask;
}
void PutElement(HighVectorBits& vector,u32 bits,u32 lane,u64 value) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1}<<bits)-1;
    const u32 half = lane*bits/64,shift = lane*bits%64;
    vector[half] = (vector[half]&~(mask<<shift))|((value&mask)<<shift);
}
u32 Encode(Shape shape,bool across,u32 operation,Registers regs) {
    const std::array<u32,5> bases{NEON_FADDP,NEON_FMAXP,NEON_FMINP,NEON_FMAXNMP,NEON_FMINNMP};
    const std::array<u32,5> half{NEON_FADDP_H,NEON_FMAXP_H,NEON_FMINP_H,NEON_FMAXNMP_H,NEON_FMINNMP_H};
    const std::array<u32,4> reductions{NEON_FMAXV,NEON_FMINV,NEON_FMAXNMV,NEON_FMINNMV};
    const std::array<u32,4> hreductions{NEON_FMAXV_H,NEON_FMINV_H,NEON_FMAXNMV_H,NEON_FMINNMV_H};
    const u32 index = across ? operation-4 : operation == 2 ? 0 : operation-3;
    const u32 base = across ? (shape.bits == 16 ? hreductions : reductions)[index] : (shape.bits == 16 ? half : bases)[index];
    return base|(shape.q ? NEON_Q : 0)|(shape.bits == 64 ? 1u<<22 : 0)|(regs[1]<<5)|regs[0]|(across ? 0 : regs[2]<<16);
}
fp_reduction_reference::Ref Tree(const std::vector<u64>& values,u32 begin,u32 count,u32 bits,u32 operation,u32 fpcr) {
    if (count==1) return {values[begin],0};
    const auto a = Tree(values,begin,count/2,bits,operation,fpcr);
    const auto b = Tree(values,begin+count/2,count/2,bits,operation,fpcr);
    auto result = fp_reduction_reference::Reference(a.value,b.value,bits,operation,fpcr);
    result.flags |= a.flags|b.flags;return result;
}
void CheckCase(Program& program,Shape shape,bool across,u32 operation,Registers regs,HighVectorBits a,HighVectorBits b,u32 fpcr) {
    Machine machine;machine.SetVector(regs[1],a);if (!across && regs[1]!=regs[2]) machine.SetVector(regs[2],b);
    machine.Context().fpcr = fpcr;machine.Context().fpsr = 0x08000010;
    const auto before = machine.Context();a = machine.GetVector(regs[1]);b = machine.GetVector(regs[2]);
    HighVectorBits expected{};u32 flags = before.fpsr;
    const u32 active = (shape.q ? 128 : 64)/shape.bits;
    if (across) {
        std::vector<u64> values;for (u32 lane=0;lane<active;++lane) values.push_back(Element(a,shape.bits,lane));
        const auto result = Tree(values,0,active,shape.bits,operation,fpcr);expected[0] = result.value;flags |= result.flags;
    } else for (u32 lane=0;lane<active;++lane) {
        const auto source = lane<active/2 ? a : b;const u32 index = lane%(active/2)*2;
        const auto result = fp_reduction_reference::Reference(Element(source,shape.bits,index),Element(source,shape.bits,index+1),shape.bits,operation,fpcr);
        PutElement(expected,shape.bits,lane,result.value);flags |= result.flags;
    }
    program.Run(machine);
    REQUIRE(machine.GetVector(regs[0]) == expected);REQUIRE(machine.Context().fpsr == flags);
    REQUIRE(machine.Context().fpcr == before.fpcr);REQUIRE(machine.Context().r == before.r);
    REQUIRE(machine.Context().fp == before.fp);REQUIRE(machine.Context().lr == before.lr);REQUIRE(machine.Context().sp == before.sp);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);REQUIRE(machine.state->current_loc.Value() == 0x1004);
    for (u32 reg=0;reg<32;++reg) if (reg!=regs[0])
        REQUIRE(std::memcmp(&machine.Context().v[reg],&before.v[reg],sizeof(before.v[reg])) == 0);
}
std::vector<u64> Edges(u32 bits) {
    const u32 fraction = bits == 16 ? 10 : bits == 32 ? 23 : 52,bias = bits == 16 ? 15 : bits == 32 ? 127 : 1023;
    const u64 sign = u64{1}<<(bits-1),infinity = u64(2*bias+1)<<fraction,quiet = u64{1}<<(fraction-1),one = u64(bias)<<fraction;
    return {0,sign,one,sign|one,one-1,one+1,1,sign|1,(u64{1}<<fraction)-1,sign|((u64{1}<<fraction)-1),u64{1}<<fraction,sign|(u64{1}<<fraction),
        infinity,sign|infinity,infinity-1,sign|(infinity-1),infinity|quiet|3,sign|infinity|quiet|5,infinity|1,sign|infinity|3,
        u64(bias-fraction-1)<<fraction,sign|(u64(bias-fraction-1)<<fraction),one|quiet,sign|one|quiet};
}
u64 Random(u64& state) { state ^= state>>12;state ^= state<<25;state ^= state>>27;return state*0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 SIMD FP pairwise and across reductions match exact addition min max rounding and sticky exceptions") {
    for (auto shape : Shapes) for (bool across : {false,true}) for (auto operation : Operations) {
        if (across && (operation==2 || shape.bits==64 || (shape.bits==32 && !shape.q))) continue;
        const auto edges = Edges(shape.bits);
        for (auto regs : Aliases) {
            Program program(Encode(shape,across,operation,regs));
            for (auto fpcr : Controls) for (u32 profile=0;profile<edges.size();++profile) {
                HighVectorBits a{},b{};
                for (u32 lane=0;lane<128/shape.bits;++lane) {
                    PutElement(a,shape.bits,lane,edges[(profile+lane)%edges.size()]);
                    PutElement(b,shape.bits,lane,edges[(profile*3+lane+1)%edges.size()]);
                }
                CheckCase(program,shape,across,operation,regs,a,b,fpcr);
            }
        }
    }
}

TEST_CASE("ARM64 FP across reduction tree preserves intermediate NaN priority and signaling exceptions") {
    for (auto shape : Shapes) {
        if (shape.bits==64 || (shape.bits==32 && !shape.q)) continue;
        const u32 active = (shape.q ? 128 : 64)/shape.bits;
        const auto edges = Edges(shape.bits);
        for (u32 operation : {4u,5u,6u,7u}) {
            const Registers regs{31,31,30};Program program(Encode(shape,true,operation,regs));
            for (u32 first=0;first<active;++first) for (u32 second=0;second<active;++second) for (u32 profile=0;profile<4;++profile) {
                HighVectorBits a{};
                for (u32 lane=0;lane<active;++lane) PutElement(a,shape.bits,lane,edges[2]+lane);
                PutElement(a,shape.bits,first,profile&1 ? edges[16] : edges[18]);
                PutElement(a,shape.bits,second,profile&2 ? edges[16]+2 : edges[18]+2);
                CheckCase(program,shape,true,operation,regs,a,{},0);
            }
        }
    }
}

TEST_CASE("ARM64 FP reductions snapshot aliases ignore inactive poison and preserve all other state") {
    u64 seed = 0xE058442668ABCDEF;
    for (auto shape : Shapes) for (bool across : {false,true}) for (auto operation : Operations) {
        if (across && (operation==2 || shape.bits==64 || (shape.bits==32 && !shape.q))) continue;
        for (auto regs : Aliases) {
            Program program(Encode(shape,across,operation,regs));
            for (u32 trial=0;trial<32;++trial) {
                HighVectorBits a{Random(seed),Random(seed)},b{Random(seed),Random(seed)};
                if (trial==0) {
                    const auto edges = Edges(shape.bits);const u32 active = (shape.q ? 128 : 64)/shape.bits;
                    for (u32 lane=0;lane<128/shape.bits;++lane) {
                        PutElement(a,shape.bits,lane,edges[lane<active ? 2 : 18]);
                        PutElement(b,shape.bits,lane,edges[lane<active ? 2 : 6]);
                    }
                }
                CheckCase(program,shape,across,operation,regs,a,b,Controls[trial%Controls.size()]);
            }
        }
    }
}

TEST_CASE("ARM64 FP reductions unsupported controls and unallocated shapes exit precisely") {
    for (auto shape : Shapes) for (bool across : {false,true}) for (auto operation : Operations) {
        if (across && (operation==2 || shape.bits==64 || (shape.bits==32 && !shape.q))) continue;
        Program program(Encode(shape,across,operation,{31,31,30}));
        for (u32 control : {1u<<0,1u<<1,1u<<2,1u<<8,1u<<9,1u<<10,1u<<11,1u<<12,1u<<15,1u<<26,1u<<31}) {
            Machine machine;machine.Context().fpcr = control;machine.Context().fpsr = 0x08000092;
            machine.Context().pc = 0x8877665544332211;const auto before = machine.Context();
            program.Run(machine,swift::arm64::InterruptReason::FALLBACK);
            auto expected = before;expected.pc = 0x1000;expected.padding = u32(swift::arm64::InterruptReason::FALLBACK);
            REQUIRE(std::memcmp(&machine.Context(),&expected,sizeof(expected)) == 0);
            REQUIRE(machine.state->current_loc.Value() == 0x1000);REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
    }
    for (bool across : {false,true}) for (auto operation : Operations) {
        if (across && operation==2) continue;
        for (auto shape : {Shape{64,false},Shape{32,false},Shape{64,true}}) {
            if (!across && (shape.bits!=64 || shape.q)) continue;
            Program program(Encode(shape,across,operation,{0,31,30}));Machine machine;
            machine.Context().pc = 0x8877665544332211;const auto before = machine.Context();
            program.Run(machine,swift::arm64::InterruptReason::ILL_CODE);
            auto expected = before;expected.padding = u32(swift::arm64::InterruptReason::ILL_CODE);
            REQUIRE(std::memcmp(&machine.Context(),&expected,sizeof(expected)) == 0);
            REQUIRE(machine.state->current_loc.Value() == 0x1000);REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
    }
}
