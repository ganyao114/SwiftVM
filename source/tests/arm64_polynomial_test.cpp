#include <array>
#include <cstring>
#include <memory>
#include <catch2/catch_test_macros.hpp>
#include "arm64_polynomial_reference.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;
using namespace vixl::aarch64;
namespace {
using PolynomialBytes = std::array<u8, 16>;
using Registers = std::array<u32, 3>;
constexpr std::array<Registers, 5> kAliases{{{0, 31, 30}, {31, 31, 30}, {30, 31, 30}, {0, 30, 30}, {30, 30, 30}}};

class PolynomialMemory final : public MemoryInterface {
public:
    explicit PolynomialMemory(u32 word) : code{word, 0xD4200000} {}
    bool Read(void* destination, size_t address, size_t size) override {
        if (address < 0x1000 || address - 0x1000 > sizeof(code) || size > sizeof(code) - (address - 0x1000)) return false;
        std::memcpy(destination, reinterpret_cast<u8*>(code.data()) + address - 0x1000, size); return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* pointer) override {
        const size_t address = reinterpret_cast<size_t>(pointer);
        return address >= 0x1000 && address - 0x1000 < sizeof(code)
            ? reinterpret_cast<u8*>(code.data()) + address - 0x1000 : nullptr;
    }
    std::array<u32, 2> code;
};
struct PolynomialMachine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(swift::arm64::ThreadContext64) + 32> storage{};
    backend::State* state = new(storage.data()) backend::State{};
    ~PolynomialMachine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() { return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin); }
};
struct PolynomialProgram {
    PolynomialMemory memory;
    ir::Block block{0, 0x1000};
    ir::Assembler assembler{&block};
#if defined(SWIFT_HAS_X64_BACKEND)
    std::array<std::unique_ptr<backend::x64::Translator>, 2> native;
#endif
    explicit PolynomialProgram(u32 word) : memory(word) {
        swift::arm64::A64Decoder(0x1000, &memory, &assembler).Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        for (u32 route = 0; route < 2; ++route) {
            native[route] = std::make_unique<backend::x64::Translator>(
                &block, route ? backend::x64::HostFeatures{} : backend::x64::HostFeatures::Detect());
            native[route]->Translate();
        }
#endif
    }
    void Check(const swift::arm64::ThreadContext64& initial, const swift::arm64::ThreadContext64& expected) {
        for (u32 route = 0; route <
#if defined(SWIFT_HAS_X64_BACKEND)
             3
#else
             1
#endif
             ; ++route) {
            PolynomialMachine machine;
            machine.Context() = initial;
            machine.state->host_cpu_flags = 0xB0000000;
            if (route == 0) backend::interp::Interpreter{*machine.state, &block}.Run();
#if defined(SWIFT_HAS_X64_BACKEND)
            else native[route - 1]->getCode<HaltReason(*)(backend::State*)>()(machine.state);
#endif
            const auto* actual_bytes = reinterpret_cast<const u8*>(&machine.Context());
            const auto* expected_bytes = reinterpret_cast<const u8*>(&expected);
            for (u32 byte = 0; byte < sizeof(expected); ++byte)
                if (actual_bytes[byte] != expected_bytes[byte]) {
                    INFO("route=" << route << ", context byte=" << byte << ", actual=" << u32(actual_bytes[byte])
                         << ", expected=" << u32(expected_bytes[byte]) << ", instruction=" << memory.code[0]);
                    REQUIRE(actual_bytes[byte] == expected_bytes[byte]);
                }
            REQUIRE(std::memcmp(&machine.Context(), &expected, sizeof(expected)) == 0);
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
            REQUIRE(machine.state->current_loc.Value() == (expected.padding == u32(swift::arm64::InterruptReason::BRK) ? 0x1004 : 0x1000));
        }
    }
};

