#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "base/types.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;
using namespace vixl::aarch64;
namespace {
using Registers = std::array<u32, 3>;
constexpr std::array<Registers, 5> kAliases{{{0, 31, 30}, {31, 31, 30}, {30, 31, 30}, {0, 30, 30}, {30, 30, 30}}};

class IntegerReservedMemory final : public MemoryInterface {
public:
    explicit IntegerReservedMemory(u32 word,bool prefix) : code{prefix ? 0xD503201Fu : word, prefix ? word : 0xD4200000u, 0xD4200000u} {}
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
    std::array<u32, 3> code;
};
struct IntegerReservedMachine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(swift::arm64::ThreadContext64) + 32> storage{};
    backend::State* state = new(storage.data()) backend::State{};
    ~IntegerReservedMachine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() { return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin); }
};
struct IntegerReservedProgram {
    IntegerReservedMemory memory;
    u64 fault_pc;
    ir::Block block{0, 0x1000};
    ir::Assembler assembler{&block};
#if defined(SWIFT_HAS_X64_BACKEND)
    std::array<std::unique_ptr<backend::x64::Translator>, 2> native;
#endif
    explicit IntegerReservedProgram(u32 word,bool prefix) : memory(word,prefix), fault_pc(0x1000+(prefix ? 4 : 0)) {
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
            IntegerReservedMachine machine;
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
            // Integer exceptions must preserve virtual guest NZCV as well.
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
            REQUIRE(machine.state->current_loc.Value() == fault_pc);
        }
    }
};

u64 Random(u64& state) {
    state ^= state>>12;state ^= state<<25;state ^= state>>27;
    return state*0x2545F4914F6CDD1D;
}
void CheckReserved(const std::vector<u32>& words,bool has_rm) {
    u64 random = 0x93C467E37DB0C7A4;
    for (const u32 opcode : words) for (const auto regs : kAliases) for (bool prefix : {false,true}) {
        const u32 word = opcode | (regs[1]<<5) | regs[0] | (has_rm ? regs[2]<<16 : 0);
        INFO("reserved instruction=" << word << ", preceding NOP=" << prefix);
        IntegerReservedProgram program(word,prefix);
        for (u32 profile=0;profile<2;++profile) {
            swift::arm64::ThreadContext64 initial{};
            initial.fpcr = profile ? 0xFFFFFFFF : 0x03400000;
            initial.fpsr = profile ? 0x0800009F : 0x91;
            initial.pc = 0x8877665544332211;
            initial.fp = Random(random);initial.lr = Random(random);initial.sp = Random(random);
            initial.padding = 0x5342414C;
            for (auto& reg : initial.r) reg = Random(random);
            for (auto& reg : initial.v) {
                const std::array<u64,2> value{Random(random),Random(random)};
                std::memcpy(&reg,value.data(),16);
            }
            auto expected = initial;
            expected.padding = u32(swift::arm64::InterruptReason::ILL_CODE);
            program.Check(initial,expected);
        }
    }
}
}  // namespace

TEST_CASE("ARM64 reserved integer vector zero compares and unary D Q0 preserve complete state at exact fault PC") {
    std::vector<u32> words;
    for (const u32 op : {u32(NEON_CMGT_zero),u32(NEON_CMGE_zero),u32(NEON_CMEQ_zero),
                        u32(NEON_CMLT_zero),u32(NEON_CMLE_zero),u32(NEON_ABS),u32(NEON_NEG)})
        words.push_back(op|(3u<<22));
    REQUIRE(words.size()==7);
    CheckReserved(words,false);
}

TEST_CASE("ARM64 reserved integer vector compares pairwise minmax and multiply widths preserve complete state") {
    std::vector<u32> words;
    for (const u32 op : {u32(NEON_CMGT),u32(NEON_CMGE),u32(NEON_CMHI),u32(NEON_CMHS),
                        u32(NEON_CMEQ),u32(NEON_CMTST),u32(NEON_ADDP)})
        words.push_back(op|(3u<<22));
    for (const u32 op : {u32(NEON_SMAX),u32(NEON_SMIN),u32(NEON_UMAX),u32(NEON_UMIN),
                        u32(NEON_SMAXP),u32(NEON_SMINP),u32(NEON_UMAXP),u32(NEON_UMINP),
                        u32(NEON_MLA),u32(NEON_MLS)})
        for (bool q : {false,true}) words.push_back(op|(3u<<22)|(q ? u32(NEON_Q) : 0));
    REQUIRE(words.size()==27);
    CheckReserved(words,true);
}

TEST_CASE("ARM64 reserved integer widening D sources trap before changing aliases or status") {
    std::vector<u32> words;
    for (const u32 op : {u32(NEON_SADDL),u32(NEON_UADDL),u32(NEON_SADDW),u32(NEON_UADDW),
                        u32(NEON_SSUBL),u32(NEON_USUBL),u32(NEON_SSUBW),u32(NEON_USUBW),
                        u32(NEON_SMULL),u32(NEON_UMULL),u32(NEON_SMLAL),u32(NEON_UMLAL),
                        u32(NEON_SMLSL),u32(NEON_UMLSL)})
        for (bool q : {false,true}) words.push_back(op|(3u<<22)|(q ? u32(NEON_Q) : 0));
    REQUIRE(words.size()==28);
    CheckReserved(words,true);
}

TEST_CASE("ARM64 reserved SHRN D destinations reject every shift and upper variant at exact fault PC") {
    std::vector<u32> words;
    for (bool q : {false,true}) for (u32 shift=1;shift<=64;++shift)
        words.push_back(u32(NEON_SHRN)|((128u-shift)<<16)|(q ? u32(NEON_Q) : 0));
    REQUIRE(words.size()==128);
    CheckReserved(words,false);
}
