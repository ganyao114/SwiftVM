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
struct Ref { u16 value; u32 exceptions; };
Ref Reference(u16 value,u32 operation,u32 fpcr) {
    const u32 exponent=(value >> 10)&31, fraction=value&1023;
    const bool nan=exponent==31 && fraction, signaling=nan && !(fraction&512);
    if (nan) {
        if (operation==5) return {u16(fpcr&(1u<<25) ? 0x7E00 : value|512),signaling ? 1u : 0u};
        return {0,signaling || operation!=0 ? 1u : 0u};
    }
    if (operation==5)
        return {u16((value&0x8000) | ((exponent ? (~exponent)&31 : 30)<<10)),0};
    if (!exponent && fraction && (fpcr&(1u<<19))) value&=0x8000;
    const bool equal=(value&0x7FFF)==0, less=!equal && (value&0x8000), greater=!equal && !(value&0x8000);
    const std::array<bool,5> predicates{equal,greater,equal||greater,less,equal||less};
    return {u16(predicates[operation] ? 0xFFFF : 0),0};
}
const std::array<u32,6> Bases{NEON_FCMEQ_H_zero_scalar,NEON_FCMGT_H_zero_scalar,NEON_FCMGE_H_zero_scalar,
    NEON_FCMLT_H_zero_scalar,NEON_FCMLE_H_zero_scalar,NEON_FRECPX_H_scalar};
const std::array<u32,8> Controls{0,1u<<22,2u<<22,3u<<22,1u<<24,1u<<25,(1u<<19)|(1u<<25),1u<<19};
u32 Encode(u32 operation,u32 rd) { return Bases[operation] | (31u<<5) | rd; }
void CheckCase(Program& program,u32 operation,u32 rd,u16 source,u32 fpcr,bool sticky=false) {
    Machine machine;
    // The source's inactive lanes include signaling NaNs and subnormals.
    machine.SetVector(31,{0xFE017C0140000000 | source,0xFC017C0380010001});
    machine.Context().fpcr=fpcr;
    machine.Context().fpsr=0x08000002 | (sticky ? 1 : 0);
    const auto before=machine.Context();
    const auto expected=Reference(source,operation,fpcr);
    program.Run(machine);
    REQUIRE(machine.GetVector(rd)==HighVectorBits{expected.value,0});
    REQUIRE(machine.Context().fpsr==(before.fpsr|expected.exceptions));
    REQUIRE(machine.Context().fpcr==before.fpcr);
    REQUIRE(machine.Context().r==before.r);
    REQUIRE(machine.Context().fp==before.fp);
    REQUIRE(machine.Context().lr==before.lr);
    REQUIRE(machine.Context().sp==before.sp);
    REQUIRE(machine.state->host_cpu_flags==0xB0000000);
    REQUIRE(machine.state->current_loc.Value()==0x1004);
    for (u32 reg=0;reg<32;++reg) if (reg!=rd)
        REQUIRE(std::memcmp(&machine.Context().v[reg],&before.v[reg],sizeof(before.v[reg]))==0);
}
u64 Random(u64& state) { state^=state>>12;state^=state<<25;state^=state>>27;return state*0x2545F4914F6CDD1D; }
}

TEST_CASE("ARM64 scalar FP16 comparisons and reciprocal exponent cover every half bit pattern") {
    for (u32 operation=0;operation<6;++operation) {
        Program program(Encode(operation,31));
        for (u32 source=0;source<65536;++source) CheckCase(program,operation,31,u16(source),0);
    }
}

TEST_CASE("ARM64 scalar FP16 comparisons and reciprocal exponent preserve aliases inactive lanes sticky status") {
    const std::array<u16,22> edges{0,0x8000,1,0x8001,0x03FF,0x83FF,0x0400,0x8400,0x3C00,0xBC00,
        0x7BFF,0xFBFF,0x7C00,0xFC00,0x7E03,0xFE05,0x7C01,0xFC03,0x7C02,0xFC04,0x7FFF,0xFFFF};
    u64 random=0x5BE0CD19137E2179;
    for (u32 operation=0;operation<6;++operation) for (u32 rd:{0u,30u,31u}) {
        Program program(Encode(operation,rd));
        for (u32 control:Controls) {
            for (u16 source:edges) for (bool sticky:{false,true}) CheckCase(program,operation,rd,source,control,sticky);
            for (u32 sample=0;sample<256;++sample)
                CheckCase(program,operation,rd,u16(Random(random)),control,sample&1);
        }
    }
}

TEST_CASE("ARM64 scalar FP16 misc rejects unsupported controls and precisely falls back remaining allocated operations") {
    for (u32 operation=0;operation<6;++operation) {
        Program program(Encode(operation,31));
        for (u32 control:{1u,2u,1u<<8,1u<<9,1u<<10,1u<<11,1u<<12,1u<<15,1u<<26,1u<<31}) {
            Machine machine;machine.Context().fpcr=control;machine.Context().fpsr=0x08000091;
            machine.Context().pc=0xCAFE0123456789AB;
            const auto before=machine.Context();
            program.Run(machine,swift::arm64::InterruptReason::FALLBACK);
            REQUIRE(std::memcmp(&machine.Context().v,&before.v,sizeof(before.v))==0);
            REQUIRE(machine.Context().r==before.r);
            REQUIRE(machine.Context().fpsr==before.fpsr);
            REQUIRE(machine.Context().fpcr==before.fpcr);
            REQUIRE(machine.Context().pc==0x1000);
            REQUIRE(machine.state->current_loc.Value()==0x1000);
            REQUIRE(machine.state->host_cpu_flags==0xB0000000);
        }
    }
}
