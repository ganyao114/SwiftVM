#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "arm64_mixed_sat_reference.h"
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
struct Shape { u32 bits;bool q,scalar; };
const std::array<Shape,11> Shapes{{{8,false,false},{8,true,false},{16,false,false},{16,true,false},
    {32,false,false},{32,true,false},{64,true,false},{8,true,true},{16,true,true},{32,true,true},{64,true,true}}};
u32 Encode(Shape shape,u32 operation,Registers regs) {
    const std::array<u32,4> bases{NEON_SUQADD,NEON_USQADD,NEON_SQABS,NEON_SQNEG};
    const u32 size = shape.bits==8 ? 0 : shape.bits==16 ? 1 : shape.bits==32 ? 2 : 3;
    return bases[operation]|(size<<22)|(shape.q ? NEON_Q : 0)|(shape.scalar ? NEONScalar : 0)|(regs[1]<<5)|regs[0];
}
u64 Element(HighVectorBits vector,u32 bits,u32 lane) {
    return (vector[lane*bits/64]>>(lane*bits%64)) & (bits==64 ? ~u64{0} : (u64{1}<<bits)-1);
}
void PutElement(HighVectorBits& vector,u32 bits,u32 lane,u64 value) {
    const u64 mask = bits==64 ? ~u64{0} : (u64{1}<<bits)-1;const u32 half = lane*bits/64,shift = lane*bits%64;
    vector[half] = (vector[half]&~(mask<<shift))|((value&mask)<<shift);
}
void CheckCase(Program& program,Shape shape,u32 operation,Registers regs,HighVectorBits source,HighVectorBits accumulator,u32 sticky) {
    Machine machine;machine.SetVector(regs[0],accumulator);machine.SetVector(regs[1],source);
    machine.Context().fpcr = 0xFFFFFFFF;machine.Context().fpsr = sticky;
    const auto before = machine.Context();const auto actual_accumulator = machine.GetVector(regs[0]);
    HighVectorBits expected{};u32 flags = sticky;
    const u32 active = shape.scalar ? 1 : (shape.q ? 128 : 64)/shape.bits;
    for (u32 lane=0;lane<active;++lane) {
        const auto result = mixed_sat_reference::Evaluate(Element(source,shape.bits,lane),Element(actual_accumulator,shape.bits,lane),shape.bits,operation);
        PutElement(expected,shape.bits,lane,result.value);flags |= result.saturated ? 1u<<27 : 0;
    }
    program.Run(machine);
    auto wanted = before;std::memcpy(&wanted.v[regs[0]],expected.data(),16);wanted.fpsr = flags;wanted.padding = u32(swift::arm64::InterruptReason::BRK);
    REQUIRE(std::memcmp(&machine.Context(),&wanted,sizeof(wanted)) == 0);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);REQUIRE(machine.state->current_loc.Value() == 0x1004);
}
std::array<u64,13> Edges(u32 bits) {
    const u64 sign = u64{1}<<(bits-1),mask = bits==64 ? ~u64{0} : (u64{1}<<bits)-1;
    return {0,1,2,sign-2,sign-1,sign,sign+1,sign+2,mask-2,mask-1,mask,0xAAAAAAAAAAAAAAAA&mask,0x5555555555555555&mask};
}
u64 Random(u64& state) { state ^= state>>12;state ^= state<<25;state ^= state>>27;return state*0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 mixed sign saturating accumulates and vector unary forms cover signed unsigned limits and QC") {
    for (auto shape : Shapes) for (u32 operation=0;operation<4;++operation) {
        if (shape.scalar && operation>=2) continue;
        const auto edges = Edges(shape.bits);
        for (auto regs : Aliases) {
            Program program(Encode(shape,operation,regs));
            for (u32 a=0;a<13;++a) for (u32 b=0;b<13;++b) {
                HighVectorBits source{},accumulator{};
                for (u32 lane=0;lane<128/shape.bits;++lane) {
                    PutElement(source,shape.bits,lane,edges[(a+lane)%13]);
                    PutElement(accumulator,shape.bits,lane,edges[(b+lane*3)%13]);
                }
                CheckCase(program,shape,operation,regs,source,accumulator,(a+b)&1 ? 0x0800009F : 0x91);
            }
        }
    }
}

TEST_CASE("ARM64 mixed sign byte saturation exhausts scalar operand pairs") {
    for (u32 operation : {0u,1u}) {
        const Shape shape{8,true,true};const Registers regs{0,31};Program program(Encode(shape,operation,regs));
        for (u32 source=0;source<256;++source) for (u32 accumulator=0;accumulator<256;++accumulator)
            CheckCase(program,shape,operation,regs,{source,~u64{0}},{accumulator,~u64{0}},0x91);
    }
}

TEST_CASE("ARM64 mixed saturating forms ignore inactive poison retain sticky QC and snapshot aliases") {
    u64 random = 0xA028442668ABCDEF;
    for (auto shape : Shapes) for (u32 operation=0;operation<4;++operation) for (auto regs : Aliases) {
        if (shape.scalar && operation>=2) continue;
        Program program(Encode(shape,operation,regs));
        for (u32 trial=0;trial<128;++trial) {
            HighVectorBits source{Random(random),Random(random)},accumulator{Random(random),Random(random)};
            if (trial<2) {
                const u32 active = shape.scalar ? 1 : (shape.q ? 128 : 64)/shape.bits;
                for (u32 lane=0;lane<128/shape.bits;++lane) {
                    PutElement(source,shape.bits,lane,lane<active ? 0 : u64{1}<<(shape.bits-1));
                    PutElement(accumulator,shape.bits,lane,0);
                }
            }
            CheckCase(program,shape,operation,regs,source,accumulator,trial&1 ? 0x0800009F : 0x91);
        }
    }
}

TEST_CASE("ARM64 mixed vector saturating forms reject D Q0 precisely") {
    for (u32 operation=0;operation<4;++operation) {
        Program program(Encode({64,false,false},operation,{0,31}));Machine machine;
        const auto before = machine.Context();program.Run(machine,swift::arm64::InterruptReason::ILL_CODE);
        auto expected = before;expected.padding = u32(swift::arm64::InterruptReason::ILL_CODE);
        REQUIRE(std::memcmp(&machine.Context(),&expected,sizeof(expected)) == 0);
        REQUIRE(machine.state->current_loc.Value() == 0x1000);REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    }
}
