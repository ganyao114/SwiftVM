#include <array>
#include <cstring>
#include <memory>
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
using DotBytes = std::array<u8, 16>;
using Registers = std::array<u32, 3>;
constexpr std::array<Registers, 5> kAliases{{{0, 31, 30}, {31, 31, 30}, {30, 31, 30}, {0, 30, 30}, {30, 30, 30}}};

class DotMemory final : public MemoryInterface {
public:
    explicit DotMemory(u32 word) : code{word, 0xD4200000} {}
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
struct DotMachine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(swift::arm64::ThreadContext64) + 32> storage{};
    backend::State* state = new(storage.data()) backend::State{};
    ~DotMachine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() { return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin); }
};
struct DotProgram {
    DotMemory memory;
    ir::Block block{0, 0x1000};
    ir::Assembler assembler{&block};
#if defined(SWIFT_HAS_X64_BACKEND)
    std::array<std::unique_ptr<backend::x64::Translator>, 2> native;
#endif
    explicit DotProgram(u32 word) : memory(word) {
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
            DotMachine machine;
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

u32 Encode(bool sign, bool q, bool indexed, u32 index, Registers regs, u32 size = 2) {
    const u32 op = indexed ? (sign ? NEON_SDOT_byelement : NEON_UDOT_byelement)
                           : (sign ? NEON_SDOT : NEON_UDOT);
    return op | (size << 22) | (q ? NEON_Q : 0) | (regs[2] << 16) | (regs[1] << 5) | regs[0]
           | (indexed ? ((index >> 1) << 11) | ((index & 1) << 21) : 0);
}
u64 Random(u64& state) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 0x2545F4914F6CDD1D; }
DotBytes ReadVector(const swift::arm64::ThreadContext64& context, u32 reg) {
    DotBytes result; std::memcpy(result.data(), &context.v[reg], 16); return result;
}
void CheckCase(DotProgram& program, bool sign, bool q, bool indexed, u32 index, Registers regs, u64& random, u32 profile) {
    swift::arm64::ThreadContext64 initial{};
    initial.fpcr = 0x01400002; initial.fpsr = 0x0800009F;
    for (u32 reg = 0; reg < 32; ++reg) {
        const std::array<u64, 2> bits{Random(random), Random(random)};
        std::memcpy(&initial.v[reg], bits.data(), 16);
    }
    if (profile < 8) {
        DotBytes lhs{}, rhs{};
        const std::array<u8, 8> edges{0, 1, 127, 128, 129, 254, 255, 17};
        for (u32 n = 0; n < 16; ++n) {
            lhs[n] = edges[(profile + n) % edges.size()];
            rhs[n] = edges[(profile + n * 3) % edges.size()];
        }
        std::memcpy(&initial.v[regs[1]], lhs.data(), 16);
        std::memcpy(&initial.v[regs[2]], rhs.data(), 16);
        if (regs[0] != regs[1] && regs[0] != regs[2]) {
            const std::array<u32, 4> accumulators{0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0};
            std::memcpy(&initial.v[regs[0]], accumulators.data(), 16);
        }
    }
    const auto lhs = ReadVector(initial, regs[1]), rhs = ReadVector(initial, regs[2]);
    const auto old = ReadVector(initial, regs[0]);
    DotBytes result{};
    for (u32 lane = 0; lane < (q ? 4 : 2); ++lane) {
        u32 accumulator; std::memcpy(&accumulator, old.data() + 4 * lane, 4);
        s64 sum = accumulator;
        for (u32 element = 0; element < 4; ++element) {
            const u32 a = lhs[4 * lane + element], b = rhs[4 * (indexed ? index : lane) + element];
            const s32 left = sign && a >= 128 ? s32(a) - 256 : s32(a);
            const s32 right = sign && b >= 128 ? s32(b) - 256 : s32(b);
            sum += s64(left) * right;
        }
        const u32 value = u32(sum); std::memcpy(result.data() + 4 * lane, &value, 4);
    }
    auto expected = initial;
    std::memcpy(&expected.v[regs[0]], result.data(), 16);
    expected.padding = u32(swift::arm64::InterruptReason::BRK);
    program.Check(initial, expected);
}
}

TEST_CASE("ARM64 signed and unsigned dot products wrap accumulate bytes with all aliases and indexed groups") {
    u64 random = 0xE7037ED1A0B428DB;
    for (bool sign : {false, true}) for (bool q : {false, true}) for (bool indexed : {false, true})
        for (u32 index = 0; index < (indexed ? 4 : 1); ++index) for (auto regs : kAliases) {
            DotProgram program(Encode(sign, q, indexed, index, regs));
            for (u32 profile = 0; profile < 72; ++profile)
                CheckCase(program, sign, q, indexed, index, regs, random, profile);
        }
}

TEST_CASE("ARM64 dot products reject reserved sizes before writing state") {
    for (bool sign : {false, true}) for (bool q : {false, true}) for (bool indexed : {false, true})
        for (u32 size : {0u, 1u, 3u}) {
            DotProgram program(Encode(sign, q, indexed, 3, kAliases[0], size));
            swift::arm64::ThreadContext64 initial{};
            initial.fpcr = 0x01400002; initial.fpsr = 0x0800009F;
            std::memset(initial.v.data(), 0x93, sizeof(initial.v));
            auto expected = initial;
            expected.padding = u32(swift::arm64::InterruptReason::ILL_CODE);
            program.Check(initial, expected);
        }
}
