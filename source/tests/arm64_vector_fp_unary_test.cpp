#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "arm64_vector_fp_unary_reference.h"
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

using Registers = std::array<u32, 2>;
const std::array<Registers, 3> Aliases{{{0,31},{31,31},{30,31}}};
const std::array<u32, 8> Controls{0,1u<<22,2u<<22,3u<<22,1u<<24,1u<<25,(1u<<19)|(1u<<25),1u<<19};
u64 Element(HighVectorBits vector, u32 bits, u32 lane) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    return (vector[lane * bits / 64] >> (lane * bits % 64)) & mask;
}
void PutElement(HighVectorBits& vector, u32 bits, u32 lane, u64 value) {
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const u32 half = lane * bits / 64, shift = lane * bits % 64;
    vector[half] = (vector[half] & ~(mask << shift)) | ((value & mask) << shift);
}
u32 Encode(u32 bits, bool q, u32 operation, Registers regs) {
    const std::array<u32, 8> bases{NEON_FSQRT,NEON_FRINTN,NEON_FRINTM,NEON_FRINTP,NEON_FRINTZ,NEON_FRINTA,NEON_FRINTX,NEON_FRINTI};
    return bases[operation] | (q ? NEON_Q : 0) | (bits == 64 ? 1u<<22 : 0) | (regs[1]<<5) | regs[0];
}
void CheckCase(Program& program, u32 bits, bool q, u32 operation, Registers regs, HighVectorBits source, u32 fpcr, u32 sticky = 0x08000002) {
    Machine machine; machine.SetVector(regs[1],source);
    machine.Context().fpcr = fpcr; machine.Context().fpsr = sticky;
    const auto before = machine.Context();
    HighVectorBits expected{};
    u32 flags = before.fpsr;
    for (u32 lane = 0; lane < (q ? 128u : 64u)/bits; ++lane) {
        const auto result = vector_fp_unary_reference::Evaluate(Element(source,bits,lane),bits,operation,fpcr);
        PutElement(expected,bits,lane,result.value); flags |= result.exceptions;
    }
    program.Run(machine);
    REQUIRE(machine.GetVector(regs[0]) == expected); REQUIRE(machine.Context().fpsr == flags);
    REQUIRE(machine.Context().fpcr == before.fpcr); REQUIRE(machine.Context().r == before.r);
    REQUIRE(machine.Context().fp == before.fp); REQUIRE(machine.Context().lr == before.lr); REQUIRE(machine.Context().sp == before.sp);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000); REQUIRE(machine.state->current_loc.Value() == 0x1004);
    for (u32 reg = 0; reg < 32; ++reg) if (reg != regs[0])
        REQUIRE(std::memcmp(&machine.Context().v[reg], &before.v[reg], sizeof(before.v[reg])) == 0);
}
std::vector<u64> Edges(u32 bits) {
    const u32 fraction = bits == 32 ? 23 : 52, bias = bits == 32 ? 127 : 1023;
    const u64 sign = u64{1}<<(bits-1), infinity = u64(2*bias+1)<<fraction, quiet = u64{1}<<(fraction-1);
    const u64 one = u64(bias)<<fraction, two = u64(bias+1)<<fraction, half = u64(bias-1)<<fraction, minimum = u64{1}<<fraction;
    return {0,sign,one,two,u64(bias+2)<<fraction,u64(bias+4)<<fraction,
        half,sign|half,one|quiet,sign|one|quiet,half-1,half+1,sign|(half-1),sign|(half+1),
        1,sign|1,minimum-1,sign|(minimum-1),minimum,sign|minimum,minimum+1,sign|(minimum+1),
        infinity,sign|infinity,infinity-1,sign|(infinity-1),infinity|quiet|3,sign|infinity|quiet|5,
        infinity|1,sign|infinity|3,one-1,one+1,(u64(bias+fraction-1)<<fraction)+1,
        (u64(bias+fraction)<<fraction)+1,((u64(bias+2)<<fraction)|quiet)-1};
}
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 S D vector FSQRT FRINT boundaries match independent integer roots and exact rational rounding") {
    for (u32 bits : {32u,64u}) for (bool q : {false,true}) for (u32 operation = 0; operation < 8; ++operation) {
        if (bits == 64 && !q) continue;
        const auto edges = Edges(bits);
        for (auto regs : Aliases) {
            Program program(Encode(bits,q,operation,regs));
            for (u32 control : Controls) for (u32 start = 0; start < edges.size(); ++start) {
                HighVectorBits source{};
                for (u32 lane = 0; lane < 128/bits; ++lane) PutElement(source,bits,lane,edges[(start+lane)%edges.size()]);
                CheckCase(program,bits,q,operation,regs,source,control);
            }
        }
    }
}

