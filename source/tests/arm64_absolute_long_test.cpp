#include <array>
#include <cstring>
#include <memory>
#include <catch2/catch_test_macros.hpp>
#include "arm64_absolute_long_reference.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;
using namespace vixl::aarch64;
namespace {
using AbsoluteLongBytes = std::array<u8, 16>;
using Registers = std::array<u32, 3>;
constexpr std::array<Registers, 5> kAliases{{{0, 31, 30}, {31, 31, 30}, {30, 31, 30}, {0, 30, 30}, {30, 30, 30}}};

class AbsoluteLongMemory final : public MemoryInterface {
public:
    explicit AbsoluteLongMemory(u32 word) : code{word, 0xD4200000} {}
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
struct AbsoluteLongMachine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(swift::arm64::ThreadContext64) + 32> storage{};
    backend::State* state = new(storage.data()) backend::State{};
    ~AbsoluteLongMachine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() { return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin); }
};
struct AbsoluteLongProgram {
    AbsoluteLongMemory memory;
    ir::Block block{0, 0x1000};
    ir::Assembler assembler{&block};
#if defined(SWIFT_HAS_X64_BACKEND)
    std::array<std::unique_ptr<backend::x64::Translator>, 2> native;
#endif
    explicit AbsoluteLongProgram(u32 word) : memory(word) {
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
            AbsoluteLongMachine machine;
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

u32 Encode(u32 bits,bool upper,u32 operation,Registers regs) {
    const std::array<u32,4> bases{NEON_SABDL,NEON_UABDL,NEON_SABAL,NEON_UABAL};
    const u32 size = bits==8 ? 0 : bits==16 ? 1 : bits==32 ? 2 : 3;
    return bases[operation]|(size<<22)|(upper ? NEON_Q : 0)|(regs[2]<<16)|(regs[1]<<5)|regs[0];
}
u64 Random(u64& state) { state ^= state>>12;state ^= state<<25;state ^= state>>27;return state*0x2545F4914F6CDD1D; }
absolute_long_reference::Vector Read(const swift::arm64::ThreadContext64& context,u32 reg) {
    absolute_long_reference::Vector result;std::memcpy(result.data(),&context.v[reg],16);return result;
}
void PutElement(absolute_long_reference::Vector& vector,u32 bits,u32 lane,u64 value) {
    const u64 mask = bits==64 ? ~u64{0} : (u64{1}<<bits)-1;const u32 half = lane*bits/64,shift = lane*bits%64;
    vector[half] = (vector[half]&~(mask<<shift))|((value&mask)<<shift);
}
void CheckCase(AbsoluteLongProgram& program,u32 bits,bool upper,u32 operation,Registers regs,
               absolute_long_reference::Vector left,absolute_long_reference::Vector right,absolute_long_reference::Vector accumulator,u64& random) {
    swift::arm64::ThreadContext64 initial{};
    initial.fpcr = 0xFFFFFFFF;initial.fpsr = random&1 ? 0x0800009F : 0x91;
    initial.pc = 0x8877665544332211;initial.fp = Random(random);initial.lr = Random(random);initial.sp = Random(random);
    for (auto& reg : initial.r) reg = Random(random);
    for (u32 reg=0;reg<32;++reg) { const std::array<u64,2> value{Random(random),Random(random)};std::memcpy(&initial.v[reg],value.data(),16); }
    std::memcpy(&initial.v[regs[0]],accumulator.data(),16);
    std::memcpy(&initial.v[regs[1]],left.data(),16);std::memcpy(&initial.v[regs[2]],right.data(),16);
    const auto result = absolute_long_reference::Evaluate(Read(initial,regs[1]),Read(initial,regs[2]),Read(initial,regs[0]),bits,operation,upper);
    auto expected = initial;std::memcpy(&expected.v[regs[0]],result.data(),16);
    expected.padding = u32(swift::arm64::InterruptReason::BRK);program.Check(initial,expected);
}
std::array<u64,13> Edges(u32 bits) {
    const u64 sign = u64{1}<<(bits-1),mask = bits==64 ? ~u64{0} : (u64{1}<<bits)-1;
    return {0,1,2,sign-2,sign-1,sign,sign+1,sign+2,mask-2,mask-1,mask,0xAAAAAAAAAAAAAAAA&mask,0x5555555555555555&mask};
}
}

TEST_CASE("ARM64 absolute long differences and wrapping accumulates cover complete width and alias boundaries") {
    u64 random = 0xE7037ED1A0B429FF;
    for (u32 bits : {8u,16u,32u}) for (bool upper : {false,true}) for (u32 operation=0;operation<4;++operation) for (auto regs : kAliases) {
        AbsoluteLongProgram program(Encode(bits,upper,operation,regs));const auto edges = Edges(bits),out_edges = Edges(bits*2);
        for (u32 a=0;a<13;++a) for (u32 b=0;b<13;++b) {
            absolute_long_reference::Vector left{},right{},accumulator{};
            for (u32 lane=0;lane<128/bits;++lane) {
                PutElement(left,bits,lane,edges[(a+lane)%13]);PutElement(right,bits,lane,edges[(b+lane*3)%13]);
            }
            for (u32 lane=0;lane<64/bits;++lane) PutElement(accumulator,bits*2,lane,out_edges[(a+lane*5)%13]);
            CheckCase(program,bits,upper,operation,regs,left,right,accumulator,random);
        }
    }
}

TEST_CASE("ARM64 byte absolute long forms exhaust every signed unsigned operand pair") {
    u64 random = 0xE7037ED1A0B42CCC;const Registers regs{0,31,30};
    for (bool upper : {false,true}) for (u32 operation=0;operation<4;++operation) {
        AbsoluteLongProgram program(Encode(8,upper,operation,regs));
        for (u32 a=0;a<256;++a) for (u32 b=0;b<256;b+=8) {
            absolute_long_reference::Vector left{},right{};
            for (u32 lane=0;lane<16;++lane) { PutElement(left,8,lane,a);PutElement(right,8,lane,b+lane%8); }
            CheckCase(program,8,upper,operation,regs,left,right,{0xFFFFFFFF80007FFF,0x80008000FFFFFFFF},random);
        }
    }
}

TEST_CASE("ARM64 long absolute differences select complete halves and preserve full state for random aliases") {
    u64 random = 0xE7037ED1A0B42ABC;
    for (u32 bits : {8u,16u,32u}) for (bool upper : {false,true}) for (u32 operation=0;operation<4;++operation) for (auto regs : kAliases) {
        AbsoluteLongProgram program(Encode(bits,upper,operation,regs));
        for (u32 trial=0;trial<128;++trial) {
            absolute_long_reference::Vector left{Random(random),Random(random)},right{Random(random),Random(random)},accumulator{Random(random),Random(random)};
            if (trial<2) { left = trial ? absolute_long_reference::Vector{~u64{0},0} : absolute_long_reference::Vector{0,~u64{0}};right = {0,0}; }
            CheckCase(program,bits,upper,operation,regs,left,right,accumulator,random);
        }
    }
}

TEST_CASE("ARM64 absolute long differences reject reserved D source sizes without mutation") {
    for (bool upper : {false,true}) for (u32 operation=0;operation<4;++operation) {
        AbsoluteLongProgram program(Encode(64,upper,operation,{0,31,30}));
        swift::arm64::ThreadContext64 initial{};initial.fpcr = 0xFFFFFFFF;initial.fpsr = 0x0800009F;
        initial.pc = 0x8877665544332211;auto expected = initial;expected.padding = u32(swift::arm64::InterruptReason::ILL_CODE);
        program.Check(initial,expected);
    }
}
