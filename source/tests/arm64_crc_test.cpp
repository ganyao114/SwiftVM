#include <array>
#include <cstring>
#include <vector>
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
template<typename Context>
auto& Gpr(Context& context, u32 reg) {
    return reg < 29 ? context.r[reg] : reg == 29 ? context.fp : context.lr;
}
u32 Reverse(u32 value) {
    u32 result = 0;
    for (u32 bit = 0; bit < 32; ++bit) result |= ((value >> bit) & 1) << (31 - bit);
    return result;
}
u32 Reference(u32 accumulator, u64 data, u32 width, bool castagnoli) {
    // Normal (non-reflected) polynomial division with explicit input/output
    // reflection, independent of both runtime implementations' reflected loops.
    u32 remainder = Reverse(accumulator);
    const u32 polynomial = castagnoli ? 0x1EDC6F41 : 0x04C11DB7;
    for (u32 bit = 0; bit < width; ++bit) {
        const bool reduce = ((remainder >> 31) ^ (data >> bit)) & 1;
        remainder <<= 1;
        if (reduce) remainder ^= polynomial;
    }
    return Reverse(remainder);
}
class InstructionMemory final : public MemoryInterface {
public:
    explicit InstructionMemory(std::vector<u32> instructions) : code(std::move(instructions)) { code.push_back(0xD4200000); }
    bool Read(void* destination, size_t address, size_t size) override {
        if (address < 0x1000 || address - 0x1000 + size > code.size() * 4) return false;
        std::memcpy(destination, reinterpret_cast<u8*>(code.data()) + address - 0x1000, size);
        return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* pointer) override {
        const auto address = reinterpret_cast<size_t>(pointer);
        return address >= 0x1000 && address - 0x1000 < code.size() * 4
            ? reinterpret_cast<u8*>(code.data()) + address - 0x1000 : nullptr;
    }
    std::vector<u32> code;
};
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(swift::arm64::ThreadContext64) + 32> storage{};
    backend::State* state = new (storage.data()) backend::State{};
    Machine() {
        state->host_cpu_flags = 0xB0000000;
        Context().fpsr = 0x0800009F;
        Context().fpcr = 0x01400000;
        for (u32 reg = 0; reg < 31; ++reg) Gpr(Context(), reg) = 0xDEADC0DE00000000 | (u64(reg + 1) * 0x1020304);
        Context().sp = 0x123456789ABCDEF0;
    }
    ~Machine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() {
        return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin);
    }
    u64 Register(u32 reg) { return reg == 31 ? 0 : Gpr(Context(), reg); }
    void Run(std::vector<u32> instructions, swift::arm64::InterruptReason reason = swift::arm64::InterruptReason::BRK) {
        InstructionMemory memory(std::move(instructions));
        ir::Block block(0, 0x1000);
        ir::Assembler assembler(&block);
        swift::arm64::A64Decoder decoder(0x1000, &memory, &assembler);
        decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto initial = Context();
        const auto initial_flags = state->host_cpu_flags;
        const auto initial_location = state->current_loc;
#endif
        backend::interp::Interpreter interpreter(*state, &block);
        interpreter.Run();
        REQUIRE(Context().padding == u32(reason));
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto expected = Context();
        const auto expected_flags = state->host_cpu_flags;
        const auto expected_location = state->current_loc;
        for (bool baseline : {false, true}) {
            Context() = initial;
            state->host_cpu_flags = initial_flags;
            state->current_loc = initial_location;
            state->halt_reason = HaltReason::None;
            backend::x64::Translator translator(&block, baseline ? backend::x64::HostFeatures{}
                                                               : backend::x64::HostFeatures::Detect());
            translator.Translate();
            translator.getCode<HaltReason (*)(backend::State*)>()(state);
            REQUIRE(std::memcmp(&Context(), &expected, sizeof(expected)) == 0);
            REQUIRE(state->host_cpu_flags == expected_flags);
            REQUIRE(state->current_loc.Value() == expected_location.Value());
        }
#endif
    }
};
constexpr std::array<u32, 8> kOperations{CRC32B, CRC32H, CRC32W, CRC32X, CRC32CB, CRC32CH, CRC32CW, CRC32CX};
}

