#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "arm64_vector_fma_reference.h"
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
using Registers=std::array<u32,3>;
const std::array<Registers,5> Aliases{{{0,31,30},{31,31,30},{30,31,30},{0,30,30},{30,30,30}}};
const std::array<u32,8> Controls{0,1u<<22,2u<<22,3u<<22,1u<<19,1u<<24,1u<<25,(1u<<19)|(1u<<24)|(1u<<25)};
u64 Element(HighVectorBits vector,u32 bits,u32 lane) {
    const u64 mask=bits==64 ? ~u64{0} : (u64{1}<<bits)-1;
    return (vector[lane*bits/64]>>(lane*bits%64))&mask;
}
void PutElement(HighVectorBits& vector,u32 bits,u32 lane,u64 value) {
    const u64 mask=bits==64 ? ~u64{0} : (u64{1}<<bits)-1;
    const u32 half=lane*bits/64,shift=lane*bits%64;
    vector[half]=(vector[half]&~(mask<<shift))|((value&mask)<<shift);
}
u32 Encode(u32 bits,bool q,bool subtract,Registers regs) {
    const u32 base=bits==16 ? (subtract ? u32(NEON_FMLS_H) : u32(NEON_FMLA_H)) :
                              (subtract ? u32(NEON_FMLS) : u32(NEON_FMLA));
    return base|(q ? NEON_Q : 0)|(bits==64 ? 1u<<22 : 0)|(regs[2]<<16)|(regs[1]<<5)|regs[0];
}
void CheckCase(Program& program,u32 bits,bool q,bool subtract,Registers regs,
               HighVectorBits lhs,HighVectorBits rhs,HighVectorBits accumulator,u32 fpcr) {
    Machine machine;
    machine.SetVector(regs[0],accumulator);machine.SetVector(regs[1],lhs);machine.SetVector(regs[2],rhs);
    machine.Context().fpcr=fpcr;machine.Context().fpsr=0x08000002;
    const auto before=machine.Context();
    lhs=machine.GetVector(regs[1]);rhs=machine.GetVector(regs[2]);accumulator=machine.GetVector(regs[0]);
    HighVectorBits expected{};
    u32 flags=before.fpsr;
    for (u32 lane=0;lane<(q ? 128u : 64u)/bits;++lane) {
        const auto result=vector_fma_reference::Evaluate(Element(lhs,bits,lane),Element(rhs,bits,lane),
            Element(accumulator,bits,lane),bits,subtract ? 1 : 0,fpcr);
        PutElement(expected,bits,lane,result.value);flags|=result.exceptions;
    }
    program.Run(machine);
    REQUIRE(machine.GetVector(regs[0])==expected);
    REQUIRE(machine.Context().fpsr==flags);
    REQUIRE(machine.Context().fpcr==before.fpcr);
    REQUIRE(machine.Context().r==before.r);
    REQUIRE(machine.Context().fp==before.fp);REQUIRE(machine.Context().lr==before.lr);REQUIRE(machine.Context().sp==before.sp);
    REQUIRE(machine.state->host_cpu_flags==0xB0000000);REQUIRE(machine.state->current_loc.Value()==0x1004);
    for (u32 reg=0;reg<32;++reg) if (reg!=regs[0])
        REQUIRE(std::memcmp(&machine.Context().v[reg],&before.v[reg],sizeof(before.v[reg]))==0);
}
using Triple=std::array<u64,3>;
std::vector<Triple> Edges(u32 bits,bool subtract) {
    const vector_fma_reference::Format f(bits);
    const u64 one=u64(f.bias)<<f.fraction,two=u64(f.bias+1)<<f.fraction,half_ulp=u64(f.bias-f.fraction-1)<<f.fraction;
    const u64 one_and_half=one|(u64{1}<<(f.fraction-1));
    const u64 cancellation=(subtract ? 0 : f.sign)|one,minimum=u64{1}<<f.fraction;
    return {{one+1,one-2,cancellation},{one,one,cancellation},{one,half_ulp,one},{one,half_ulp,one+1},
        // Tiny corrections above/below an exact product midpoint must survive
        // binary128 rounding even when the addend is the minimum subnormal.
        {one+1,one_and_half,f.sign|1},{one+3,one_and_half,1},
        {f.exponent_mask|f.quiet|1,f.exponent_mask|2,f.exponent_mask|f.quiet|3},
        {f.exponent_mask|1,f.exponent_mask|f.quiet|2,f.sign|f.exponent_mask|3},
        {f.exponent_mask|f.quiet|1,one,f.sign|f.exponent_mask|f.quiet|3},
        {f.exponent_mask,0,f.exponent_mask|f.quiet|3},{0,f.exponent_mask,f.exponent_mask|1},
        {f.exponent_mask,one,f.sign|f.exponent_mask},{f.sign|f.exponent_mask,one,f.exponent_mask},
        {0,one,0},{f.sign,one,0},{0,one,f.sign},{f.sign,one,f.sign},
        {minimum+1,one-1,(subtract ? 0 : f.sign)|minimum},{1,two,(subtract ? 0 : f.sign)|1},
        {f.exponent_mask-1,two,f.exponent_mask-1},{f.sign|(f.exponent_mask-1),two,f.sign|(f.exponent_mask-1)}};
}
u64 Random(u64& state) { state^=state>>12;state^=state<<25;state^=state>>27;return state*0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 H S D vector FMLA FMLS match exact fused rational arithmetic at rounding and NaN boundaries") {
    for (u32 bits:{16u,32u,64u}) for (bool q:{false,true}) for (bool subtract:{false,true}) {
        if (bits==64 && !q) continue;
        const auto edges=Edges(bits,subtract);
        for (auto regs:Aliases) {
            Program program(Encode(bits,q,subtract,regs));
            for (u32 control:Controls) for (u32 start=0;start<edges.size();++start) {
                HighVectorBits lhs{},rhs{},acc{};
                for (u32 lane=0;lane<128/bits;++lane) {
                    const auto values=edges[(start+lane)%edges.size()];
                    PutElement(lhs,bits,lane,values[0]);PutElement(rhs,bits,lane,values[1]);PutElement(acc,bits,lane,values[2]);
                }
                CheckCase(program,bits,q,subtract,regs,lhs,rhs,acc,control);
            }
        }
    }
}