TEST_CASE("ARM64 vector S D unary random aliases preserve complete state sticky exception and QC bits") {
    u64 random = 0x8375A8D4C238EA17;
    for (u32 bits : {32u,64u}) for (bool q : {false,true}) for (u32 operation = 0; operation < 8; ++operation) {
        if (bits == 64 && !q) continue;
        for (auto regs : Aliases) {
            Program program(Encode(bits,q,operation,regs));
            for (u32 control : Controls) for (u32 sample = 0; sample < 64; ++sample)
                CheckCase(program,bits,q,operation,regs,{Random(random),Random(random)},control,sample & 1 ? 0x0800009F : 0x08000002);
        }
    }
}

TEST_CASE("ARM64 S Q0 unary ignores inactive NaN and FZ subnormal poison and X alone records rounding IX") {
    for (u32 operation = 0; operation < 8; ++operation) for (auto regs : Aliases) {
        Program program(Encode(32,false,operation,regs));
        for (u32 control : Controls) CheckCase(program,32,false,operation,regs,{0x3F8000003F800000,0x7F800001FF800003},control);
        CheckCase(program,32,false,operation,regs,{0x3F8000003F800000,0x0000000180000001},1u<<24);
    }
    for (u32 bits : {32u,64u}) for (u32 control : Controls) {
        const u32 fraction = bits == 32 ? 23 : 52, bias = bits == 32 ? 127 : 1023;
        const u64 half = u64(bias-1)<<fraction;
        const auto exact = vector_fp_unary_reference::Evaluate(half,bits,6,control);
        const auto quiet = vector_fp_unary_reference::Evaluate(half,bits,7,control);
        REQUIRE(exact.exceptions == 16); REQUIRE(quiet.exceptions == 0); REQUIRE(exact.value == quiet.value);
        HighVectorBits source{};
        for (u32 lane = 0; lane < 128/bits; ++lane) PutElement(source,bits,lane,half);
        for (u32 operation : {6u,7u}) {
            Program program(Encode(bits,true,operation,Aliases[0]));
            CheckCase(program,bits,true,operation,Aliases[0],source,control);
        }
    }
}

TEST_CASE("ARM64 vector S D unary unsupported controls and reserved D Q0 exit before modifying arithmetic state") {
    for (u32 bits : {32u,64u}) for (bool q : {false,true}) for (u32 operation = 0; operation < 8; ++operation) {
        const bool reserved = bits == 64 && !q;
        Program program(Encode(bits,q,operation,Aliases[1]));
        for (u32 control : {0u,1u,2u,1u<<8,1u<<9,1u<<10,1u<<11,1u<<12,1u<<15,1u<<26,1u<<31}) {
            if (!reserved && !control) continue;
            Machine machine; machine.Context().fpcr = control; machine.Context().fpsr = 0x08000091;
            machine.Context().pc = 0xCAFE0123456789AB;
            const auto before = machine.Context();
            program.Run(machine,reserved ? swift::arm64::InterruptReason::ILL_CODE : swift::arm64::InterruptReason::FALLBACK);
            REQUIRE(std::memcmp(&machine.Context().v,&before.v,sizeof(before.v)) == 0);
            REQUIRE(machine.Context().fpsr == before.fpsr); REQUIRE(machine.Context().fpcr == before.fpcr); REQUIRE(machine.Context().r == before.r);
            REQUIRE(machine.Context().pc == (reserved ? before.pc : 0x1000)); REQUIRE(machine.state->current_loc.Value() == 0x1000);
        }
    }
}