TEST_CASE("ARM64 CRC32 and CRC32C match normal polynomial division for all widths and aliases") {
    constexpr std::array<u64, 9> data_values{0, 1, 0x80, 0xFF, 0x8000, 0xFFFF, 0x80000000, 0xFFFFFFFF, 0xFEDCBA9876543210};
    constexpr std::array<u32, 4> accumulators{0, 1, 0x80000000, 0xFFFFFFFF};
    // Rd distinct, Rd==Rn, Rd==Rm, all same, Rn==Rm, WZR seed,
    // WZR/XZR data, WZR destination and all WZR.
    constexpr std::array<std::array<u32, 3>, 9> registers{{{0,1,2},{1,1,2},{2,1,2},{1,1,1},
        {0,1,1},{0,31,2},{0,1,31},{31,1,2},{31,31,31}}};
    for (u32 index = 0; index < kOperations.size(); ++index) {
        const u32 width = 8u << (index % 4);
        const bool castagnoli = index >= 4;
        for (u64 data : data_values) for (u32 accumulator : accumulators) for (auto regs : registers) {
            const auto [rd, rn, rm] = regs;
            INFO("op=" << kOperations[index] << " data=" << data << " seed=" << accumulator
                 << " rd=" << rd << " rn=" << rn << " rm=" << rm);
            Machine machine;
            if (rn != 31) Gpr(machine.Context(), rn) = 0xC0FFEE5500000000 | accumulator;
            if (rm != 31) Gpr(machine.Context(), rm) = width == 64 ? data : 0xDEADBEEF00000000 | u32(data);
            const auto before = machine.Context();
            const u32 expected = Reference(u32(machine.Register(rn)), machine.Register(rm), width, castagnoli);
            machine.Run({kOperations[index] | (rm << 16) | (rn << 5) | rd});
            if (rd != 31) REQUIRE(Gpr(machine.Context(), rd) == u64(expected));
            for (u32 reg = 0; reg < 31; ++reg) if (reg != rd) REQUIRE(Gpr(machine.Context(), reg) == Gpr(before, reg));
            REQUIRE(machine.Context().sp == before.sp);
            REQUIRE(machine.Context().fpcr == before.fpcr);
            REQUIRE(machine.Context().fpsr == before.fpsr);
            REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
        }
    }
}

TEST_CASE("ARM64 CRC32 byte data ignores every upper bit and covers all byte values") {
    for (bool castagnoli : {false, true}) for (u32 byte = 0; byte < 256; ++byte) {
        Machine machine;
        machine.Context().r[1] = 0xBADC0DE012345678;
        machine.Context().r[2] = 0xDEADBEEFFF00FF00 | byte;
        const auto expected = Reference(0x12345678, byte, 8, castagnoli);
        machine.Run({(castagnoli ? CRC32CB : CRC32B) | (2u << 16) | (1u << 5)});
        REQUIRE(machine.Context().r[0] == u64(expected));
    }
}

TEST_CASE("ARM64 CRC32 standard check values agree across byte and wide instruction chains") {
    // Standard reflected CRC check values include initial/final inversion;
    // ARM instructions themselves apply neither inversion.
    constexpr char message[] = "123456789";
    for (bool castagnoli : {false, true}) for (u32 bytes : {1u, 2u, 4u, 8u}) {
        Machine machine;
        machine.Context().r[0] = 0xDEADBEEFFFFFFFFF;
        std::vector<u32> code;
        u32 offset = 0, source = 1;
        while (offset < 9) {
            const u32 count = offset + bytes <= 9 ? bytes : 1;
            u64 data = 0;
            for (u32 i = 0; i < count; ++i) data |= u64(u8(message[offset + i])) << (8 * i);
            machine.Context().r[source] = data;
            const u32 width_index = count == 8 ? 3 : count == 4 ? 2 : count == 2 ? 1 : 0;
            code.push_back(kOperations[(castagnoli ? 4 : 0) + width_index] | (source << 16));
            offset += count;
            ++source;
        }
        machine.Run(code);
        REQUIRE(machine.Context().r[0] == u64((castagnoli ? 0xE3069283u : 0xCBF43926u) ^ 0xFFFFFFFFu));
        REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    }
}

TEST_CASE("ARM64 CRC32 rejects reserved sf width combinations without architectural mutation") {
    for (u32 op : kOperations) {
        Machine machine;
        const auto before = machine.Context();
        machine.Run({(op ^ 0x80000000u) | (2u << 16) | (1u << 5)}, swift::arm64::InterruptReason::ILL_CODE);
        for (u32 reg = 0; reg < 31; ++reg) REQUIRE(Gpr(machine.Context(), reg) == Gpr(before, reg));
        REQUIRE(machine.Context().sp == before.sp);
        REQUIRE(machine.state->current_loc.Value() == 0x1000);
        REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
    }
}