TEST_CASE("ARM64 vector FMLA FMLS deterministic random aliases and full guest status match fused integers") {
    u64 random=0xCBBB9D5DC1059ED8;
    for (u32 bits:{16u,32u,64u}) for (bool q:{false,true}) for (bool subtract:{false,true}) {
        if (bits==64 && !q) continue;
        for (auto regs:Aliases) {
            Program program(Encode(bits,q,subtract,regs));
            for (u32 control:Controls) for (u32 sample=0;sample<64;++sample) {
                const HighVectorBits lhs{Random(random),Random(random)},rhs{Random(random),Random(random)},acc{Random(random),Random(random)};
                CheckCase(program,bits,q,subtract,regs,lhs,rhs,acc,control);
            }
        }
    }
}

TEST_CASE("ARM64 vector FMA only evaluates active lanes and precisely exits unsupported controls or D Q0") {
    for (u32 bits:{16u,32u}) for (bool subtract:{false,true}) for (auto regs:Aliases) {
        const vector_fma_reference::Format f(bits);
        HighVectorBits lhs{},rhs{},acc{};
        for (u32 lane=0;lane<128/bits;++lane) {
            PutElement(lhs,bits,lane,lane<64/bits ? 0 : f.exponent_mask|1);
            PutElement(rhs,bits,lane,lane<64/bits ? u64(f.bias)<<f.fraction : f.exponent_mask);
            PutElement(acc,bits,lane,lane<64/bits ? 0 : f.exponent_mask|f.quiet|3);
        }
        Program program(Encode(bits,false,subtract,regs));
        for (u32 control:Controls) CheckCase(program,bits,false,subtract,regs,lhs,rhs,acc,control);
    }
    for (u32 bits:{16u,32u,64u}) for (bool q:{false,true}) for (bool subtract:{false,true}) {
        Program program(Encode(bits,q,subtract,Aliases[1]));
        for (u32 control:{0u,1u,2u,1u<<8,1u<<9,1u<<10,1u<<11,1u<<12,1u<<15,1u<<26,1u<<31}) {
            const bool reserved=bits==64 && !q;
            if (!reserved && !control) continue;
            Machine machine;machine.Context().fpcr=control;machine.Context().fpsr=0x08000091;machine.Context().pc=0xCAFE0123456789AB;
            const auto before=machine.Context();
            program.Run(machine,reserved ? swift::arm64::InterruptReason::ILL_CODE : swift::arm64::InterruptReason::FALLBACK);
            REQUIRE(std::memcmp(&machine.Context().v,&before.v,sizeof(before.v))==0);
            REQUIRE(machine.Context().r==before.r);REQUIRE(machine.Context().fpcr==before.fpcr);REQUIRE(machine.Context().fpsr==before.fpsr);
            REQUIRE(machine.Context().pc==(reserved ? before.pc : 0x1000));
            REQUIRE(machine.state->current_loc.Value()==0x1000);REQUIRE(machine.state->host_cpu_flags==0xB0000000);
        }
    }
}

TEST_CASE("ARM64 existing scalar FMA four sign combinations preserve fused NaN order during kernel extraction") {
    for (u32 bits:{16u,32u,64u}) for (u32 operation=0;operation<4;++operation) for (u32 rd:{0u,31u,30u,3u}) {
        const u32 control=((operation&1)^u32(operation>=2))|(operation>=2 ? 2u : 0u);
        const u32 type=bits==16 ? 3 : bits==64 ? 1 : 0;
        const u32 code=0x1F000000|(type<<22)|(operation>=2 ? 1u<<21 : 0)|(operation&1 ? 1u<<15 : 0)|
                       (30u<<16)|(3u<<10)|(31u<<5)|rd;
        Program program(code);
        for (u32 fpcr:Controls) for (auto values:Edges(bits,control&1)) {
            Machine machine;
            machine.SetVector(31,{values[0],0xCAFE0123456789AB});
            machine.SetVector(30,{values[1],0xDEADFEDCBA987654});
            machine.SetVector(3,{values[2],0xABCD1032547698EF});
            machine.Context().fpcr=fpcr;machine.Context().fpsr=0x08000002;
            const auto before=machine.Context();
            const auto expected=vector_fma_reference::Evaluate(values[0],values[1],values[2],bits,control,fpcr);
            program.Run(machine);
            REQUIRE(machine.GetVector(rd)==HighVectorBits{expected.value,0});
            REQUIRE(machine.Context().fpsr==(before.fpsr|expected.exceptions));
            REQUIRE(machine.Context().fpcr==before.fpcr);REQUIRE(machine.Context().r==before.r);
            REQUIRE(machine.state->host_cpu_flags==0xB0000000);REQUIRE(machine.state->current_loc.Value()==0x1004);
            for (u32 reg=0;reg<32;++reg) if (reg!=rd)
                REQUIRE(std::memcmp(&machine.Context().v[reg],&before.v[reg],sizeof(before.v[reg]))==0);
        }
    }
}
