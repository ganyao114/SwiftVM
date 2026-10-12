#include <array>
#include <cstring>
#include <memory>
#include <catch2/catch_test_macros.hpp>
#include "arm64_high_narrow_reference.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;
using namespace vixl::aarch64;
namespace {
using HighNarrowBytes = std::array<u8, 16>;
using Registers = std::array<u32, 3>;
constexpr std::array<Registers, 5> kAliases{{{0, 31, 30}, {31, 31, 30}, {30, 31, 30}, {0, 30, 30}, {30, 30, 30}}};

class HighNarrowMemory final : public MemoryInterface {
public:
    explicit HighNarrowMemory(u32 word) : code{word, 0xD4200000} {}
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
struct HighNarrowMachine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(swift::arm64::ThreadContext64) + 32> storage{};
    backend::State* state = new(storage.data()) backend::State{};
    ~HighNarrowMachine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() { return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin); }
};
struct HighNarrowProgram {
    HighNarrowMemory memory;
    ir::Block block{0, 0x1000};
    ir::Assembler assembler{&block};
#if defined(SWIFT_HAS_X64_BACKEND)
    std::array<std::unique_ptr<backend::x64::Translator>, 2> native;
#endif
    explicit HighNarrowProgram(u32 word) : memory(word) {
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
            HighNarrowMachine machine;
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


const std::array<u32,4> Operations{NEON_ADDHN,NEON_RADDHN,NEON_SUBHN,NEON_RSUBHN};
u32 Encode(u32 operation,u32 bits,bool upper,Registers regs,u32 size_override=4) {
    const u32 size = size_override==4 ? bits==16 ? 0 : bits==32 ? 1 : 2 : size_override;
    return Operations[operation]|(size<<22)|(upper ? NEON_Q : 0)|(regs[2]<<16)|(regs[1]<<5)|regs[0];
}
u64 Random(u64& state) { state ^= state>>12;state ^= state<<25;state ^= state>>27;return state*0x2545F4914F6CDD1D; }
high_narrow_reference::Vector Read(const swift::arm64::ThreadContext64& context,u32 reg) {
    high_narrow_reference::Vector result;std::memcpy(result.data(),&context.v[reg],16);return result;
}
void CheckCase(HighNarrowProgram& program,u32 operation,u32 bits,bool upper,Registers regs,
               high_narrow_reference::Vector left,high_narrow_reference::Vector right,u64& random) {
    swift::arm64::ThreadContext64 initial{};
    initial.fpcr = 0xFFFFFFFF;initial.fpsr = random&1 ? 0x0800009F : 0x91;
    initial.pc = 0x8877665544332211;initial.fp = Random(random);initial.lr = Random(random);initial.sp = Random(random);
    for (auto& reg : initial.r) reg = Random(random);
    for (u32 reg=0;reg<32;++reg) { const std::array<u64,2> value{Random(random),Random(random)};std::memcpy(&initial.v[reg],value.data(),16); }
    std::memcpy(&initial.v[regs[1]],left.data(),16);std::memcpy(&initial.v[regs[2]],right.data(),16);
    const auto result = high_narrow_reference::Evaluate(Read(initial,regs[1]),Read(initial,regs[2]),Read(initial,regs[0]),bits,operation,upper);
    auto expected = initial;std::memcpy(&expected.v[regs[0]],result.data(),16);
    expected.padding = u32(swift::arm64::InterruptReason::BRK);program.Check(initial,expected);
}
std::array<u64,12> Edges(u32 bits) {
    const u64 mask = bits==64 ? ~u64{0} : (u64{1}<<bits)-1;
    const u64 half = u64{1}<<(bits/2-1), boundary = u64{1}<<(bits/2), sign = u64{1}<<(bits-1);
    return {0,1,mask,mask-1,half-1,half,half+1,boundary-1,boundary,boundary+1,sign-1,sign};
}
high_narrow_reference::Vector Pack(const std::array<u64,12>& edges,u32 bits,u32 index) {
    high_narrow_reference::Vector result{};
    for (u32 lane=0;lane<128/bits;++lane)
        result[lane*bits/64] |= edges[(index+lane)%edges.size()]<<(lane*bits%64);
    return result;
}
}

TEST_CASE("ARM64 high narrow add subtract and rounded forms follow exact modular arithmetic for all aliases") {
    u64 random = 0xA44DD228F1721301;
    for (u32 operation=0;operation<4;++operation) for (u32 bits : {16u,32u,64u})
    for (bool upper : {false,true}) for (auto regs : kAliases) {
        HighNarrowProgram program(Encode(operation,bits,upper,regs));
        const auto edges = Edges(bits);
        for (u32 a=0;a<edges.size();++a) for (u32 b=0;b<edges.size();++b) {
            INFO("operation="<<operation<<" bits="<<bits<<" upper="<<upper<<" rd="<<regs[0]<<" rn="<<regs[1]<<" rm="<<regs[2]<<" a="<<a<<" b="<<b);
            CheckCase(program,operation,bits,upper,regs,Pack(edges,bits,a),Pack(edges,bits,b),random);
        }
        for (u32 trial=0;trial<256;++trial)
            CheckCase(program,operation,bits,upper,regs,{Random(random),Random(random)},{Random(random),Random(random)},random);
    }
}

TEST_CASE("ARM64 high narrow rounding propagates carries across discarded half without setting saturation") {
    u64 random = 0xCA441E50B0A0D001;
    for (u32 operation=0;operation<4;++operation) for (u32 bits : {16u,32u,64u})
    for (bool upper : {false,true}) {
        const Registers regs{31,31,30};
        HighNarrowProgram program(Encode(operation,bits,upper,regs));
        const u64 boundary = u64{1}<<(bits/2), half = boundary/2;
        const u64 mask = bits==64 ? ~u64{0} : (u64{1}<<bits)-1;
        for (u32 low=0;low<256;++low) {
            high_narrow_reference::Vector left{},right{};
            for (u32 lane=0;lane<128/bits;++lane) {
                const u64 a = ((u64(low)<< (bits/2)) | (half + (lane%3)-1)) & mask;
                const u64 b = lane&1 ? 1 : mask;
                left[lane*bits/64] |= a<<(lane*bits%64);
                right[lane*bits/64] |= b<<(lane*bits%64);
            }
            CheckCase(program,operation,bits,upper,regs,left,right,random);
        }
    }
}

TEST_CASE("ARM64 high narrow rejects reserved size before changing any architectural state") {
    for (u32 operation=0;operation<4;++operation) for (bool upper : {false,true}) {
        HighNarrowProgram program(Encode(operation,16,upper,{31,31,30},3));
        swift::arm64::ThreadContext64 initial{};initial.fpcr=0xFFFFFFFF;initial.fpsr=0x0800009F;
        initial.pc=0x8877665544332211;auto expected=initial;
        expected.padding=u32(swift::arm64::InterruptReason::ILL_CODE);program.Check(initial,expected);
    }
}