u32 Encode(bool widening,u32 bits,bool q,Registers regs,u32 size_override=4) {
    const u32 size = size_override==4 ? bits==64 ? 3 : 0 : size_override;
    return (widening ? NEON_PMULL : NEON_PMUL)|(size<<22)|(q ? NEON_Q : 0)|(regs[2]<<16)|(regs[1]<<5)|regs[0];
}
u64 Random(u64& state) { state ^= state>>12;state ^= state<<25;state ^= state>>27;return state*0x2545F4914F6CDD1D; }
polynomial_reference::Vector Read(const swift::arm64::ThreadContext64& context,u32 reg) {
    polynomial_reference::Vector result;std::memcpy(result.data(),&context.v[reg],16);return result;
}
void CheckCase(PolynomialProgram& program,bool widening,u32 bits,bool q,Registers regs,
               polynomial_reference::Vector left,polynomial_reference::Vector right,u64& random) {
    swift::arm64::ThreadContext64 initial{};
    initial.fpcr = 0xFFFFFFFF;initial.fpsr = random&1 ? 0x0800009F : 0x91;
    initial.pc = 0x8877665544332211;initial.fp = Random(random);initial.lr = Random(random);initial.sp = Random(random);
    for (auto& reg : initial.r) reg = Random(random);
    for (u32 reg=0;reg<32;++reg) { const std::array<u64,2> value{Random(random),Random(random)};std::memcpy(&initial.v[reg],value.data(),16); }
    std::memcpy(&initial.v[regs[1]],left.data(),16);std::memcpy(&initial.v[regs[2]],right.data(),16);
    const auto result = polynomial_reference::Evaluate(Read(initial,regs[1]),Read(initial,regs[2]),widening,bits,q);
    auto expected = initial;std::memcpy(&expected.v[regs[0]],result.data(),16);
    expected.padding = u32(swift::arm64::InterruptReason::BRK);program.Check(initial,expected);
}
}

TEST_CASE("ARM64 PMUL and byte PMULL cover every polynomial byte pair with inactive halves and aliases") {
    u64 random = 0xE7037ED1A0B428FF;
    for (bool widening : {false,true}) for (bool q : {false,true}) for (auto regs : kAliases) {
        PolynomialProgram program(Encode(widening,8,q,regs));
        for (u32 a=0;a<256;++a) for (u32 b=0;b<256;b+=8) {
            polynomial_reference::Vector left{},right{};
            for (u32 lane=0;lane<16;++lane) {
                left[lane/8] |= u64(a)<<(lane%8*8);
                right[lane/8] |= u64((b+lane%8)&255)<<(lane%8*8);
            }
            CheckCase(program,widening,8,q,regs,left,right,random);
        }
        for (u32 trial=0;trial<64;++trial)
            CheckCase(program,widening,8,q,regs,{Random(random),Random(random)},{Random(random),Random(random)},random);
    }
}

TEST_CASE("ARM64 PMULL64 lower and upper select carryless full products with aliases and random patterns") {
    const std::array<u64,8> edges{0,1,2,u64{1}<<63,(u64{1}<<63)-1,~u64{0},0xAAAAAAAAAAAAAAAA,0x5555555555555555};
    u64 random = 0xE7037ED1A0B42ABC;
    for (bool q : {false,true}) for (auto regs : kAliases) {
        PolynomialProgram program(Encode(true,64,q,regs));
        for (u32 a=0;a<8;++a) for (u32 b=0;b<8;++b)
            CheckCase(program,true,64,q,regs,{edges[a],edges[(a+3)%8]},{edges[b],edges[(b+5)%8]},random);
        for (u32 trial=0;trial<512;++trial)
            CheckCase(program,true,64,q,regs,{Random(random),Random(random)},{Random(random),Random(random)},random);
    }
}

TEST_CASE("ARM64 polynomial forms reject reserved sizes without architectural mutation") {
    for (bool widening : {false,true}) for (bool q : {false,true}) for (u32 size : {1u,2u,3u}) {
        if (widening && size==3) continue;
        PolynomialProgram program(Encode(widening,8,q,{0,31,30},size));
        swift::arm64::ThreadContext64 initial{};initial.fpcr = 0xFFFFFFFF;initial.fpsr = 0x0800009F;
        initial.pc = 0x8877665544332211;auto expected = initial;expected.padding = u32(swift::arm64::InterruptReason::ILL_CODE);
        program.Check(initial,expected);
    }
}
