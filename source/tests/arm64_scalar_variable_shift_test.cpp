#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "arm64_scalar_variable_shift_reference.h"
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


using Registers = std::array<u32,3>;  // Rd, Rn, Rm.
const std::array<Registers,5> Aliases{{{0,30,31},{30,30,31},{31,30,31},{0,31,31},{31,31,31}}};
const std::array<u32,8> Operations{NEON_SSHL_scalar,NEON_USHL_scalar,NEON_SRSHL_scalar,NEON_URSHL_scalar,
    NEON_SQSHL_scalar,NEON_UQSHL_scalar,NEON_SQRSHL_scalar,NEON_UQRSHL_scalar};
u64 Mask(u32 bits) { return bits == 64 ? ~u64{0} : (u64{1} << bits)-1; }
u64 Random(u64& state) { state ^= state>>12; state ^= state<<25; state ^= state>>27; return state*0x2545F4914F6CDD1D; }
u32 Encode(u32 operation,u32 bits,Registers regs) {
    const u32 size = bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : 3;
    return Operations[operation] | (size<<22) | (regs[2]<<16) | (regs[1]<<5) | regs[0];
}
void CheckCase(Program& program,u32 operation,u32 bits,Registers regs,u64 value,u64 count,u32 control,u32 sticky) {
    Machine machine;
    machine.SetVector(regs[1],{value,0xFEDCBA9876543210});
    machine.SetVector(regs[2],{count,0x0123456789ABCDEF});
    machine.Context().fpcr = control; machine.Context().fpsr = sticky;
    const auto before = machine.Context();
    const auto reference = scalar_variable_shift_reference::Evaluate(
        machine.GetVector(regs[1])[0],machine.GetVector(regs[2])[0],bits,operation);
    auto expected = before;
    const HighVectorBits destination{reference.value,0};
    std::memcpy(&expected.v[regs[0]],destination.data(),16);
    expected.fpsr |= reference.saturated ? 1u<<27 : 0;
    expected.padding = u32(swift::arm64::InterruptReason::BRK);
    // The BRK visitor leaves the context PC at its caller's value; current_loc
    // carries precise instruction publication separately.
    program.Run(machine);
    REQUIRE(std::memcmp(&machine.Context(),&expected,sizeof(expected)) == 0);
    REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    REQUIRE(machine.state->current_loc.Value() == 0x1004);
}
std::array<u64,12> Edges(u32 bits) {
    const u64 sign = u64{1}<<(bits-1), mask = Mask(bits);
    return {0,1,2,3,sign-1,sign,sign+1,mask,mask-1,mask-2,sign/2,sign/2-1};
}
}

TEST_CASE("ARM64 scalar variable shifts cover every signed low byte count and boundary input") {
    for (u32 operation=0; operation<8; ++operation) for (u32 bits : {8u,16u,32u,64u}) {
        if (operation<4 && bits!=64) continue;
        const auto edges = Edges(bits);
        for (auto regs : Aliases) {
            Program program(Encode(operation,bits,regs));
            for (u32 byte=0; byte<256; ++byte) for (u32 profile : {0u,1u,4u,5u,7u,8u}) {
                INFO("operation="<<operation<<" bits="<<bits<<" rd="<<regs[0]<<" rn="<<regs[1]<<" rm="<<regs[2]<<" count="<<byte<<" profile="<<profile);
                const u64 poisoned = edges[profile] | (bits == 64 ? 0 : ~Mask(bits));
                const u64 count = (profile&1 ? 0xA5A5A5A5A5A5A500 : 0x5A5A5A5A5A5A5A00) | byte;
                CheckCase(program,operation,bits,regs,poisoned,count,profile&1 ? 0xFFFFFFFF : 0,profile&1 ? 0x0800009F : 0x9F);
            }
        }
    }
}

TEST_CASE("ARM64 scalar variable shifts directed rounding saturation aliases and random bit patterns") {
    u64 seed = 0xB8053256127A0901;
    for (u32 operation=0; operation<8; ++operation) for (u32 bits : {8u,16u,32u,64u}) {
        if (operation<4 && bits!=64) continue;
        const auto edges = Edges(bits);
        for (auto regs : Aliases) {
            Program program(Encode(operation,bits,regs));
            for (s32 count : {-128,-s32(bits)-1,-s32(bits),1-s32(bits),-2,-1,0,1,2,s32(bits)-1,s32(bits),s32(bits)+1,127}) {
                for (u64 value : edges) CheckCase(program,operation,bits,regs,value,
                    (Random(seed)&~u64{255}) | u8(count),0xFFFFFFFF,0x82);
            }
            for (u32 trial=0; trial<96; ++trial)
                CheckCase(program,operation,bits,regs,Random(seed),Random(seed),u32(Random(seed)),trial&1 ? 0x08000092 : 0x92);
        }
    }
}

TEST_CASE("ARM64 scalar nonsaturating variable shifts reject reserved byte halfword and word forms") {
    const Registers regs{31,30,31};
    for (u32 operation=0; operation<4; ++operation) for (u32 bits : {8u,16u,32u}) {
        Program program(Encode(operation,bits,regs));
        Machine machine; machine.Context().fpcr = 0xFFFFFFFF; machine.Context().fpsr = 0x0800009F;
        const auto before = machine.Context();
        program.Run(machine,swift::arm64::InterruptReason::ILL_CODE);
        auto expected = before; expected.padding = u32(swift::arm64::InterruptReason::ILL_CODE);
        REQUIRE(std::memcmp(&machine.Context(),&expected,sizeof(expected)) == 0);
        REQUIRE(machine.state->current_loc.Value() == 0x1000);
        REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    }
}

